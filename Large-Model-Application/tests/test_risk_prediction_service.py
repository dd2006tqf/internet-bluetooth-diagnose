"""Route-level service test for L3 risk prediction (路线图 ③ 后端).

Verifies the composition path: baseline row → history provider → predictor,
with an injected clock so the whole pipeline stays deterministic.
"""

from __future__ import annotations

import pytest
from sqlalchemy import create_engine
from sqlalchemy.pool import StaticPool

from industrial_ops_agent.network_assurance.risk_service import RiskPredictionService
from industrial_ops_agent.network_assurance.wireless_contracts import (
    InsufficientPrediction,
    RiskWindow,
    SubjectType,
    WindowEstimationStatus,
)
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import (
    Base,
    NetworkDeviceBaselineHistoryRecord,
    NetworkDeviceBaselineRecord,
)
from industrial_ops_agent.persistence.tenant import TenantContext

HOUR_MS = 3_600_000
BASE = 1_700_000_000_000
ASSET = "radxa-cubie-a7a"
DEVICE = "AA:BB:CC:DD:EE:01"
NOW_MS = BASE + 120 * HOUR_MS  # 注入的"当前时刻"


@pytest.fixture
def test_db() -> Database:
    engine = create_engine(
        "sqlite:///:memory:",
        connect_args={"check_same_thread": False},
        poolclass=StaticPool,
    )
    Base.metadata.create_all(engine)
    return Database.from_engine(engine)


@pytest.fixture
def tenant_context() -> TenantContext:
    return TenantContext(tenant_id="tenant-alpha", subject_id="operator-1")


def _seed_baseline(
    test_db: Database, *, device: str = DEVICE, baseline_id: str = "nbli-risk"
) -> None:
    from sqlalchemy.orm import Session

    with Session(test_db.engine) as session:  # type: ignore[attr-defined]
        session.add(
            NetworkDeviceBaselineRecord(
                baseline_id=baseline_id,
                tenant_id="tenant-alpha",
                asset_id=ASSET,
                site_id="site-1",
                gateway_id="gw-1",
                hci_index=0,
                protocol="BLUETOOTH",
                address_type="LE_RANDOM",
                device_address=device,
                baseline_rssi_dbm=-70,
                baseline_sample_count=12,
                state="STABLE",
                first_seen_ms=BASE,
                last_seen_ms=NOW_MS,
            )
        )
        # 96h 内 5 个点：-60 → -70 的慢性漂移（相对退化 10dB）
        for i, value in enumerate([-60, -62.5, -65, -67.5, -70]):
            session.add(
                NetworkDeviceBaselineHistoryRecord(
                    history_id=f"nblh-{i}",
                    tenant_id="tenant-alpha",
                    baseline_id=baseline_id,
                    asset_id=ASSET,
                    device_address=device,
                    observed_at_ms=BASE + i * 24 * HOUR_MS,
                    baseline_rssi_dbm=int(value),
                    baseline_sample_count=12,
                    state="STABLE",
                )
            )
        session.commit()


def test_prediction_returns_risk_window_for_drifting_device(
    test_db: Database, tenant_context: TenantContext
) -> None:
    _seed_baseline(test_db)
    service = RiskPredictionService(test_db)

    result = service.predict_device(
        tenant_context, ASSET, DEVICE, horizon_hours=168, now_ms=NOW_MS
    )

    assert isinstance(result, RiskWindow)
    assert result.subject_type is SubjectType.DEVICE
    assert result.subject_id == DEVICE
    # 5 天持续漂移 + 数据不充分（无丢包/抖动历史）→ 风险高、置信度低
    assert result.risk_level.value in {"HIGH", "MEDIUM"}
    assert result.prediction_confidence is not None
    # 丢包组件缺失 → 有风险判断但时间窗口不可估（三态之二）
    assert result.window_estimation_status is WindowEstimationStatus.UNAVAILABLE
    assert result.window_earliest_hours is None
    assert result.forecast_horizon_hours == 168
    # baseline 轨迹必须出现在证据里（不是 current-minus-current 差值）
    assert any(t.metric == "baseline_rssi_dbm" for t in result.trend_evidence)
    assert result.model_version.startswith("riskwindow-v1+sha256:")


def test_prediction_without_baseline_row_is_insufficient(
    test_db: Database, tenant_context: TenantContext
) -> None:
    service = RiskPredictionService(test_db)
    result = service.predict_device(
        tenant_context, ASSET, "FF:FF:FF:FF:FF:FF", horizon_hours=168, now_ms=NOW_MS
    )
    assert isinstance(result, InsufficientPrediction)
    # 如实列出缺失，不编造任何风险判断
    assert result.missing_requirements
    assert "基线" in "".join(result.missing_requirements)
    assert not hasattr(result, "risk_level")


def test_prediction_is_deterministic_with_injected_clock(
    test_db: Database, tenant_context: TenantContext
) -> None:
    _seed_baseline(test_db)
    service = RiskPredictionService(test_db)

    first = service.predict_device(
        tenant_context, ASSET, DEVICE, horizon_hours=168, now_ms=NOW_MS
    )
    second = service.predict_device(
        tenant_context, ASSET, DEVICE, horizon_hours=168, now_ms=NOW_MS
    )
    assert first == second

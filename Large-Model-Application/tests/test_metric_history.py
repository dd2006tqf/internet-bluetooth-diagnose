"""B3 tests for MetricHistoryProvider — seed/carry-forward & evidence extraction.

The change-log semantics under test:

    load_history(start, end) = latest sample <= start (seed, staleness-bounded)
                             + all changes in (start, end]

Without the seed, a Day2→Day5 window would only see Day3/Day5 changes and lose
the fact that the window *started* at -60 — the exact failure mode called out
in the plan.
"""

from __future__ import annotations

from datetime import UTC, datetime, timedelta

import pytest
from sqlalchemy import create_engine
from sqlalchemy.pool import StaticPool

from industrial_ops_agent.network_assurance.metric_history import (
    MetricHistoryProvider,
)
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import (
    Base,
    NetworkDeviceBaselineHistoryRecord,
    NetworkDeviceBaselineRecord,
    NetworkSnapshotRecord,
)
from industrial_ops_agent.persistence.tenant import TenantContext

HOUR_MS = 3_600_000
BASE = 1_700_000_000_000  # 窗口起点（固定时钟，测试确定性）

BASELINE_ID = "nbli-test01"
ASSET = "radxa-cubie-a7a"


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


def _add_history_row(
    session,
    *,
    history_id: str,
    observed_at_ms: int,
    value: int | None,
    count: int = 12,
    state: str = "STABLE",
) -> None:
    session.add(
        NetworkDeviceBaselineHistoryRecord(
            history_id=history_id,
            tenant_id="tenant-alpha",
            baseline_id=BASELINE_ID,
            asset_id=ASSET,
            device_address="AA:01",
            observed_at_ms=observed_at_ms,
            baseline_rssi_dbm=value,
            baseline_sample_count=count,
            state=state,
        )
    )


def test_change_log_seeds_window_start_state(
    test_db: Database, tenant_context: TenantContext
) -> None:
    """窗口起点状态必须由 seed 补齐（否则"Day2 起点仍是 -60"会丢）。"""
    from sqlalchemy.orm import Session

    with Session(test_db.engine) as session:  # type: ignore[attr-defined]
        _add_history_row(session, history_id="h0", observed_at_ms=BASE, value=-60)
        _add_history_row(
            session, history_id="h1", observed_at_ms=BASE + 72 * HOUR_MS, value=-62
        )
        _add_history_row(
            session, history_id="h2", observed_at_ms=BASE + 120 * HOUR_MS, value=-64
        )
        session.commit()

    provider = MetricHistoryProvider(test_db)
    samples = provider.load_baseline_trajectory(
        tenant_context,
        BASELINE_ID,
        start_ms=BASE + 24 * HOUR_MS,  # 窗口从 Day1 起
        end_ms=BASE + 120 * HOUR_MS,
    )

    # 3 条而不是 2 条：seed(h0, <=start) + 窗口内变更(h1, h2)
    assert [s.value for s in samples] == [-60.0, -62.0, -64.0]
    assert [s.ts_ms for s in samples] == [
        BASE,
        BASE + 72 * HOUR_MS,
        BASE + 120 * HOUR_MS,
    ]
    # 种子与变更都来自真实行（evidence 可回链）
    assert samples[0].evidence_id == "h0"
    assert samples[2].evidence_id == "h2"
    # 成熟度透传（STABLE + count>=10）
    assert all(s.mature for s in samples)


def test_stale_seed_is_excluded(test_db: Database, tenant_context: TenantContext) -> None:
    """超过陈旧期（默认 336h）的种子不得进入序列——它不能代表窗口起点状态。"""
    from sqlalchemy.orm import Session

    with Session(test_db.engine) as session:  # type: ignore[attr-defined]
        _add_history_row(
            session, history_id="ancient", observed_at_ms=BASE - 400 * HOUR_MS, value=-58
        )
        _add_history_row(
            session, history_id="fresh", observed_at_ms=BASE + 10 * HOUR_MS, value=-64
        )
        session.commit()

    provider = MetricHistoryProvider(test_db)
    samples = provider.load_baseline_trajectory(
        tenant_context, BASELINE_ID, start_ms=BASE, end_ms=BASE + 120 * HOUR_MS
    )
    # 400h 前的种子被陈旧期剔除；只剩窗口内的变更
    assert [s.evidence_id for s in samples] == ["fresh"]


def test_immature_rows_are_marked_not_mature(
    test_db: Database, tenant_context: TenantContext
) -> None:
    from sqlalchemy.orm import Session

    with Session(test_db.engine) as session:  # type: ignore[attr-defined]
        _add_history_row(session, history_id="learning", observed_at_ms=BASE,
                         value=-60, count=3, state="LEARNING")
        _add_history_row(session, history_id="stable", observed_at_ms=BASE + 48 * HOUR_MS,
                         value=-61, count=12, state="STABLE")
        session.commit()

    provider = MetricHistoryProvider(test_db)
    samples = provider.load_baseline_trajectory(
        tenant_context, BASELINE_ID, start_ms=BASE + 1 * HOUR_MS, end_ms=BASE + 48 * HOUR_MS
    )
    # 种子（LEARNING）不成熟 → 不能当预测锚点；窗口内变更成熟
    assert [(s.evidence_id, s.mature) for s in samples] == [
        ("learning", False),
        ("stable", True),
    ]


def test_current_state_row_can_seed_pre_migration_device(
    test_db: Database, tenant_context: TenantContext
) -> None:
    """迁移前就存在的设备没有历史行：当前态行 updated_at 作种子。"""
    from sqlalchemy.orm import Session

    now = datetime.now(UTC)
    with Session(test_db.engine) as session:  # type: ignore[attr-defined]
        session.add(
            NetworkDeviceBaselineRecord(
                baseline_id=BASELINE_ID,
                tenant_id="tenant-alpha",
                asset_id=ASSET,
                site_id="site-1",
                gateway_id="gw-1",
                hci_index=0,
                protocol="BLUETOOTH",
                address_type="LE_RANDOM",
                device_address="AA:01",
                baseline_rssi_dbm=-66,
                baseline_sample_count=12,
                state="STABLE",
            )
        )
        session.commit()
        session.refresh(session.get(NetworkDeviceBaselineRecord, BASELINE_ID))

    provider = MetricHistoryProvider(test_db)
    start = int(now.timestamp() * 1000) + HOUR_MS  # 窗口起点在"现在"之后一点
    samples = provider.load_baseline_trajectory(
        tenant_context, BASELINE_ID, start_ms=start, end_ms=start + 48 * HOUR_MS
    )
    assert len(samples) == 1
    assert samples[0].value == -66.0
    assert samples[0].evidence_id.startswith("current:")
    assert samples[0].mature is True


def test_snapshot_evidence_extraction_with_unit_scale(
    test_db: Database, tenant_context: TenantContext
) -> None:
    from sqlalchemy.orm import Session

    captured = datetime(2026, 10, 4, 6, 0, tzinfo=UTC)
    experience = {
        "network_health": {
            "reliability": {
                "state": "BAD",
                "evidence": [
                    {"metric": "wifi_loss_rate", "value": 4.2, "detail": "%"},
                    {"metric": "other", "value": 99, "detail": "x"},
                ],
            }
        }
    }
    with Session(test_db.engine) as session:  # type: ignore[attr-defined]
        session.add(
            NetworkSnapshotRecord(
                snapshot_id="snap-1",
                tenant_id="tenant-alpha",
                asset_id=ASSET,
                sequence_id=7,
                network_epoch=1,
                config_generation=1,
                captured_at=captured,
                received_at=captured,
                overall_state="DEGRADED",
                display_score=55,
                experience_json=experience,
            )
        )
        session.commit()

    provider = MetricHistoryProvider(test_db)
    start = int((captured - timedelta(hours=1)).timestamp() * 1000)
    end = int((captured + timedelta(hours=1)).timestamp() * 1000)

    loss = provider.load_snapshot_evidence(
        tenant_context, ASSET, "wifi_loss_rate", start, end, unit_scale=0.01
    )
    assert len(loss) == 1
    assert loss[0].value == pytest.approx(0.042)  # 4.2% → 0.042 ratio
    assert loss[0].evidence_id == "snap-1"
    assert loss[0].ts_ms == int(captured.timestamp() * 1000)

    # 指标缺失 → 空序列（不当 0）
    jitter = provider.load_snapshot_evidence(
        tenant_context, ASSET, "median_jitter_ms", start, end
    )
    assert jitter == []

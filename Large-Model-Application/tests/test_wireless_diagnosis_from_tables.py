"""End-to-end tests for the default (table-backed) diagnosis path.

Before Phase 4a the diagnosis service could only work with an injected
``bundle_loader``; the default path raised ``WirelessIncidentNotFound``
unconditionally, so the API returned 404 for every real incident. These tests
drive the real chain instead:

    signed uplink shape -> ingest_wireless_batch (four fact tables)
                        -> WirelessDiagnosisService.get_diagnosis (no loader)
                        -> classify_incident result

They also pin the assembly rules that the rules engine depends on:
qualifying events follow the incident->event links, the window keeps *all*
events (so degradation evidence is visible), and the environment window
reaches the classifier.
"""

from __future__ import annotations

import pytest
from sqlalchemy import create_engine
from sqlalchemy.pool import StaticPool

from industrial_ops_agent.network_assurance.service import NetworkAssuranceService
from industrial_ops_agent.network_assurance.wireless_contracts import (
    BaselineView,
    ConfidenceLevel,
    EnvironmentWindowView,
    ObservedPattern,
    WirelessEventUplinkBatch,
    WirelessEventView,
)
from industrial_ops_agent.network_assurance.wireless_diagnosis import (
    WirelessDiagnosisService,
    WirelessIncidentNotFound,
)
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import Base
from industrial_ops_agent.persistence.tenant import TenantContext

DEVICE = "radxa-cubie-a7a"
KEY_ID = "weaknet-edge-telemetry-v1"
INCIDENT = "site-1_1700000001000"


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
    return TenantContext(tenant_id="tenant-alpha", subject_id="edge-automation")


def _event(
    event_id: str,
    device: str,
    ts_ms: int,
    *,
    event_type: str = "LINK_DISCONNECTED",
    rssi: int | None = -70,
    reason: str = "CONNECTION_TIMEOUT",
) -> WirelessEventView:
    return WirelessEventView(
        event_id=event_id,
        ts_ms=ts_ms,
        site_id="site-1",
        gateway_id="gw-1",
        protocol="BLUETOOTH",
        device_address=device,
        address_type="LE_RANDOM",
        hci_index=0,
        event_type=event_type,
        rssi_at_event_dbm=rssi,
        reason=reason,
        source="KERNEL_MGMT",
    )


def _uplink(*, wifi_anomaly: bool) -> WirelessEventUplinkBatch:
    events = [
        _event("e1", "AA:01", 1_700_000_001_000),
        _event("e2", "AA:02", 1_700_000_001_500),
    ]
    return WirelessEventUplinkBatch.model_validate(
        {
            "device_id": DEVICE,
            "watermark_ms": 1_700_000_001_500,
            "events": events,
            "incidents": [
                {
                    "incident_id": INCIDENT,
                    "site_id": "site-1",
                    "gateway_id": "gw-1",
                    "started_at_ms": 1_700_000_001_000,
                    "last_event_ms": 1_700_000_001_500,
                    "affected_devices": 2,
                    "state": "OPEN",
                    "evidence_event_ids": ["e1", "e2"],
                }
            ],
            "baselines": [
                BaselineView(
                    site_id="site-1",
                    gateway_id="gw-1",
                    device_address="AA:01",
                    baseline_rssi_dbm=-64,
                    baseline_sample_count=20,
                    state="STABLE",
                ).model_dump()
            ],
            "env_window": EnvironmentWindowView(
                available=True,
                link_type="WIFI_2_4G",
                wifi_anomaly=wifi_anomaly,
                from_ms=1_700_000_000_000,
                to_ms=1_700_000_060_000,
            ).model_dump(),
        }
    )


def test_diagnosis_runs_from_uplinked_tables(test_db: Database, tenant_context: TenantContext):
    service = NetworkAssuranceService(database=test_db)
    service.ingest_wireless_batch(tenant_context, _uplink(wifi_anomaly=True), key_id=KEY_ID)

    diagnosis = WirelessDiagnosisService(test_db)
    # 关键：不注入 bundle_loader —— 走真实默认路径
    result = diagnosis.get_diagnosis(tenant_context, INCIDENT)

    assert result.incident_id == INCIDENT
    assert result.presentation.llm_used is False  # GET 绝不调用模型
    # 两事件跨度 500ms 且无前置劣化 + Wi-Fi 异常 → 亚秒同步断开 + 共存干扰
    assert result.canonical.observed_pattern == (ObservedPattern.SUB_SECOND_SIMULTANEOUS_DISCONNECT)
    assert result.canonical.hypothesis.value == "COEXISTENCE_RF_INTERFERENCE"
    assert result.canonical.confidence == ConfidenceLevel.HIGH
    assert set(result.canonical.evidence_ids) == {"e1", "e2"}


def test_hypothesis_follows_environment_evidence(test_db: Database, tenant_context: TenantContext):
    service = NetworkAssuranceService(database=test_db)
    service.ingest_wireless_batch(tenant_context, _uplink(wifi_anomaly=False), key_id=KEY_ID)

    diagnosis = WirelessDiagnosisService(test_db)
    result = diagnosis.get_diagnosis(tenant_context, INCIDENT)

    # 无 Wi-Fi 异常证据 → 不得宣称共存干扰（环境窗口确实参与了判定）
    assert result.canonical.hypothesis.value == "LOCAL_ADAPTER_OR_HOST_STALL"
    assert result.canonical.confidence == ConfidenceLevel.MEDIUM


def test_get_is_idempotent_and_cached(test_db: Database, tenant_context: TenantContext):
    service = NetworkAssuranceService(database=test_db)
    service.ingest_wireless_batch(tenant_context, _uplink(wifi_anomaly=True), key_id=KEY_ID)

    diagnosis = WirelessDiagnosisService(test_db)
    first = diagnosis.get_diagnosis(tenant_context, INCIDENT)
    second = diagnosis.get_diagnosis(tenant_context, INCIDENT)
    assert second.diagnosis_id == first.diagnosis_id


def test_unknown_incident_still_not_found(test_db: Database, tenant_context: TenantContext):
    diagnosis = WirelessDiagnosisService(test_db)
    with pytest.raises(WirelessIncidentNotFound):
        diagnosis.get_diagnosis(tenant_context, "site-1_does_not_exist")


def test_other_tenant_cannot_read_incident(test_db: Database, tenant_context: TenantContext):
    service = NetworkAssuranceService(database=test_db)
    service.ingest_wireless_batch(tenant_context, _uplink(wifi_anomaly=True), key_id=KEY_ID)

    other = TenantContext(tenant_id="tenant-beta", subject_id="edge-automation")
    diagnosis = WirelessDiagnosisService(test_db)
    with pytest.raises(WirelessIncidentNotFound):
        diagnosis.get_diagnosis(other, INCIDENT)


def test_missing_links_fall_back_to_window_of_disconnects(
    test_db: Database, tenant_context: TenantContext
):
    """回链缺失（旧数据/未上行）时必须确定性回退，而不是给出空证据。"""

    batch = _uplink(wifi_anomaly=False)
    incident = batch.incidents[0].model_copy(update={"evidence_event_ids": []})
    batch = batch.model_copy(update={"incidents": [incident]})

    service = NetworkAssuranceService(database=test_db)
    service.ingest_wireless_batch(tenant_context, batch, key_id=KEY_ID)

    result = WirelessDiagnosisService(test_db).get_diagnosis(tenant_context, INCIDENT)
    assert set(result.canonical.evidence_ids) == {"e1", "e2"}


def test_qualifying_events_stay_pure_when_other_gateway_events_are_in_window(
    test_db: Database, tenant_context: TenantContext
):
    """多网关窗口并入时，qualifying 仍只认本事故的证据回链。

    同一 site 的另一台网关在同一时间窗内也上报了事件——它应该出现在
    window_events（规则引擎需要看见"这片区域当时还有什么"），但**绝不能**
    进入 qualifying_events，否则"哪几条事件支撑这起事故"就失真了。
    """

    batch = WirelessEventUplinkBatch.model_validate(
        {
            "device_id": DEVICE,
            "watermark_ms": 1_700_000_001_500,
            "events": [
                _event("e1", "AA:01", 1_700_000_001_000).model_dump(),
                _event("e2", "AA:02", 1_700_000_001_500).model_dump(),
                # 另一台网关对同一现场的观测（同 site、同窗口、不同 gateway）
                _event("other-gw-e1", "AA:09", 1_700_000_001_200)
                .model_copy(update={"gateway_id": "gw-2"})
                .model_dump(),
            ],
            "incidents": [
                {
                    "incident_id": INCIDENT,
                    "site_id": "site-1",
                    "gateway_id": "gw-1",
                    "started_at_ms": 1_700_000_001_000,
                    "last_event_ms": 1_700_000_001_500,
                    "affected_devices": 2,
                    "state": "OPEN",
                    # 只回链本网关的两条事件
                    "evidence_event_ids": ["e1", "e2"],
                }
            ],
        }
    )
    service = NetworkAssuranceService(database=test_db)
    service.ingest_wireless_batch(tenant_context, batch, key_id=KEY_ID)

    diagnosis = WirelessDiagnosisService(test_db)
    result = diagnosis.get_diagnosis(tenant_context, INCIDENT)

    # qualifying 纯净：只有回链里的两条
    assert set(result.canonical.evidence_ids) == {"e1", "e2"}

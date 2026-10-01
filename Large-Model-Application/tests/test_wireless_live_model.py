"""Live integration test against upstream model gateway (Phase 4b).

Marked with @pytest.mark.live so it is excluded from ordinary PR CI runs.
Runs only when IOAP_MODEL_GATEWAY_API_KEY is provisioned.
"""

import os
import pytest
from industrial_ops_agent.guardrails.network_causal import NetworkCausalGuardrail, WirelessCausalContext
from industrial_ops_agent.network_assurance.wireless_contracts import (
    ConfidenceLevel,
    DiagnosisHypothesis,
    EnvironmentWindowView,
    IncidentEvidenceBundle,
    IncidentView,
    ObservedPattern,
    WirelessEventView,
)
from industrial_ops_agent.network_assurance.wireless_diagnosis import (
    WirelessDiagnosisService,
)
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import Base
from industrial_ops_agent.persistence.tenant import TenantContext
from sqlalchemy import create_engine
from sqlalchemy.pool import StaticPool


@pytest.mark.live
def test_live_model_diagnosis_end_to_end():
    api_key = os.environ.get("IOAP_MODEL_GATEWAY_API_KEY")
    if not api_key:
        pytest.skip("No IOAP_MODEL_GATEWAY_API_KEY configured; skipping live model test.")

    engine = create_engine(
        "sqlite:///:memory:",
        connect_args={"check_same_thread": False},
        poolclass=StaticPool,
    )
    Base.metadata.create_all(engine)
    db = Database.from_engine(engine)
    tenant_context = TenantContext(tenant_id="tenant-live", subject_id="operator-live")

    service = WirelessDiagnosisService(db)

    # 构造真实的同频竞争事故证据包
    events = [
        WirelessEventView(
            event_id="live_e1",
            ts_ms=1759200000000,
            site_id="site-factory",
            gateway_id="gw-factory-1",
            protocol="BLUETOOTH",
            device_address="AA:BB:CC:11:22:33",
            address_type="LE_RANDOM",
            hci_index=0,
            event_type="LINK_DISCONNECTED",
            rssi_at_event_dbm=-75,
            reason="CONNECTION_TIMEOUT",
            source="KERNEL_MGMT",
        ),
        WirelessEventView(
            event_id="live_e2",
            ts_ms=1759200001000,
            site_id="site-factory",
            gateway_id="gw-factory-1",
            protocol="BLUETOOTH",
            device_address="AA:BB:CC:44:55:66",
            address_type="LE_RANDOM",
            hci_index=0,
            event_type="LINK_DISCONNECTED",
            rssi_at_event_dbm=-78,
            reason="CONNECTION_TIMEOUT",
            source="KERNEL_MGMT",
        ),
    ]
    bundle = IncidentEvidenceBundle(
        incident=IncidentView(
            incident_id="sitinc_live_1",
            site_id="site-factory",
            gateway_id="gw-factory-1",
            started_at_ms=1759200000000,
            last_event_ms=1759200001000,
            resolved_at_ms=1759200061000,
            affected_devices=2,
            state="RESOLVED",
        ),
        qualifying_events=events,
        window_events=events,
        baselines={},
        environment=EnvironmentWindowView(
            available=True,
            link_type="WIFI_2_4G",
            wifi_anomaly=True,
            coexistence_warning=True,
        ),
    )

    res = service.diagnose_incident(tenant_context, "sitinc_live_1", bundle_loader=lambda _: bundle)

    assert res.canonical.observed_pattern == ObservedPattern.MULTI_DEVICE_CONCURRENT_ANOMALY
    assert res.canonical.hypothesis == DiagnosisHypothesis.COEXISTENCE_RF_INTERFERENCE
    assert res.canonical.confidence == ConfidenceLevel.HIGH

    # 验证模型润色产出或安全回退
    assert len(res.presentation.diagnosis_report) > 0
    assert len(res.presentation.recommendations) > 0
    if res.presentation.llm_used:
        assert res.guardrail_status == "ALLOWED"
        assert res.presentation.llm_model_name == "deepseek-v4-pro-0813"

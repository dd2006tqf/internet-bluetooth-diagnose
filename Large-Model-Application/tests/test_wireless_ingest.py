"""Unit tests for the Phase 4a wireless-fact uplink (service level).

The edge treats replay as a normal path (its watermark only advances after
an acknowledgement), so every test below exercises both the first-ingest and
the identical-replay behaviour the board's exporter depends on:

  1. first batch registers the gateway asset and accepts every group;
  2. identical replay counts duplicates without duplicating rows;
  3. incident lifecycle advance (OPEN -> RESOLVED) counts accepted;
  4. baseline profile upsert accounting;
  5. env window refresh keeps a single row;
  6. cross-tenant device claim fails closed (409 semantics upstream).
"""

from __future__ import annotations

import pytest
from pydantic import ValidationError
from sqlalchemy import create_engine
from sqlalchemy.orm import Session
from sqlalchemy.pool import StaticPool

from industrial_ops_agent.network_assurance.service import (
    NetworkAssuranceConflict,
    NetworkAssuranceService,
)
from industrial_ops_agent.network_assurance.wireless_contracts import (
    BaselineView,
    EnvironmentWindowView,
    IncidentView,
    WirelessEventUplinkBatch,
    WirelessEventView,
)
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import (
    Base,
    NetworkAssetRecord,
    NetworkDeviceBaselineRecord,
    NetworkEnvWindowRecord,
    NetworkSiteIncidentRecord,
    NetworkWirelessEventRecord,
)
from industrial_ops_agent.persistence.tenant import TenantContext

DEVICE = "radxa-cubie-a7a"
KEY_ID = "weaknet-edge-telemetry-v1"


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


def _event(event_id: str = "evt-1", ts_ms: int = 1_700_000_001_000) -> WirelessEventView:
    return WirelessEventView(
        event_id=event_id,
        ts_ms=ts_ms,
        site_id="site-1",
        gateway_id="gw-1",
        protocol="BLUETOOTH",
        device_address="AA:01",
        address_type="LE_RANDOM",
        hci_index=0,
        event_type="LINK_DISCONNECTED",
        rssi_at_event_dbm=-70,
        reason="CONNECTION_TIMEOUT",
        source="KERNEL_MGMT",
    )


def _incident(
    incident_id: str = "site-1_1700000001000",
    *,
    state: str = "OPEN",
    resolved_at_ms: int | None = None,
    last_event_ms: int = 1_700_000_001_500,
) -> IncidentView:
    return IncidentView(
        incident_id=incident_id,
        site_id="site-1",
        gateway_id="gw-1",
        started_at_ms=1_700_000_001_000,
        last_event_ms=last_event_ms,
        resolved_at_ms=resolved_at_ms,
        affected_devices=2,
        state=state,
        evidence_event_ids=["evt-1"],
    )


def _baseline(
    device_address: str = "AA:01", *, count: int = 12, state: str = "STABLE"
) -> BaselineView:
    return BaselineView(
        site_id="site-1",
        gateway_id="gw-1",
        hci_index=0,
        protocol="BLUETOOTH",
        address_type="LE_RANDOM",
        device_address=device_address,
        baseline_rssi_dbm=-65,
        min_seen_rssi_dbm=-78,
        max_seen_rssi_dbm=-59,
        baseline_sample_count=count,
        state=state,
    )


def _env(*, wifi_anomaly: bool = True) -> EnvironmentWindowView:
    return EnvironmentWindowView(
        available=True,
        link_type="WIFI_2_4G",
        wifi_anomaly=wifi_anomaly,
        coexistence_warning=False,
        from_ms=1_700_000_000_000,
        to_ms=1_700_000_060_000,
        snapshots=[{"ts_ms": 1_700_000_000_000, "rssi_dbm": -66}],
    )


def _batch(**overrides) -> WirelessEventUplinkBatch:
    payload: dict[str, object] = {
        "device_id": DEVICE,
        "watermark_ms": 1_700_000_001_500,
        "events": [_event()],
        "incidents": [_incident()],
        "baselines": [_baseline()],
        "env_window": _env(),
    }
    payload.update(overrides)
    return WirelessEventUplinkBatch.model_validate(payload)


def _count_rows(db: Database, model) -> int:
    with Session(db.engine) as session:  # type: ignore[attr-defined]
        return len(session.query(model).all())


def test_first_batch_accepts_and_registers_asset(test_db: Database, tenant_context: TenantContext):
    service = NetworkAssuranceService(database=test_db)
    result = service.ingest_wireless_batch(tenant_context, _batch(), key_id=KEY_ID)

    assert result.accepted.events == 1
    assert result.accepted.incidents == 1
    assert result.accepted.baselines == 1
    assert result.duplicates.events == 0
    assert result.duplicates.incidents == 0
    assert result.duplicates.baselines == 0

    with Session(test_db.engine) as session:  # type: ignore[attr-defined]
        asset = session.get(NetworkAssetRecord, DEVICE)
        assert asset is not None
        assert asset.tenant_id == "tenant-alpha"
        assert asset.signing_key_id == KEY_ID

        event = session.get(NetworkWirelessEventRecord, "evt-1")
        assert event is not None
        assert event.asset_id == DEVICE
        assert event.rssi_at_event_dbm == -70
        assert event.raw_reason_code == 0

        incident = session.get(NetworkSiteIncidentRecord, "site-1_1700000001000")
        assert incident is not None
        assert incident.state == "OPEN"
        assert incident.evidence_event_ids_json == ["evt-1"]


def test_identical_replay_counts_duplicates_without_duplicate_rows(
    test_db: Database, tenant_context: TenantContext
):
    service = NetworkAssuranceService(database=test_db)
    service.ingest_wireless_batch(tenant_context, _batch(), key_id=KEY_ID)
    replay = service.ingest_wireless_batch(tenant_context, _batch(), key_id=KEY_ID)

    # 事件是不可变事实：重放全部计为 duplicate，行数不增
    assert replay.accepted.events == 0
    assert replay.duplicates.events == 1
    # 载荷未变化的事故与基线同样计为 duplicate
    assert replay.accepted.incidents == 0
    assert replay.duplicates.incidents == 1
    assert replay.accepted.baselines == 0
    assert replay.duplicates.baselines == 1

    assert _count_rows(test_db, NetworkWirelessEventRecord) == 1
    assert _count_rows(test_db, NetworkSiteIncidentRecord) == 1
    assert _count_rows(test_db, NetworkDeviceBaselineRecord) == 1
    assert _count_rows(test_db, NetworkEnvWindowRecord) == 1


def test_incident_lifecycle_advance_counts_accepted_and_persists_state(
    test_db: Database, tenant_context: TenantContext
):
    service = NetworkAssuranceService(database=test_db)
    service.ingest_wireless_batch(tenant_context, _batch(), key_id=KEY_ID)

    resolved = _batch(
        incidents=[_incident(state="RESOLVED", resolved_at_ms=1_700_000_061_000)],
        events=[],
        baselines=[],
        env_window=None,
    )
    advance = service.ingest_wireless_batch(tenant_context, resolved, key_id=KEY_ID)
    assert advance.accepted.incidents == 1
    assert advance.duplicates.incidents == 0

    with Session(test_db.engine) as session:  # type: ignore[attr-defined]
        row = session.get(NetworkSiteIncidentRecord, "site-1_1700000001000")
        assert row is not None
        assert row.state == "RESOLVED"
        assert row.resolved_at_ms == 1_700_000_061_000

    # 再次重放同一 RESOLVED 载荷 → 全部 duplicate
    again = service.ingest_wireless_batch(tenant_context, resolved, key_id=KEY_ID)
    assert again.accepted.incidents == 0
    assert again.duplicates.incidents == 1


def test_incident_window_never_rewinds_on_late_replay(
    test_db: Database, tenant_context: TenantContext
):
    service = NetworkAssuranceService(database=test_db)
    service.ingest_wireless_batch(tenant_context, _batch(), key_id=KEY_ID)

    late = _incident(last_event_ms=1_700_000_001_100)
    # 迟到的旧载荷：last_event_ms 早于已存窗口 —— 时间右边界不得回退
    service.ingest_wireless_batch(
        tenant_context,
        _batch(incidents=[late], events=[], baselines=[], env_window=None),
        key_id=KEY_ID,
    )

    with Session(test_db.engine) as session:  # type: ignore[attr-defined]
        row = session.get(NetworkSiteIncidentRecord, "site-1_1700000001000")
        assert row is not None
        assert row.last_event_ms == 1_700_000_001_500


def test_baseline_profile_upsert_counts_change_as_accepted(
    test_db: Database, tenant_context: TenantContext
):
    service = NetworkAssuranceService(database=test_db)
    service.ingest_wireless_batch(tenant_context, _batch(), key_id=KEY_ID)

    updated = _batch(
        baselines=[_baseline(count=40, state="DEGRADED")], events=[], incidents=[], env_window=None
    )
    result = service.ingest_wireless_batch(tenant_context, updated, key_id=KEY_ID)
    assert result.accepted.baselines == 1
    assert result.duplicates.baselines == 0

    with Session(test_db.engine) as session:  # type: ignore[attr-defined]
        rows = session.query(NetworkDeviceBaselineRecord).all()
        assert len(rows) == 1
        assert rows[0].baseline_sample_count == 40
        assert rows[0].state == "DEGRADED"


def test_env_window_refresh_keeps_single_row(test_db: Database, tenant_context: TenantContext):
    service = NetworkAssuranceService(database=test_db)
    service.ingest_wireless_batch(tenant_context, _batch(), key_id=KEY_ID)
    service.ingest_wireless_batch(
        tenant_context,
        _batch(events=[], incidents=[], baselines=[], env_window=_env(wifi_anomaly=False)),
        key_id=KEY_ID,
    )

    with Session(test_db.engine) as session:  # type: ignore[attr-defined]
        rows = session.query(NetworkEnvWindowRecord).all()
        assert len(rows) == 1
        assert rows[0].wifi_anomaly is False


def test_cross_tenant_device_claim_fails_closed(test_db: Database, tenant_context: TenantContext):
    service = NetworkAssuranceService(database=test_db)
    service.ingest_wireless_batch(tenant_context, _batch(), key_id=KEY_ID)

    other_tenant = TenantContext(tenant_id="tenant-beta", subject_id="edge-automation")
    with pytest.raises(NetworkAssuranceConflict):
        service.ingest_wireless_batch(other_tenant, _batch(), key_id=KEY_ID)

    # 冲突方不得写入任何行
    assert _count_rows(test_db, NetworkWirelessEventRecord) == 1


def test_empty_and_oversized_batches_are_rejected_by_contract():
    with pytest.raises(ValidationError):
        WirelessEventUplinkBatch.model_validate({"device_id": DEVICE, "watermark_ms": 1})

    with pytest.raises(ValidationError):
        WirelessEventUplinkBatch.model_validate(
            {
                "device_id": DEVICE,
                "watermark_ms": 1,
                "events": [_event(f"evt-{i}") for i in range(1001)],
            }
        )

"""S7 tests: an uplinked site incident opens an incident *draft*.

The integration deliberately stops at a draft. Creating a formal incident
requires a confirmed evidence bundle and at least one clean media object
(``application/incidents.py:1433``), and a work order additionally requires an
approved proposal. There is no "system subject confirmed the evidence"
channel, so auto-opening a work order would mean fabricating that evidence —
which is exactly the kind of shortcut this platform exists to prevent. The
draft is therefore a proposal in the operator's queue, and the human decision
that follows keeps the platform's existing gates intact.

These tests pin: default-on behaviour, the idempotency key that makes replays
safe, the off switch, and that a failing draft path never breaks fact ingest.
"""

from __future__ import annotations

from typing import Any

import pytest
from sqlalchemy import create_engine, select
from sqlalchemy.orm import Session
from sqlalchemy.pool import StaticPool

from industrial_ops_agent.network_assurance.service import NetworkAssuranceService
from industrial_ops_agent.network_assurance.wireless_contracts import (
    WirelessEventUplinkBatch,
    WirelessEventView,
)
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import Base, IncidentDraftRecord
from industrial_ops_agent.persistence.tenant import TenantContext

DEVICE = "radxa-cubie-a7a"
KEY_ID = "weaknet-edge-telemetry-v1"
INCIDENT = "site-1_1700000001000"


class _RecordingDraftService:
    """Stand-in for the platform's IncidentDraftService.

    Records the calls so the test can assert the contract the integration
    depends on (asset binding, idempotency key) without pulling in the whole
    authorization stack.
    """

    def __init__(self) -> None:
        self.calls: list[dict[str, Any]] = []

    def create(self, identity: Any, **kwargs: Any) -> Any:
        self.calls.append({"identity": identity, **kwargs})
        return kwargs["idempotency_key"]


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


def _uplink(*, incident_state: str = "OPEN") -> WirelessEventUplinkBatch:
    return WirelessEventUplinkBatch.model_validate(
        {
            "device_id": DEVICE,
            "watermark_ms": 1_700_000_001_500,
            "events": [
                WirelessEventView(
                    event_id="e1",
                    ts_ms=1_700_000_001_000,
                    site_id="site-1",
                    gateway_id="gw-1",
                    protocol="BLUETOOTH",
                    device_address="AA:01",
                    address_type="LE_RANDOM",
                    hci_index=0,
                    event_type="LINK_DISCONNECTED",
                    reason="CONNECTION_TIMEOUT",
                    source="KERNEL_MGMT",
                ).model_dump()
            ],
            "incidents": [
                {
                    "incident_id": INCIDENT,
                    "site_id": "site-1",
                    "gateway_id": "gw-1",
                    "started_at_ms": 1_700_000_001_000,
                    "last_event_ms": 1_700_000_001_500,
                    "affected_devices": 2,
                    "state": incident_state,
                    "evidence_event_ids": ["e1"],
                }
            ],
        }
    )


def _drafts(db: Database) -> list[IncidentDraftRecord]:
    with Session(db.engine) as session:  # type: ignore[attr-defined]
        return list(session.scalars(select(IncidentDraftRecord)).all())


def test_draft_opened_for_new_active_incident(test_db: Database, tenant_context: TenantContext):
    recorder = _RecordingDraftService()
    service = NetworkAssuranceService(
        database=test_db,
        incident_draft_service_factory=lambda _ctx: (recorder, "identity"),
    )

    service.ingest_wireless_batch(tenant_context, _uplink(), key_id=KEY_ID)

    assert len(recorder.calls) == 1
    call = recorder.calls[0]
    assert call["asset_id"] == DEVICE  # 草稿挂在网关资产上（S6 桥提供身份）
    assert call["idempotency_key"] == f"weaknet-incident-draft:{INCIDENT}"
    assert INCIDENT in call["description"]


def test_replay_does_not_open_a_second_draft(test_db: Database, tenant_context: TenantContext):
    recorder = _RecordingDraftService()
    service = NetworkAssuranceService(
        database=test_db,
        incident_draft_service_factory=lambda _ctx: (recorder, "identity"),
    )

    service.ingest_wireless_batch(tenant_context, _uplink(), key_id=KEY_ID)
    service.ingest_wireless_batch(tenant_context, _uplink(), key_id=KEY_ID)

    # 第二次上行是重复事故（accepted=0），不得再触发草稿调用
    assert len(recorder.calls) == 1


def test_resolved_incident_does_not_open_draft(test_db: Database, tenant_context: TenantContext):
    recorder = _RecordingDraftService()
    service = NetworkAssuranceService(
        database=test_db,
        incident_draft_service_factory=lambda _ctx: (recorder, "identity"),
    )

    service.ingest_wireless_batch(
        tenant_context, _uplink(incident_state="RESOLVED"), key_id=KEY_ID
    )
    assert recorder.calls == []


def test_switch_off_disables_drafting(test_db: Database, tenant_context: TenantContext):
    recorder = _RecordingDraftService()
    service = NetworkAssuranceService(
        database=test_db,
        auto_incident_draft_enabled=False,
        incident_draft_service_factory=lambda _ctx: (recorder, "identity"),
    )

    service.ingest_wireless_batch(tenant_context, _uplink(), key_id=KEY_ID)
    assert recorder.calls == []


def test_draft_failure_never_breaks_ingest(test_db: Database, tenant_context: TenantContext):
    class _Exploding:
        def create(self, *args: Any, **kwargs: Any) -> Any:
            raise RuntimeError("draft backend down")

    service = NetworkAssuranceService(
        database=test_db,
        incident_draft_service_factory=lambda _ctx: (_Exploding(), "identity"),
    )

    # 事实入库必须成功——草稿是下游增强，不能拖垮上游
    result = service.ingest_wireless_batch(tenant_context, _uplink(), key_id=KEY_ID)
    assert result.accepted.incidents == 1
    assert result.accepted.events == 1

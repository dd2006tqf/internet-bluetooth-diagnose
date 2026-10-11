"""Unit and integration tests for T4 (Edge Hypothesis ingestion) and T5 (Council integration & Gate B).

Verifies:
1. L1/L2 Strict Persistence Separation:
   - `network_snapshots` stores pure L1 telemetry facts without any natural-language AI hypothesis fields.
   - `network_edge_hypotheses` stores the L2 advisory hypothesis linked to `(network_epoch, sequence_id)`.
2. Idempotency:
   - Replaying the same batch does not duplicate rows in `network_edge_hypotheses`.
3. Council Read-Only Advisory & TTL Filtering:
   - Active (unexpired) edge hypotheses are injected into `CouncilInput.edge_hypotheses` and `edge_advisory_context`.
   - Expired edge hypotheses (`age > ttl_seconds`) are automatically excluded.
4. No Circular Evidence & Governed Execution:
   - `hypothesis_id` is NEVER included in `allowed_binding_ref_ids()`; a proposal attempting to bind `hypothesis_id` as an `L1_FACT` or `CANONICAL_DIAGNOSIS` is rejected by `validate_proposals`.
   - Ingesting an edge hypothesis NEVER creates a row in `network_pending_actions`.
"""

from __future__ import annotations

from datetime import UTC, datetime, timedelta

import pytest
from sqlalchemy import create_engine, select
from sqlalchemy.pool import StaticPool

from industrial_ops_agent.network_assurance.contracts import (
    EdgeAiHypothesis,
    NetworkDeviceTelemetry,
    NetworkExperienceSnapshot,
    NetworkSleResult,
    NetworkTelemetryBatch,
)
from industrial_ops_agent.network_assurance.council_contracts import (
    ActionProposal,
    ActionProposalKind,
    SourceBinding,
    SourceBindingKind,
)
from industrial_ops_agent.network_assurance.network_council import (
    NetworkCouncilRunner,
    build_specialist_payload,
    validate_proposals,
)
from industrial_ops_agent.network_assurance.network_council_service import (
    NetworkCouncilService,
)
from industrial_ops_agent.network_assurance.service import NetworkAssuranceService
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import (
    Base,
    NetworkEdgeHypothesisRecord,
    NetworkPendingActionRecord,
    NetworkSiteIncidentRecord,
    NetworkSnapshotRecord,
)
from industrial_ops_agent.persistence.tenant import TenantContext


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
def tenant() -> TenantContext:
    return TenantContext(tenant_id="tenant-gate-b", subject_id="edge-tester")


def _make_sle(state: str = "GOOD") -> NetworkSleResult:
    return NetworkSleResult(
        state=state,
        coverage="FULL_FOR_PROFILE",
        applicability="APPLICABLE",
        capability_level_negative=False,
        reason="ok",
        evidence=[],
    )


def _make_sample_snapshot() -> NetworkExperienceSnapshot:
    return NetworkExperienceSnapshot(
        interface="wlan0",
        assessment_profile="INTERNET_ACCESS",
        overall_state="DEGRADED",
        overall_coverage="FULL_FOR_PROFILE",
        display_score=55,
        network_health={
            "ip_reachability": _make_sle("GOOD"),
            "responsiveness": _make_sle("BAD"),
            "reliability": _make_sle("GOOD"),
            "rf_health": _make_sle("GOOD"),
        },
        service_health={
            "dns": _make_sle("GOOD"),
            "tcp_connect": _make_sle("GOOD"),
            "tcp_rtt": _make_sle("DEGRADED"),
            "http_access": _make_sle("GOOD"),
            "captive_portal": _make_sle("GOOD"),
            "active_dns": _make_sle("GOOD"),
            "active_tcp": _make_sle("GOOD"),
            "active_https": _make_sle("GOOD"),
            "active_portal": _make_sle("GOOD"),
        },
        warnings=[],
        primary_issue="High latency or excessive jitter",
        link_type="WIFI_2_4G",
    )


def test_t4_ingest_separates_l1_snapshot_and_l2_hypothesis_with_idempotency(
    test_db: Database, tenant: TenantContext
) -> None:
    svc = NetworkAssuranceService(database=test_db)
    now = datetime.now(UTC)

    batch = NetworkTelemetryBatch(
        snapshots=[
            NetworkDeviceTelemetry(
                device_id="radxa-cubie-a7a",
                sequence_id=10,
                network_epoch=2,
                config_generation=1,
                captured_at=now,
                snapshot=_make_sample_snapshot(),
            )
        ],
        edge_ai_hypotheses=[
            EdgeAiHypothesis(
                hypothesis_id="eh-gateb00001",
                device_id="radxa-cubie-a7a",
                network_epoch=2,
                sequence_id=10,
                interface="wlan0",
                model_id="qwen2.5-0.5b-instruct-q4_k_m",
                summary="疑似蜂窝热点空口调度排队或高并发连接竞争。",
                evidence_refs=["snap-radxa-cubie-a7a-2-10"],
                recommended_actions=["PROBE_LOCAL_GATEWAY"],
                created_at=now,
                ttl_seconds=300,
                cloud_eligible=True,
            )
        ],
    )

    # First ingest
    res1 = svc.ingest_batch(tenant, batch, key_id="key-1", received_at=now)
    assert res1.accepted == 1
    assert res1.duplicates == 0

    # Replay same batch (idempotency check)
    res2 = svc.ingest_batch(tenant, batch, key_id="key-1", received_at=now)
    assert res2.accepted == 0
    assert res2.duplicates == 1

    with test_db.transaction(tenant) as session:
        snaps = session.scalars(select(NetworkSnapshotRecord)).all()
        hyps = session.scalars(select(NetworkEdgeHypothesisRecord)).all()
        pending = session.scalars(select(NetworkPendingActionRecord)).all()

        assert len(snaps) == 1
        # Verify L1 snapshot is completely unpolluted by L2 AI text
        assert "edge_ai" not in str(snaps[0].experience_json)

        assert len(hyps) == 1
        assert hyps[0].hypothesis_id == "eh-gateb00001"
        assert hyps[0].sequence_id == 10
        assert hyps[0].status == "UNVERIFIED"
        assert hyps[0].recommended_actions_json == ["PROBE_LOCAL_GATEWAY"]

        # Verify governed execution: ingesting an AI hypothesis NEVER queues an action
        assert len(pending) == 0


def test_t5_council_filters_ttl_and_forbids_circular_evidence_binding(
    test_db: Database, tenant: TenantContext
) -> None:
    svc = NetworkAssuranceService(database=test_db)
    now = datetime.now(UTC)

    # Ingest 1 expired hypothesis (created 600s ago, ttl=300s) and 1 active hypothesis
    batch = NetworkTelemetryBatch(
        snapshots=[
            NetworkDeviceTelemetry(
                device_id="radxa-cubie-a7a",
                sequence_id=20,
                network_epoch=2,
                config_generation=1,
                captured_at=now - timedelta(seconds=600),
                snapshot=_make_sample_snapshot(),
            ),
            NetworkDeviceTelemetry(
                device_id="radxa-cubie-a7a",
                sequence_id=21,
                network_epoch=2,
                config_generation=1,
                captured_at=now,
                snapshot=_make_sample_snapshot(),
            ),
        ],
        edge_ai_hypotheses=[
            EdgeAiHypothesis(
                hypothesis_id="eh-expired001",
                device_id="radxa-cubie-a7a",
                network_epoch=2,
                sequence_id=20,
                interface="wlan0",
                model_id="qwen2.5-0.5b-instruct-q4_k_m",
                summary="过期假设不应被 Council 采用",
                evidence_refs=["snap-radxa-cubie-a7a-2-20"],
                recommended_actions=["PROBE_LOCAL_GATEWAY"],
                created_at=now - timedelta(seconds=600),
                ttl_seconds=300,
                cloud_eligible=True,
            ),
            EdgeAiHypothesis(
                hypothesis_id="eh-active0002",
                device_id="radxa-cubie-a7a",
                network_epoch=2,
                sequence_id=21,
                interface="wlan0",
                model_id="qwen2.5-0.5b-instruct-q4_k_m",
                summary="有效期内假设供 Council 参考",
                evidence_refs=["snap-radxa-cubie-a7a-2-21"],
                recommended_actions=["PROBE_LOCAL_GATEWAY"],
                created_at=now - timedelta(seconds=20),
                ttl_seconds=300,
                cloud_eligible=True,
            ),
        ],
    )
    svc.ingest_batch(tenant, batch, key_id="key-1", received_at=now)

    # Create a site incident for the gateway
    now_ms = int(now.timestamp() * 1000)
    with test_db.transaction(tenant) as session:
        session.add(
            NetworkSiteIncidentRecord(
                incident_id="inc-gateb-01",
                tenant_id=tenant.tenant_id,
                asset_id="radxa-cubie-a7a",
                gateway_id="radxa-cubie-a7a",
                started_at_ms=now_ms,
                last_event_ms=now_ms,
                affected_devices=3,
                state="OPEN",
                evidence_event_ids_json=[],
            )
        )

    council_svc = NetworkCouncilService(
        database=test_db,
        runner=None,  # type: ignore[arg-type]
    )
    req_input = council_svc.build_request_from_tables(tenant, "inc-gateb-01")
    council_input = council_svc.build_input(req_input)

    # 1. Verify TTL filtering: only eh-active0002 is present
    assert len(council_input.edge_hypotheses) == 1
    assert council_input.edge_hypotheses[0].hypothesis_id == "eh-active0002"

    # 2. Verify specialist payload receives edge_advisory_context
    from industrial_ops_agent.network_assurance.council_contracts import CouncilRole
    payload = build_specialist_payload(council_input, CouncilRole.RF_SPECTRUM)
    assert len(payload["edge_advisory_context"]) == 1
    assert payload["edge_advisory_context"][0]["hypothesis_id"] == "eh-active0002"

    # 3. Verify anti-circular-evidence guard: eh-active0002 is NOT in allowed_binding_ref_ids
    allowed_refs = council_input.allowed_binding_ref_ids()
    for ref_set in allowed_refs.values():
        assert "eh-active0002" not in ref_set

    # If a coordinator tries to cite eh-active0002 as an L1_FACT or CANONICAL_DIAGNOSIS, E1 rejects it
    bad_proposal = ActionProposal(
        kind=ActionProposalKind.CONFIG_CHANGE,
        config_key="rtt.interval_ms",
        config_value="5000",
        rationale="Citing edge AI hypothesis directly as fact",
        source_bindings=[
            SourceBinding(kind=SourceBindingKind.L1_FACT, ref_id="eh-active0002")
        ],
    )
    violations = validate_proposals([bad_proposal], council_input)
    assert any("eh-active0002" in v for v in violations)

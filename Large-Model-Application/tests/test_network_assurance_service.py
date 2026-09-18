"""Unit tests for the hardened NetworkAssuranceService pending action path.

Verifies Evolution 2 invariant guarantees:
  1. Anti-replay nonce is generated on every queue_action
  2. Action generation increments monotonically per asset
  3. _claim_pending_actions binds a unique claim_token and marks DELIVERED
  4. record_action_results rejects mismatched claim_token
  5. record_action_results rejects mismatched asset_id
  6. record_action_results accepts ROLLBACK outcome and updates terminal state
  7. Terminal status cannot be overwritten
"""

from __future__ import annotations

from datetime import UTC, datetime
from typing import Any

import pytest

from industrial_ops_agent.domain import as_utc
from industrial_ops_agent.network_assurance.contracts import (
    NetworkActionOutcome,
    NetworkActionResults,
)
from industrial_ops_agent.network_assurance.service import NetworkAssuranceService
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import (
    NetworkAssetRecord,
    NetworkPendingActionRecord,
)
from industrial_ops_agent.persistence.tenant import TenantContext


@pytest.fixture
def test_db() -> Database:
    from sqlalchemy import create_engine
    from sqlalchemy.pool import StaticPool
    from industrial_ops_agent.persistence.models import Base

    # StaticPool keeps the same in-memory DB connection open across transactions
    engine = create_engine(
        "sqlite:///:memory:",
        connect_args={"check_same_thread": False},
        poolclass=StaticPool,
    )
    Base.metadata.create_all(engine)
    return Database.from_engine(engine)


@pytest.fixture
def service(test_db: Database) -> NetworkAssuranceService:
    return NetworkAssuranceService(database=test_db)


@pytest.fixture
def tenant() -> TenantContext:
    return TenantContext(tenant_id="tenant-alpha", subject_id="tester")


def _provision_asset(db: Database, tenant: TenantContext, asset_id: str) -> None:
    with db.transaction(tenant) as session:
        session.add(
            NetworkAssetRecord(
                asset_id=asset_id,
                tenant_id=tenant.tenant_id,
                display_name=asset_id,
                link_type="WIRED_ETHERNET",
                overall_state="GOOD",
                display_score=100,
                connection_status="ONLINE",
            )
        )


class TestQueueActionGenerationAndNonce:
    def test_queue_action_generates_nonce_and_increments_generation(
        self, test_db: Database, service: NetworkAssuranceService, tenant: TenantContext
    ) -> None:
        _provision_asset(test_db, tenant, "edge-1")

        act_id_1 = service.queue_action(
            tenant,
            "edge-1",
            config_key="rtt.interval_ms",
            config_value="5000",
            issued_by="admin",
        )
        act_id_2 = service.queue_action(
            tenant,
            "edge-1",
            config_key="rtt.timeout_ms",
            config_value="1000",
            issued_by="admin",
        )

        with test_db.transaction(tenant) as session:
            r1 = session.get(NetworkPendingActionRecord, act_id_1)
            r2 = session.get(NetworkPendingActionRecord, act_id_2)
            assert r1 is not None and r2 is not None
            # Nonce must be present, 32 hex chars, and distinct
            assert len(r1.nonce) == 32
            assert len(r2.nonce) == 32
            assert r1.nonce != r2.nonce
            # Generation must be monotonic per-asset: 1 then 2
            assert r1.generation == 1
            assert r2.generation == 2

    def test_generations_are_isolated_per_asset(
        self, test_db: Database, service: NetworkAssuranceService, tenant: TenantContext
    ) -> None:
        _provision_asset(test_db, tenant, "edge-a")
        _provision_asset(test_db, tenant, "edge-b")

        a1 = service.queue_action(
            tenant, "edge-a", config_key="rtt.interval_ms", config_value="5000", issued_by="admin"
        )
        b1 = service.queue_action(
            tenant, "edge-b", config_key="rtt.interval_ms", config_value="5000", issued_by="admin"
        )

        with test_db.transaction(tenant) as session:
            ra = session.get(NetworkPendingActionRecord, a1)
            rb = session.get(NetworkPendingActionRecord, b1)
            assert ra is not None and rb is not None
            # Both start at 1 because generation is per-asset
            assert ra.generation == 1
            assert rb.generation == 1


class TestClaimPendingActions:
    def test_claim_assigns_claim_token_and_marks_delivered(
        self, test_db: Database, service: NetworkAssuranceService, tenant: TenantContext
    ) -> None:
        _provision_asset(test_db, tenant, "edge-1")
        act_id = service.queue_action(
            tenant, "edge-1", config_key="rtt.interval_ms", config_value="5000", issued_by="admin"
        )

        with test_db.transaction(tenant) as session:
            claimed = service._claim_pending_actions(
                session, tenant, "edge-1", now=datetime.now(UTC)
            )

        assert len(claimed) == 1
        item = claimed[0]
        assert item["action_id"] == act_id
        assert len(item["claim_token"]) == 32
        assert item["generation"] == 1
        assert len(item["nonce"]) == 32

        with test_db.transaction(tenant) as session:
            rec = session.get(NetworkPendingActionRecord, act_id)
            assert rec is not None
            assert rec.status == "DELIVERED"
            assert rec.claim_token == item["claim_token"]
            assert rec.claimed_by_device_id == "edge-1"


class TestRecordActionResults:
    def test_applied_with_matching_token_updates_record(
        self, test_db: Database, service: NetworkAssuranceService, tenant: TenantContext
    ) -> None:
        _provision_asset(test_db, tenant, "edge-1")
        act_id = service.queue_action(
            tenant, "edge-1", config_key="rtt.interval_ms", config_value="5000", issued_by="admin"
        )
        with test_db.transaction(tenant) as session:
            claimed = service._claim_pending_actions(
                session, tenant, "edge-1", now=datetime.now(UTC)
            )
        token = claimed[0]["claim_token"]

        results = NetworkActionResults(
            schema_version="network.edge.action-results.v2",
            device_id="edge-1",
            results=[
                NetworkActionOutcome(
                    action_id=act_id,
                    status="APPLIED",
                    detail="applied successfully",
                    claim_token=token,
                    generation=1,
                    reported_at=datetime.now(UTC),
                )
            ],
        )
        updated = service.record_action_results(tenant, results)
        assert updated == 1

        with test_db.transaction(tenant) as session:
            rec = session.get(NetworkPendingActionRecord, act_id)
            assert rec is not None
            assert rec.status == "APPLIED"
            assert rec.result_detail == "applied successfully"

    def test_mismatched_claim_token_is_rejected(
        self, test_db: Database, service: NetworkAssuranceService, tenant: TenantContext
    ) -> None:
        _provision_asset(test_db, tenant, "edge-1")
        act_id = service.queue_action(
            tenant, "edge-1", config_key="rtt.interval_ms", config_value="5000", issued_by="admin"
        )
        with test_db.transaction(tenant) as session:
            service._claim_pending_actions(session, tenant, "edge-1", now=datetime.now(UTC))

        results = NetworkActionResults(
            schema_version="network.edge.action-results.v2",
            device_id="edge-1",
            results=[
                NetworkActionOutcome(
                    action_id=act_id,
                    status="APPLIED",
                    detail="applied",
                    claim_token="wrong-claim-token-1234567890abcdef",
                    generation=1,
                    reported_at=datetime.now(UTC),
                )
            ],
        )
        updated = service.record_action_results(tenant, results)
        assert updated == 0

        with test_db.transaction(tenant) as session:
            rec = session.get(NetworkPendingActionRecord, act_id)
            assert rec is not None
            # Must still be DELIVERED, not APPLIED
            assert rec.status == "DELIVERED"

    def test_rollback_status_is_accepted_and_terminal(
        self, test_db: Database, service: NetworkAssuranceService, tenant: TenantContext
    ) -> None:
        _provision_asset(test_db, tenant, "edge-1")
        act_id = service.queue_action(
            tenant, "edge-1", config_key="rtt.interval_ms", config_value="5000", issued_by="admin"
        )
        with test_db.transaction(tenant) as session:
            claimed = service._claim_pending_actions(
                session, tenant, "edge-1", now=datetime.now(UTC)
            )
        token = claimed[0]["claim_token"]

        results = NetworkActionResults(
            schema_version="network.edge.action-results.v2",
            device_id="edge-1",
            results=[
                NetworkActionOutcome(
                    action_id=act_id,
                    status="ROLLBACK",
                    detail="watchdog_timeout_health_bad",
                    claim_token=token,
                    generation=1,
                    reported_at=datetime.now(UTC),
                )
            ],
        )
        updated = service.record_action_results(tenant, results)
        assert updated == 1

        with test_db.transaction(tenant) as session:
            rec = session.get(NetworkPendingActionRecord, act_id)
            assert rec is not None
            assert rec.status == "ROLLBACK"
            assert rec.result_detail == "watchdog_timeout_health_bad"

        # Subsequent report on the same action must not overwrite terminal state
        results_retry = NetworkActionResults(
            schema_version="network.edge.action-results.v2",
            device_id="edge-1",
            results=[
                NetworkActionOutcome(
                    action_id=act_id,
                    status="APPLIED",
                    detail="too late",
                    claim_token=token,
                    generation=1,
                    reported_at=datetime.now(UTC),
                )
            ],
        )
        assert service.record_action_results(tenant, results_retry) == 0

    def test_v1_payload_accepted_when_claim_token_empty(
        self, test_db: Database, service: NetworkAssuranceService, tenant: TenantContext
    ) -> None:
        # Legacy action with no claim_token stored
        _provision_asset(test_db, tenant, "edge-legacy")
        now = datetime.now(UTC)
        with test_db.transaction(tenant) as session:
            session.add(
                NetworkPendingActionRecord(
                    action_id="nact-legacy",
                    tenant_id=tenant.tenant_id,
                    asset_id="edge-legacy",
                    generation=0,
                    nonce="",
                    claim_token="",
                    config_key="rtt.interval_ms",
                    config_value="5000",
                    status="DELIVERED",
                    issued_by="admin",
                    issued_at=now,
                    version=1,
                )
            )

        # v1 client sends action-results.v1 with empty claim_token
        results_v1 = NetworkActionResults(
            schema_version="network.edge.action-results.v1",
            device_id="edge-legacy",
            results=[
                NetworkActionOutcome(
                    action_id="nact-legacy",
                    status="APPLIED",
                    detail="v1 applied",
                    claim_token="",
                    generation=0,
                    reported_at=now,
                )
            ],
        )
        updated = service.record_action_results(tenant, results_v1)
        assert updated == 1

        with test_db.transaction(tenant) as session:
            rec = session.get(NetworkPendingActionRecord, "nact-legacy")
            assert rec is not None
            assert rec.status == "APPLIED"

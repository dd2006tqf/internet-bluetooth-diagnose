"""N5 service-level tests: 幂等复用 / force / fail-closed 落库（路线图 ④）。

These exercise the service against an in-memory SQLite DB with a Mock model call,
covering the behaviours that matter operationally:

  - 同一输入连点三次 → 只有一次模型调用、同一条记录；
  - 语义变化（诊断 / 知识案例 / 网关 catalog 版本）→ 新指纹 → 触发新会商；
  - force=true → 新 attempt（复用同一 council_id）；
  - Runner 失败 → 记录 FAILED 且 proposals 为空（绝不保存半合法结果）；
  - 落库记录不含任何批准/执行语义列。
"""

from __future__ import annotations

from typing import Any

import pytest
from sqlalchemy import create_engine, inspect
from sqlalchemy.pool import StaticPool

from industrial_ops_agent.network_assurance.council_contracts import (
    CouncilFailure,
    CouncilRole,
    CouncilStatus,
)
from industrial_ops_agent.network_assurance.edge_action_catalog import (
    TOP_LEVEL_CATALOG_VERSION,
)
from industrial_ops_agent.network_assurance.network_council import NetworkCouncilRunner
from industrial_ops_agent.network_assurance.network_council_service import (
    CouncilRequestInput,
    NetworkCouncilService,
)
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import Base
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
def context() -> TenantContext:
    return TenantContext(tenant_id="tenant-alpha", subject_id="operator-1")


def _opinion() -> dict[str, Any]:
    return {
        "observations": ["事实 A"],
        "referenced_fact_ids": ["e1"],
        "recommendation_direction": "建议方向",
    }


def _proposal(diagnosis_digest: str = "sha256:diag-1") -> dict[str, Any]:
    """协调官的提案会绑定**本次输入**的诊断摘要（真实模型正是被这样告知的）。"""
    return {
        "kind": "ACTION_ID",
        "action_id": "CHECK_RESOLVER_CONFIG",
        "rationale": "现有诊断指向解析链路，先确认本机解析器配置",
        "proposed_preconditions": [],
        "source_bindings": [{"kind": "CANONICAL_DIAGNOSIS", "ref_id": diagnosis_digest}],
    }


class CountingComplete:
    """记录调用次数的 Mock 模型。"""

    def __init__(self) -> None:
        self.calls: list[CouncilRole] = []
        self.coordinator_payloads: list[dict[str, Any]] = []

    def __call__(self, role: CouncilRole, payload: dict[str, Any]) -> dict[str, Any]:
        self.calls.append(role)
        if role is CouncilRole.COORDINATOR:
            self.coordinator_payloads.append(payload)
            digest = str(payload.get("canonical_diagnosis_digest", "sha256:diag-1"))
            return {"proposals": [_proposal(digest)]}
        return _opinion()

    @property
    def total(self) -> int:
        return len(self.calls)


def _request(**overrides: Any) -> CouncilRequestInput:
    base: dict[str, Any] = {
        "incident_id": "sitinc_demo_1",
        "canonical_diagnosis_digest": "sha256:diag-1",
        "prediction_results": [],
        "device_facts": ["e1", "e2"],
        "rf_facts": ["rf-1"],
        "kernel_facts": ["k-1"],
        "diagnosis_evidence_ids": ["diag-evidence-1"],
        "prediction_evidence_ids": [],
        "affected_assets": ["radxa-cubie-a7a"],
        "production_critical": False,
        "gateway_catalog_version": TOP_LEVEL_CATALOG_VERSION,
        "knowledge_cases": [],
        "retrieval_strategy_version": "rag-v1",
    }
    base.update(overrides)
    return CouncilRequestInput(**base)


# ---------------------------------------------------------------------------
# 幂等
# ---------------------------------------------------------------------------


def test_same_input_reuses_council_and_calls_model_once(
    test_db: Database, context: TenantContext
) -> None:
    complete = CountingComplete()
    service = NetworkCouncilService(test_db, NetworkCouncilRunner(complete=complete))

    first = service.request_council(context, _request())
    second = service.request_council(context, _request())
    third = service.request_council(context, _request())

    assert complete.total == 4  # 3 专家 + 1 协调官，只跑了一次会商
    assert first.council_id == second.council_id == third.council_id
    assert first.input_fingerprint == third.input_fingerprint
    assert second.status is CouncilStatus.REVIEW_PENDING
    assert len(third.proposals) == 1


def test_force_opens_new_attempt_under_same_council(
    test_db: Database, context: TenantContext
) -> None:
    complete = CountingComplete()
    service = NetworkCouncilService(test_db, NetworkCouncilRunner(complete=complete))

    first = service.request_council(context, _request())
    forced = service.request_council(context, _request(), force=True)

    assert complete.total == 8  # 跑了两次
    assert forced.council_id == first.council_id  # 同一事故的同一会商
    assert forced.version > first.version


def test_diagnosis_change_triggers_new_fingerprint(
    test_db: Database, context: TenantContext
) -> None:
    complete = CountingComplete()
    service = NetworkCouncilService(test_db, NetworkCouncilRunner(complete=complete))

    first = service.request_council(context, _request())
    changed = service.request_council(
        context, _request(canonical_diagnosis_digest="sha256:diag-2")
    )

    assert first.input_fingerprint != changed.input_fingerprint
    assert complete.total == 8  # 输入语义变了 → 必须重新会商
    assert changed.council_id != first.council_id


def test_knowledge_case_change_triggers_new_council(
    test_db: Database, context: TenantContext
) -> None:
    """知识库更新后必须重新会商，否则新知识永不参与。"""
    complete = CountingComplete()
    service = NetworkCouncilService(test_db, NetworkCouncilRunner(complete=complete))

    service.request_council(context, _request(knowledge_cases=[("case-A", "1")]))
    service.request_council(context, _request(knowledge_cases=[("case-A", "1")]))
    assert complete.total == 4  # 第二次命中缓存

    service.request_council(context, _request(knowledge_cases=[("case-D", "2")]))
    assert complete.total == 8  # 案例集合变化 → 新会商


def test_gateway_catalog_version_drift_changes_fingerprint(
    test_db: Database, context: TenantContext
) -> None:
    complete = CountingComplete()
    service = NetworkCouncilService(test_db, NetworkCouncilRunner(complete=complete))
    first = service.request_council(context, _request())
    drifted = service.request_council(
        context, _request(gateway_catalog_version="cat-old-firmware")
    )
    assert first.input_fingerprint != drifted.input_fingerprint


def test_knowledge_case_order_is_semantically_equivalent(
    test_db: Database, context: TenantContext
) -> None:
    complete = CountingComplete()
    service = NetworkCouncilService(test_db, NetworkCouncilRunner(complete=complete))
    a = service.request_council(
        context, _request(knowledge_cases=[("case-A", "1"), ("case-B", "1")])
    )
    b = service.request_council(
        context, _request(knowledge_cases=[("case-B", "1"), ("case-A", "1")])
    )
    assert complete.total == 4  # canonical sort → 同一输入
    assert a.council_id == b.council_id


# ---------------------------------------------------------------------------
# fail closed
# ---------------------------------------------------------------------------


def test_runner_failure_persists_failed_without_proposals(
    test_db: Database, context: TenantContext
) -> None:
    class FailingComplete:
        def __call__(self, role: CouncilRole, payload: dict[str, Any]) -> dict[str, Any]:
            raise RuntimeError("upstream unavailable")

    service = NetworkCouncilService(
        test_db, NetworkCouncilRunner(complete=FailingComplete())
    )
    with pytest.raises(CouncilFailure):
        service.request_council(context, _request())

    view = service.get_council(context, "sitinc_demo_1")
    assert view is not None
    assert view.status is CouncilStatus.FAILED
    assert view.proposals == []
    assert view.failure_code


def test_catalog_violation_is_failed_not_partially_saved(
    test_db: Database, context: TenantContext
) -> None:
    class IllegalComplete:
        def __call__(self, role: CouncilRole, payload: dict[str, Any]) -> dict[str, Any]:
            if role is CouncilRole.COORDINATOR:
                digest = str(payload.get("canonical_diagnosis_digest", "sha256:diag-1"))
                return {
                    "proposals": [
                        _proposal(digest),
                        {**_proposal(digest), "action_id": "REBOOT_FACTORY_NETWORK_STACK"},
                    ]
                }
            return _opinion()

    service = NetworkCouncilService(test_db, NetworkCouncilRunner(complete=IllegalComplete()))
    with pytest.raises(CouncilFailure):
        service.request_council(context, _request())

    view = service.get_council(context, "sitinc_demo_1")
    assert view is not None
    assert view.status is CouncilStatus.FAILED
    # 合法的那条也不保存：不静默过滤
    assert view.proposals == []


# ---------------------------------------------------------------------------
# 结构性约束：记录里没有批准/执行语义
# ---------------------------------------------------------------------------


def test_council_tables_have_no_approval_or_execution_columns(test_db: Database) -> None:
    inspector = inspect(test_db.engine)  # type: ignore[attr-defined]
    councils = {col["name"] for col in inspector.get_columns("network_councils")}
    for forbidden in ("approved_by", "approved_at", "execution_status", "queued_action_id"):
        assert forbidden not in councils
    contributions = {
        col["name"] for col in inspector.get_columns("network_council_contributions")
    }
    assert "approved_by" not in contributions


# ---------------------------------------------------------------------------
# 专家贡献可读（消解 network_council_contributions 只写不读）
# ---------------------------------------------------------------------------


def test_get_council_exposes_per_role_contributions(
    test_db: Database, context: TenantContext
) -> None:
    complete = CountingComplete()
    service = NetworkCouncilService(test_db, NetworkCouncilRunner(complete=complete))
    service.request_council(context, _request())

    view = service.get_council(context, "sitinc_demo_1")
    assert view is not None
    assert len(view.contributions) == 3  # 三位专家各一条，协调官不落贡献行
    assert {c.agent_role for c in view.contributions} == {
        "RF_SPECTRUM",
        "KERNEL_STACK",
        "OPS_SAFETY",
    }
    for contribution in view.contributions:
        assert contribution.council_id == view.council_id
        assert contribution.attempt_number == 1
        assert contribution.input_digest == "sha256:diag-1"
        assert contribution.output_digest.startswith("sha256:")
        assert contribution.prompt_bundle_hash
        assert contribution.output["observations"] == ["事实 A"]
        assert contribution.completed_at is not None


def test_failed_council_has_no_contributions(
    test_db: Database, context: TenantContext
) -> None:
    class FailingComplete:
        def __call__(self, role: CouncilRole, payload: dict[str, Any]) -> dict[str, Any]:
            raise RuntimeError("upstream unavailable")

    service = NetworkCouncilService(
        test_db, NetworkCouncilRunner(complete=FailingComplete())
    )
    with pytest.raises(CouncilFailure):
        service.request_council(context, _request())

    view = service.get_council(context, "sitinc_demo_1")
    assert view is not None
    assert view.contributions == []  # fail closed：失败会商不产出贡献行


# ---------------------------------------------------------------------------
# 网关自述目录版本 → 会商输入（接通 Policy 漂移检测的输入源）
# ---------------------------------------------------------------------------


def _seed_incident(
    db: Database, ctx: TenantContext, *, gateway_id: str = "radxa-cubie-a7a"
) -> str:
    """写一条最小 incident（builder 只读它与其证据回链）。"""

    from industrial_ops_agent.persistence.models import NetworkSiteIncidentRecord

    incident_id = "sitinc_catalog_probe_1"
    with db.transaction(ctx) as session:
        session.add(
            NetworkSiteIncidentRecord(
                incident_id=incident_id,
                tenant_id=ctx.tenant_id,
                asset_id=gateway_id,
                site_id=gateway_id,
                gateway_id=gateway_id,
                started_at_ms=1_700_000_000_000,
                last_event_ms=1_700_000_001_000,
                affected_devices=0,
                state="OPEN",
                evidence_event_ids_json=[],
            )
        )
    return incident_id


def _seed_catalog_version(
    db: Database, ctx: TenantContext, version: str, *, device_id: str = "radxa-cubie-a7a"
) -> None:
    """写网关自述版本（等价于板端上行携带 catalog_version 后的落库结果）。"""

    from datetime import UTC, datetime

    from industrial_ops_agent.persistence.models import (
        NetworkGatewayCatalogVersionRecord,
    )

    with db.transaction(ctx) as session:
        session.add(
            NetworkGatewayCatalogVersionRecord(
                tenant_id=ctx.tenant_id,
                device_id=device_id,
                catalog_version=version,
                reported_at=datetime.now(UTC),
            )
        )


def test_builder_uses_gateway_reported_catalog_version(
    test_db: Database, context: TenantContext
) -> None:
    """核心接通：网关自述版本必须流入会商输入（此前恒为默认值）。"""

    service = NetworkCouncilService(test_db, NetworkCouncilRunner(complete=CountingComplete()))
    incident_id = _seed_incident(test_db, context)
    _seed_catalog_version(test_db, context, "deadbeef0000")

    built = service.build_request_from_tables(context, incident_id)

    assert built.gateway_catalog_version == "deadbeef0000"


def test_builder_falls_back_to_cloud_catalog_when_gateway_silent(
    test_db: Database, context: TenantContext
) -> None:
    """未上报 → 回落云端目录。缺失不等于漂移，绝不阻断。"""

    service = NetworkCouncilService(test_db, NetworkCouncilRunner(complete=CountingComplete()))
    incident_id = _seed_incident(test_db, context)

    built = service.build_request_from_tables(context, incident_id)

    assert built.gateway_catalog_version == TOP_LEVEL_CATALOG_VERSION


def test_builder_explicit_argument_wins_over_reported(
    test_db: Database, context: TenantContext
) -> None:
    """显式传入（测试注入漂移）优先于自述值，保持既有测试契约。"""

    service = NetworkCouncilService(test_db, NetworkCouncilRunner(complete=CountingComplete()))
    incident_id = _seed_incident(test_db, context)
    _seed_catalog_version(test_db, context, "deadbeef0000")

    built = service.build_request_from_tables(
        context, incident_id, gateway_catalog_version="cat-injected"
    )

    assert built.gateway_catalog_version == "cat-injected"


def test_reported_drift_blocks_policy_end_to_end(
    test_db: Database, context: TenantContext
) -> None:
    """端到端：网关自述漂移版本 → Policy fail-closed（allowed=False、零审批）。"""

    from industrial_ops_agent.network_assurance.action_policy import decide_proposal
    from industrial_ops_agent.network_assurance.council_contracts import (
        ActionProposal,
        ActionProposalKind,
    )
    from industrial_ops_agent.network_assurance.network_action_approval import (
        proposal_digest as _digest,
    )

    service = NetworkCouncilService(test_db, NetworkCouncilRunner(complete=CountingComplete()))
    incident_id = _seed_incident(test_db, context)
    _seed_catalog_version(test_db, context, "deadbeef0000")

    built = service.build_request_from_tables(context, incident_id)
    proposal = ActionProposal(
        kind=ActionProposalKind.ACTION_ID,
        action_id="CHECK_RESOLVER_CONFIG",
        rationale="漂移场景下的提案",
        proposed_preconditions=[],
        # 契约要求提案必须带来源绑定（≥1）——漂移拒收发生在目录比对，
        # 与来源绑定无关，所以这里给一条最小合法绑定即可。
        source_bindings=[
            {"kind": "CANONICAL_DIAGNOSIS", "ref_id": "sha256:drift-probe"}
        ],
    )
    decision = decide_proposal(
        proposal,
        proposal_digest=_digest(proposal),
        target_gateway="radxa-cubie-a7a",
        production_critical=False,
        gateway_catalog_version=built.gateway_catalog_version,
    )

    assert decision.allowed is False
    assert decision.block_reason is not None
    assert "catalog_version_mismatch" in decision.block_reason

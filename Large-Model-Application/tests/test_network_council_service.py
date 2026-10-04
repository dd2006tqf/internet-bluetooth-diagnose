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

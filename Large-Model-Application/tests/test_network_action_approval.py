"""S2 tests: 提案身份 → Policy 裁决 → 人工审批 → 执行/运行手册（路线图 ⑤）。

覆盖用户验收矩阵的核心项：
- 所有 E1 提案（含 BLOCKED）都有稳定 proposal_id 与不可变快照；
- BLOCKED 不产生 ApprovalRequest（P0-2）；
- Approval 批准的是 decision（decision_id + digest + normalized_action），不是 LLM 原文；
- MANUAL 批准不伪装执行（P0-3）；runbook 从批准快照生成；
- Execute 只收幂等键、载荷来自批准快照（P1-1）、同键幂等、异键冲突；
- Guard 真复用（异常类即 approval.models 的类）；
- force 新 attempt → 旧审批 SUPERSEDED（P1-2）。
"""

from __future__ import annotations

from datetime import UTC, datetime, timedelta
from typing import Any

import pytest
from sqlalchemy import create_engine, select
from sqlalchemy.pool import StaticPool

from industrial_ops_agent.approval.models import (
    ApprovalConflict,
    SeparationOfDutiesViolation,
)
from industrial_ops_agent.network_assurance.council_contracts import (
    CouncilRole,
)
from industrial_ops_agent.network_assurance.network_action_approval import (
    IdempotencyConflict,
    ManualExecutionNotAllowed,
    NetworkActionApprovalService,
)
from industrial_ops_agent.network_assurance.network_council import NetworkCouncilRunner
from industrial_ops_agent.network_assurance.network_council_service import (
    CouncilRequestInput,
    NetworkCouncilService,
)
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import (
    Base,
    NetworkActionApprovalDecisionRecord,
    NetworkActionApprovalRecord,
    NetworkActionDecisionRecord,
    NetworkAssetRecord,
    NetworkCouncilProposalRecord,
    NetworkPendingActionRecord,
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
def context() -> TenantContext:
    return TenantContext(tenant_id="tenant-alpha", subject_id="initiator-1")


class CountingComplete:
    """Mock 模型：三专家 + 协调官，协调官产出两条建议（只读动作 + 调参）。"""

    def __init__(self, config_key: str = "rtt.interval", config_value: str = "5s") -> None:
        self.calls = 0
        self._config_key = config_key
        self._config_value = config_value

    def __call__(self, role: CouncilRole, payload: dict[str, Any]) -> dict[str, Any]:
        self.calls += 1
        if role is CouncilRole.COORDINATOR:
            digest = str(payload.get("canonical_diagnosis_digest", "sha256:diag-1"))
            return {
                "proposals": [
                    {
                        "kind": "ACTION_ID",
                        "action_id": "CHECK_RESOLVER_CONFIG",
                        "rationale": "现有诊断指向解析链路，先确认本机解析器配置",
                        "proposed_preconditions": [],
                        "source_bindings": [
                            {"kind": "CANONICAL_DIAGNOSIS", "ref_id": digest}
                        ],
                    },
                    {
                        "kind": "CONFIG_CHANGE",
                        "config_key": self._config_key,
                        "config_value": self._config_value,
                        "rationale": "加密采样周期以获取更细粒度的延迟证据",
                        "proposed_preconditions": [],
                        "source_bindings": [
                            {"kind": "CANONICAL_DIAGNOSIS", "ref_id": digest}
                        ],
                    },
                ]
            }
        return {
            "observations": ["事实 A"],
            "referenced_fact_ids": ["e1"],
            "recommendation_direction": "建议方向",
        }


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
        "knowledge_cases": [],
        "retrieval_strategy_version": "rag-v1",
    }
    base.update(overrides)
    return CouncilRequestInput(**base)


def _provision_asset(test_db: Database, context: TenantContext, asset_id: str) -> None:
    """queue_action 要求目标资产已注册（与 test_network_assurance_service 同法）。"""
    with test_db.transaction(context) as session:
        if session.get(NetworkAssetRecord, asset_id) is None:
            session.add(
                NetworkAssetRecord(
                    asset_id=asset_id,
                    tenant_id=context.tenant_id,
                    display_name=asset_id,
                    link_type="WIRED_ETHERNET",
                    overall_state="GOOD",
                    display_score=100,
                    connection_status="ONLINE",
                )
            )


def _convene(
    test_db: Database,
    context: TenantContext,
    *,
    force: bool = False,
    **request_kw: Any,
):
    _provision_asset(test_db, context, "radxa-cubie-a7a")
    complete = CountingComplete()
    service = NetworkCouncilService(test_db, NetworkCouncilRunner(complete=complete))
    view = service.request_council(context, _request(**request_kw), force=force)
    return service, view


def _rows(test_db: Database, model: Any) -> list[Any]:
    with test_db.transaction(TenantContext(tenant_id="tenant-alpha", subject_id="x")) as session:
        return list(session.scalars(select(model)).all())


# ---------------------------------------------------------------------------
# P0-1 + P0-2：稳定身份与 BLOCKED 语义
# ---------------------------------------------------------------------------


def test_all_e1_proposals_get_stable_ids(test_db: Database, context: TenantContext) -> None:
    _, view = _convene(test_db, context)
    proposals = _rows(test_db, NetworkCouncilProposalRecord)
    assert len(proposals) == 2
    ids = {p.proposal_id for p in proposals}
    assert len(ids) == 2
    assert all(pid.startswith("ncprop-") for pid in ids)
    # 快照不可变：存的是 ActionProposal 的完整原文
    assert all(p.proposal_snapshot.get("rationale") for p in proposals)
    # 索引仅用于展示排序
    assert sorted(p.proposal_index for p in proposals) == [0, 1]


def test_blocked_decision_creates_no_approval_request(
    test_db: Database, context: TenantContext
) -> None:
    """P0-2：Policy 拦截（catalog 漂移）→ 有提案、有裁决、零审批。"""
    _, view = _convene(test_db, context, gateway_catalog_version="cat-old-firmware")

    proposals = _rows(test_db, NetworkCouncilProposalRecord)
    decisions = _rows(test_db, NetworkActionDecisionRecord)
    approvals = _rows(test_db, NetworkActionApprovalRecord)

    assert len(proposals) == 2          # 建议身份仍在（可审计"建议了什么"）
    assert len(decisions) == 2          # 裁决留痕（可审计"为什么被拦"）
    assert all(d.allowed is False for d in decisions)
    assert all("catalog_version_mismatch" in (d.block_reason or "") for d in decisions)
    assert approvals == []              # P0-2：零审批行——没有"请批准"与"禁止执行"并存


def test_allowed_decisions_create_pending_approvals(
    test_db: Database, context: TenantContext
) -> None:
    _, view = _convene(test_db, context)
    decisions = _rows(test_db, NetworkActionDecisionRecord)
    approvals = _rows(test_db, NetworkActionApprovalRecord)

    assert len(decisions) == 2 and all(d.allowed for d in decisions)
    assert len(approvals) == 2
    assert all(a.status == "PENDING" for a in approvals)
    assert all(a.execution_status == "NOT_EXECUTED" for a in approvals)
    # 执行模式按提案类型分派：ACTION_ID → MANUAL，CONFIG_CHANGE → REMOTE
    assert any(a.manual_execution_required for a in approvals)
    assert any(not a.manual_execution_required for a in approvals)


# ---------------------------------------------------------------------------
# Approval 批准的是 decision，不是 LLM 原文
# ---------------------------------------------------------------------------


def test_approval_binds_decision_not_raw_proposal(
    test_db: Database, context: TenantContext
) -> None:
    _, view = _convene(test_db, context)
    decisions = {d.proposal_id: d for d in _rows(test_db, NetworkActionDecisionRecord)}
    for approval in _rows(test_db, NetworkActionApprovalRecord):
        decision = decisions[approval.proposal_id]
        assert approval.decision_id == decision.decision_id
        assert approval.decision_digest == decision.decision_digest
        # 批准载荷 = Policy 规范化动作（不是 LLM 原文）
        assert approval.normalized_action == decision.normalized_action
        assert approval.approval_payload_digest == decision.normalized_action_digest


# ---------------------------------------------------------------------------
# 人的批准（Guard 真复用：异常类来自 approval.models）
# ---------------------------------------------------------------------------


def test_self_approval_rejected_with_platform_guard(
    test_db: Database, context: TenantContext
) -> None:
    _, view = _convene(test_db, context)
    approval = _rows(test_db, NetworkActionApprovalRecord)[0]
    service = NetworkActionApprovalService(test_db)

    # 发起人（= council.requested_by）试图自批 → SoD，异常类就是 approval.models 的
    with pytest.raises(SeparationOfDutiesViolation):
        service.decide(
            context,
            approval.proposal_id,
            approval_decision="APPROVED",
            reason="我批准我自己",
            expected_version=approval.version,
            decider_subject_id=context.subject_id,
        )


def test_wrong_version_rejected_with_platform_guard(
    test_db: Database, context: TenantContext
) -> None:
    _, view = _convene(test_db, context)
    approval = _rows(test_db, NetworkActionApprovalRecord)[0]
    service = NetworkActionApprovalService(test_db)

    with pytest.raises(ApprovalConflict):
        service.decide(
            context,
            approval.proposal_id,
            approval_decision="APPROVED",
            reason="版本不对的批准",
            expected_version=approval.version + 99,
            decider_subject_id="approver-2",
        )
    # 类对象必须与平台共用同一个（P1-3：零复制）
    import industrial_ops_agent.approval.models as platform_models

    assert ApprovalConflict is platform_models.ApprovalConflict
    assert SeparationOfDutiesViolation is platform_models.SeparationOfDutiesViolation


def test_approve_then_execute_queues_remote_config(
    test_db: Database, context: TenantContext
) -> None:
    _, view = _convene(test_db, context)
    service = NetworkActionApprovalService(test_db)

    remote = next(
        a for a in _rows(test_db, NetworkActionApprovalRecord)
        if not a.manual_execution_required
    )
    updated = service.decide(
        context,
        remote.proposal_id,
        approval_decision="APPROVED",
        reason="延迟抖动需要更细粒度采样，生产风险可接受",
        expected_version=remote.version,
        decider_subject_id="approver-2",
    )
    assert updated["status"] == "APPROVED"
    assert updated["approved_by"] == "approver-2"

    result = service.execute(context, remote.proposal_id, idempotency_key="exec-key-1")
    assert result["execution_status"] == "QUEUED"
    assert result["queued_action_id"]
    assert result["idempotent_replay"] is False

    # 载荷来自批准快照：config_key/value 与当初被批准的规范化动作一致
    pending = _rows(test_db, NetworkPendingActionRecord)
    assert len(pending) == 1
    assert pending[0].config_key == "rtt.interval"
    assert pending[0].config_value == "5s"
    # approved_by 只来自 decision 行
    assert pending[0].approved_by == "approver-2"

    # 幂等：同一 key 重放 → 短路，不产生第二个 pending action
    replay = service.execute(context, remote.proposal_id, idempotency_key="exec-key-1")
    assert replay["idempotent_replay"] is True
    assert len(_rows(test_db, NetworkPendingActionRecord)) == 1

    # 异键 → 冲突
    with pytest.raises(IdempotencyConflict):
        service.execute(context, remote.proposal_id, idempotency_key="exec-key-2")


def test_execute_requires_approval_first(
    test_db: Database, context: TenantContext
) -> None:
    _, view = _convene(test_db, context)
    service = NetworkActionApprovalService(test_db)
    remote = next(
        a for a in _rows(test_db, NetworkActionApprovalRecord)
        if not a.manual_execution_required
    )
    # 未批准 → execution_rejection(PENDING_APPROVAL) → ApprovalConflict
    with pytest.raises(ApprovalConflict):
        service.execute(context, remote.proposal_id, idempotency_key="k")
    assert _rows(test_db, NetworkPendingActionRecord) == []


# ---------------------------------------------------------------------------
# P0-3：MANUAL_RUNBOOK 不伪装执行
# ---------------------------------------------------------------------------


def test_manual_execution_not_allowed_and_runbook_from_snapshot(
    test_db: Database, context: TenantContext
) -> None:
    _, view = _convene(test_db, context)
    service = NetworkActionApprovalService(test_db)

    manual = next(
        a for a in _rows(test_db, NetworkActionApprovalRecord)
        if a.manual_execution_required
    )
    service.decide(
        context,
        manual.proposal_id,
        approval_decision="APPROVED",
        reason="只读检查，低风险，现场执行",
        expected_version=manual.version,
        decider_subject_id="approver-2",
    )

    # P0-3：execute 不是"返回命令"，而是明确拒绝
    with pytest.raises(ManualExecutionNotAllowed):
        service.execute(context, manual.proposal_id, idempotency_key="k")
    assert _rows(test_db, NetworkPendingActionRecord) == []  # 板端零下发

    rb = service.runbook(context, manual.proposal_id)
    assert rb["command"] == "sudo weaknet action CHECK_RESOLVER_CONFIG"
    assert rb["manual_execution_required"] is True
    assert rb["execution_status"] == "NOT_EXECUTED"      # UI 绝不能渲染成"已执行"
    assert rb["approved_by"] == "approver-2"
    assert rb["runbook_renderer_version"] == "runbook-v1"
    assert rb["catalog_version"]          # 批准时的 catalog 版本随快照固化


def test_runbook_rejects_unapproved_or_remote(
    test_db: Database, context: TenantContext
) -> None:
    _, view = _convene(test_db, context)
    service = NetworkActionApprovalService(test_db)
    approvals = _rows(test_db, NetworkActionApprovalRecord)
    manual = next(a for a in approvals if a.manual_execution_required)
    remote = next(a for a in approvals if not a.manual_execution_required)

    # 未批准不给手册
    with pytest.raises(ApprovalConflict):
        service.runbook(context, manual.proposal_id)
    # 远程提案没有手册
    with pytest.raises(ManualExecutionNotAllowed):
        service.runbook(context, remote.proposal_id)


# ---------------------------------------------------------------------------
# P1-2：force 新 attempt → SUPERSEDED
# ---------------------------------------------------------------------------


def test_force_supersedes_old_approvals(
    test_db: Database, context: TenantContext
) -> None:
    service, first = _convene(test_db, context)
    first_approvals = _rows(test_db, NetworkActionApprovalRecord)
    assert len(first_approvals) == 2
    first_attempt = {a.approval_id for a in first_approvals}

    # 同一输入 force 重跑 → 新 attempt、新 proposal_id、新审批
    service.request_council(context, _request(), force=True)
    all_approvals = _rows(test_db, NetworkActionApprovalRecord)

    old = [a for a in all_approvals if a.approval_id in first_attempt]
    new = [a for a in all_approvals if a.approval_id not in first_attempt]
    assert all(a.status == "SUPERSEDED" for a in old)   # 被取代 ≠ 过期
    assert len(new) == 2 and all(a.status == "PENDING" for a in new)
    assert old[0].attempt < new[0].attempt

    # 被取代的审批无法执行（状态门禁）
    approval_service = NetworkActionApprovalService(test_db)
    old_manual = next(a for a in old if a.manual_execution_required)
    with pytest.raises(ApprovalConflict):
        approval_service.runbook(context, old_manual.proposal_id)


# ---------------------------------------------------------------------------
# 命名纪律：policy_decision 与 approval_decision 分表
# ---------------------------------------------------------------------------


def test_policy_and_approval_decisions_live_in_separate_tables(
    test_db: Database, context: TenantContext
) -> None:
    _, view = _convene(test_db, context)
    service = NetworkActionApprovalService(test_db)
    approval = _rows(test_db, NetworkActionApprovalRecord)[0]
    service.decide(
        context,
        approval.proposal_id,
        approval_decision="REJECTED",
        reason="当前不批，等生产窗口",
        expected_version=approval.version,
        decider_subject_id="approver-2",
    )

    policy_rows = _rows(test_db, NetworkActionDecisionRecord)      # Policy 裁决
    approval_rows = _rows(test_db, NetworkActionApprovalDecisionRecord)  # 人裁
    assert len(policy_rows) == 2                # policy：每条提案一裁
    assert len(approval_rows) == 1              # approval：仅这一次人裁
    assert approval_rows[0].approval_decision == "REJECTED"
    assert approval_rows[0].decider_subject_id == "approver-2"
    # 两表字段不混：approval 侧没有 risk/allowed，policy 侧没有 decider
    assert not hasattr(approval_rows[0], "allowed")
    assert not hasattr(policy_rows[0], "decider_subject_id")


# ---------------------------------------------------------------------------
# 跨租户隔离（sqlite 无 RLS，靠应用层 tenant_id 条件兜底——必须有测试钉死）
# ---------------------------------------------------------------------------


def test_cross_tenant_proposal_id_is_invisible(
    test_db: Database, context: TenantContext
) -> None:
    """tenant-B 拿 tenant-A 的 proposal_id：decide/execute/runbook 全部 404。"""
    from industrial_ops_agent.network_assurance.network_action_approval import (
        ApprovalNotFound,
    )

    _convene(test_db, context)
    service = NetworkActionApprovalService(test_db)
    proposal_id = _rows(test_db, NetworkActionApprovalRecord)[0].proposal_id
    other = TenantContext(tenant_id="tenant-beta", subject_id="intruder")

    with pytest.raises(ApprovalNotFound):
        service.decide(
            other,
            proposal_id,
            approval_decision="APPROVED",
            reason="跨租户批准尝试",
            expected_version=1,
            decider_subject_id="intruder",
        )
    with pytest.raises(ApprovalNotFound):
        service.execute(other, proposal_id, idempotency_key="k")
    with pytest.raises(ApprovalNotFound):
        service.runbook(other, proposal_id)


# ---------------------------------------------------------------------------
# EXPIRED 是存储态：到期审批被读出时落库迁移（DB/API/Web/审计四边一致）
# ---------------------------------------------------------------------------


def test_expired_approval_transitions_and_refuses_decide_execute(
    test_db: Database, context: TenantContext
) -> None:
    _, view = _convene(test_db, context)
    service = NetworkActionApprovalService(test_db)
    # 用 REMOTE（CONFIG_CHANGE）审批：MANUAL 会先被 manual_execution_required 拦，
    # 走不到过期判定——测 EXPIRED 语义必须用远程提案。
    approval = next(
        a for a in _rows(test_db, NetworkActionApprovalRecord)
        if not a.manual_execution_required
    )

    # 手动把 expires_at 拨到过去（等价于等 24h TTL 到期）
    with test_db.transaction(context) as session:
        row = session.get(NetworkActionApprovalRecord, approval.approval_id)
        row.expires_at = datetime.now(UTC) - timedelta(seconds=1)

    # decide 触发 lazy transition：PENDING → EXPIRED 落库，再被 Guard 拦
    with pytest.raises(ApprovalConflict):
        service.decide(
            context,
            approval.proposal_id,
            approval_decision="APPROVED",
            reason="到期后才来批准",
            expected_version=approval.version,
            decider_subject_id="approver-2",
        )
    stored = next(
        a for a in _rows(test_db, NetworkActionApprovalRecord)
        if a.approval_id == approval.approval_id
    )
    assert stored.status == "EXPIRED"          # 不是永远 PENDING
    assert stored.version == approval.version + 1

    # execute / runbook 同样拒绝（EXPIRED ≠ APPROVED）
    with pytest.raises(ApprovalConflict):
        service.execute(context, approval.proposal_id, idempotency_key="k")
    assert _rows(test_db, NetworkPendingActionRecord) == []

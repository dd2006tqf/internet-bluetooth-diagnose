"""⑥ 端到端审计链测试（golden trace REMOTE_HAPPY + 其余四场景）。

纪律：
- Node 只记录**真实存在**的对象；不存在的边/操作走 edge_checks /
  operation_checks，不虚构 absent 节点；
- T5/T6（审批失效、幂等/租户/attempt 隔离）走真实 service 路径捕获
  拒绝，而不是读取已有单测结论；
- Prediction 不落库——运行时捕获进 prediction_snapshots；
- Council 用确定性 mock（CountingComplete），live 冒烟在
  test_network_council_live.py，不污染本场景。
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
from industrial_ops_agent.network_assurance.council_contracts import CouncilRole
from industrial_ops_agent.network_assurance.e2e_trace import (
    TraceEdgeCheck,
    TraceNode,
    TraceOperationCheck,
    TraceReport,
    verify_trace,
)
from industrial_ops_agent.network_assurance.network_action_approval import (
    ApprovalNotFound,
    IdempotencyConflict,
    ManualExecutionNotAllowed,
    NetworkActionApprovalService,
)
from industrial_ops_agent.network_assurance.network_council import (
    NetworkCouncilRunner,
)
from industrial_ops_agent.network_assurance.network_council_service import (
    CouncilRequestInput,
    NetworkCouncilService,
)
from industrial_ops_agent.network_assurance.service import NetworkAssuranceService
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import (
    Base,
    NetworkActionApprovalRecord,
    NetworkActionDecisionRecord,
    NetworkActionOutcomeRecord,
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


GATEWAY = "radxa-cubie-a7a"


class CountingComplete:
    """确定性协调官：固定产出 1 条 ACTION_ID + 1 条 CONFIG_CHANGE。"""

    def __call__(self, role: CouncilRole, payload: dict[str, Any]) -> dict[str, Any]:
        if role is CouncilRole.COORDINATOR:
            digest = str(payload.get("canonical_diagnosis_digest", "sha256:diag-1"))
            return {
                "proposals": [
                    {
                        "kind": "ACTION_ID",
                        "action_id": "CHECK_RESOLVER_CONFIG",
                        "rationale": "确认本机解析器配置",
                        "proposed_preconditions": [],
                        "source_bindings": [
                            {"kind": "CANONICAL_DIAGNOSIS", "ref_id": digest}
                        ],
                    },
                    {
                        "kind": "CONFIG_CHANGE",
                        "config_key": "rtt.interval",
                        "config_value": "5s",
                        "rationale": "更细粒度采样观察抖动",
                        "proposed_preconditions": [],
                        "source_bindings": [
                            {"kind": "CANONICAL_DIAGNOSIS", "ref_id": digest}
                        ],
                    },
                ]
            }
        return {
            "observations": ["fact"],
            "referenced_fact_ids": ["e1"],
            "recommendation_direction": "dir",
        }


def _request(**overrides: Any) -> CouncilRequestInput:
    base: dict[str, Any] = {
        "incident_id": "sitinc_e2e_1",
        "canonical_diagnosis_digest": "sha256:diag-1",
        "prediction_results": [],
        "device_facts": ["e1"],
        "rf_facts": [],
        "kernel_facts": [],
        "diagnosis_evidence_ids": ["diag-evidence-1"],
        "prediction_evidence_ids": [],
        "affected_assets": [GATEWAY],
        "production_critical": False,
        "knowledge_cases": [],
        "retrieval_strategy_version": "rag-v1",
    }
    base.update(overrides)
    return CouncilRequestInput(**base)


def _provision_asset(db: Database, ctx: TenantContext, asset_id: str) -> None:
    with db.transaction(ctx) as s:
        if s.get(NetworkAssetRecord, asset_id) is None:
            s.add(
                NetworkAssetRecord(
                    asset_id=asset_id,
                    tenant_id=ctx.tenant_id,
                    display_name=asset_id,
                    link_type="WIRED_ETHERNET",
                    overall_state="GOOD",
                    display_score=100,
                    connection_status="ONLINE",
                )
            )


def _rows(db: Database, model: Any, tenant: str = "tenant-alpha") -> list[Any]:
    with db.transaction(TenantContext(tenant_id=tenant, subject_id="x")) as s:
        return list(s.scalars(select(model)).all())


def _deliver_and_ack(
    db: Database, ctx: TenantContext, action_id: str, *, status: str = "APPLIED"
) -> None:
    """完整走 claim(DELIVERED) → record_action_results(终态)。"""
    from industrial_ops_agent.network_assurance.contracts import (
        NetworkActionOutcome,
        NetworkActionResults,
    )

    service = NetworkAssuranceService(db)
    with db.transaction(ctx) as session:
        claimed = service._claim_pending_actions(
            session, ctx, GATEWAY, now=datetime.now(UTC)
        )
    token = next(c["claim_token"] for c in claimed if c["action_id"] == action_id)
    results = NetworkActionResults(
        device_id=GATEWAY,
        results=[
            NetworkActionOutcome(
                action_id=action_id,
                status=status,
                detail="e2e ack",
                claim_token=token,
                generation=1,
                reported_at=datetime.now(UTC),
            )
        ],
    )
    service.record_action_results(ctx, results)


def _convene(db: Database, ctx: TenantContext, **kw: Any):
    _provision_asset(db, ctx, GATEWAY)
    service = NetworkCouncilService(db, NetworkCouncilRunner(complete=CountingComplete()))
    return service, service.request_council(ctx, _request(**kw))


def _find_proposal(
    db: Database, *, manual: bool
) -> tuple[NetworkCouncilProposalRecord, NetworkActionApprovalRecord]:
    proposals = _rows(db, NetworkCouncilProposalRecord)
    approvals = _rows(db, NetworkActionApprovalRecord)
    for p in proposals:
        a = next(a for a in approvals if a.proposal_id == p.proposal_id)
        if a.manual_execution_required == manual:
            return p, a
    raise AssertionError("proposal not found")


# ---------------------------------------------------------------------------
# 场景 A：REMOTE_HAPPY —— golden trace
# ---------------------------------------------------------------------------


def test_scenario_a_remote_happy_path(
    test_db: Database, context: TenantContext, tmp_path
):
    _, view = _convene(test_db, context)
    approval_svc = NetworkActionApprovalService(test_db)

    proposal, approval = _find_proposal(test_db, manual=False)
    decision = next(
        d for d in _rows(test_db, NetworkActionDecisionRecord)
        if d.proposal_id == proposal.proposal_id
    )

    # 人裁
    approval_svc.decide(
        context,
        proposal.proposal_id,
        approval_decision="APPROVED",
        reason="生产窗口内可执行",
        expected_version=approval.version,
        decider_subject_id="approver-2",
    )

    # 执行（REMOTE：payload 来自批准快照）
    approval_svc.execute(context, proposal.proposal_id, idempotency_key="trace-a")
    pending = _rows(test_db, NetworkPendingActionRecord)[0]

    # 板端回报
    _deliver_and_ack(test_db, context, pending.action_id)
    outcome = _rows(test_db, NetworkActionOutcomeRecord)[0]
    pending_after = _rows(test_db, NetworkPendingActionRecord)[0]

    report = TraceReport(
        scenario="REMOTE_HAPPY",
        incident_id="sitinc_e2e_1",
        nodes=[
            TraceNode(kind="INCIDENT", id="sitinc_e2e_1", status="ONGOING"),
            TraceNode(
                kind="DIAGNOSIS",
                id="sha256:diag-1",
                digest_or_version="sha256:diag-1",
                status="ALLOWED",
            ),
            TraceNode(
                kind="COUNCIL",
                id=view.council_id,
                digest_or_version=view.input_fingerprint[:24],
                status=view.status,
                extra={"attempt": 1},
            ),
            TraceNode(
                kind="PROPOSAL",
                id=proposal.proposal_id,
                digest_or_version=proposal.proposal_digest[:24],
                status="E1_VALID",
            ),
            TraceNode(
                kind="ACTION_DECISION",
                id=decision.decision_id,
                digest_or_version=decision.decision_digest[:24],
                status="ALLOWED" if decision.allowed else "BLOCKED",
                extra={
                    "risk": decision.risk,
                    "execution_mode": decision.execution_mode,
                    "risk_policy_version": decision.risk_policy_version,
                },
            ),
            TraceNode(
                kind="APPROVAL",
                id=approval.approval_id,
                digest_or_version=approval.decision_digest[:24],
                status="APPROVED",
                extra={"approved_by": "approver-2"},
            ),
            TraceNode(
                kind="PENDING_ACTION",
                id=pending.action_id,
                digest_or_version=f"gen{pending.generation}",
                status=pending_after.status,
                extra={
                    "config": f"{pending.config_key}={pending.config_value}",
                    "approved_by": pending.approved_by,
                },
            ),
            TraceNode(
                kind="ACTION_OUTCOME",
                id=outcome.outcome_id,
                digest_or_version=outcome.action_payload_digest[:24],
                status=outcome.status,
                ts=outcome.completed_at.isoformat(),
            ),
        ],
        edge_checks=[
            # 存在的边
            TraceEdgeCheck("PROPOSAL", "ACTION_DECISION", "PRESENT", "PRESENT"),
            TraceEdgeCheck("ACTION_DECISION", "APPROVAL", "PRESENT", "PRESENT"),
            TraceEdgeCheck("APPROVAL", "PENDING_ACTION", "PRESENT", "PRESENT"),
            TraceEdgeCheck("PENDING_ACTION", "ACTION_OUTCOME", "PRESENT", "PRESENT"),
            # L5 终态 → L1 事实投影的字段一致性（核心断言）
            TraceEdgeCheck(
                "PENDING_ACTION",
                "ACTION_OUTCOME",
                "PRESENT",
                "PRESENT"
                if (
                    outcome.pending_action_id == pending_after.action_id
                    and outcome.status == pending_after.status
                    and outcome.completed_at == pending_after.completed_at
                    and outcome.action_snapshot["config_key"] == pending_after.config_key
                    and outcome.action_snapshot["config_value"]
                    == pending_after.config_value
                )
                else "ABSENT",
                reason="l5_terminal_fields_match_l1_projection",
            ),
            # provenance 直达 outcome
            TraceEdgeCheck(
                "PROPOSAL",
                "ACTION_OUTCOME",
                "PRESENT",
                "PRESENT"
                if (
                    outcome.proposal_id == proposal.proposal_id
                    and outcome.approval_id == approval.approval_id
                )
                else "ABSENT",
                reason="council_provenance_reaches_l1_fact",
            ),
        ],
        operation_checks=[
            TraceOperationCheck("decide", proposal.proposal_id, "ACCEPTED", "ACCEPTED"),
            TraceOperationCheck("execute", proposal.proposal_id, "ACCEPTED", "ACCEPTED"),
            TraceOperationCheck(
                "edge_ack", pending.action_id, "ACCEPTED", "ACCEPTED"
            ),
        ],
        completed_at=datetime.now(UTC).isoformat(),
    )
    verdict = verify_trace(report)
    # pytest 断言：golden trace 必须 OK；断链时把明细带进失败信息
    assert verdict.result == "OK", f"trace broken: {verdict.details}"
    # 落地验证：L5↔L1 字段一致性 + provenance 直达
    assert outcome.pending_action_id == pending_after.action_id
    assert outcome.status == pending_after.status == "APPLIED"
    assert outcome.completed_at == pending_after.completed_at
    assert outcome.proposal_id == proposal.proposal_id
    assert outcome.approval_id == approval.approval_id

    # T7：把 golden trace 落盘，让 reporter CLI 端到端跑一次（真实验证产物）
    import subprocess
    import sys
    from pathlib import Path

    report_path = tmp_path / "trace_a.json"
    report_path.write_text(report.to_json(), encoding="utf-8")
    proc = subprocess.run(
        [
            sys.executable,
            "scripts/verify_end_to_end_trace.py",
            "--trace-report",
            str(report_path),
        ],
        capture_output=True,
        text=True,
        cwd=Path(__file__).resolve().parent.parent,
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "TRACE OK" in proc.stdout
    assert "ACTION_OUTCOME" in proc.stdout


# ---------------------------------------------------------------------------
# 场景 B：MANUAL_RUNBOOK —— ACTION_ID 批准后只产手册，绝不进 L5
# ---------------------------------------------------------------------------


def test_scenario_b_manual_runbook(test_db: Database, context: TenantContext):
    _, view = _convene(test_db, context)
    approval_svc = NetworkActionApprovalService(test_db)
    proposal, approval = _find_proposal(test_db, manual=True)
    decision = next(
        d for d in _rows(test_db, NetworkActionDecisionRecord)
        if d.proposal_id == proposal.proposal_id
    )

    approval_svc.decide(
        context,
        proposal.proposal_id,
        approval_decision="APPROVED",
        reason="低风险只读，现场执行",
        expected_version=approval.version,
        decider_subject_id="approver-2",
    )
    runbook = approval_svc.runbook(context, proposal.proposal_id)

    # execute 必须拒绝（409 语义）
    try:
        approval_svc.execute(context, proposal.proposal_id, idempotency_key="x")
        execute_actual = "ACCEPTED"
        execute_err = None
    except ManualExecutionNotAllowed:
        execute_actual = "REJECTED"
        execute_err = "manual_execution_not_allowed"

    report = TraceReport(
        scenario="MANUAL_RUNBOOK",
        incident_id="sitinc_e2e_1",
        nodes=[
            TraceNode(kind="INCIDENT", id="sitinc_e2e_1", status="ONGOING"),
            TraceNode(kind="COUNCIL", id=view.council_id, status=view.status),
            TraceNode(kind="PROPOSAL", id=proposal.proposal_id, status="E1_VALID"),
            TraceNode(
                kind="ACTION_DECISION",
                id=decision.decision_id,
                status="ALLOWED",
                extra={"execution_mode": "MANUAL_RUNBOOK", "risk": decision.risk},
            ),
            TraceNode(
                kind="APPROVAL", id=approval.approval_id, status="APPROVED"
            ),
            TraceNode(
                kind="MANUAL_RUNBOOK",
                id=approval.approval_id,
                status="NOT_EXECUTED",
                extra={"command": runbook["command"]},
            ),
        ],
        edge_checks=[
            TraceEdgeCheck("ACTION_DECISION", "APPROVAL", "PRESENT", "PRESENT"),
            TraceEdgeCheck("APPROVAL", "MANUAL_RUNBOOK", "PRESENT", "PRESENT"),
            # 不存在的边：MANUAL 绝不能产生 pending action / outcome
            TraceEdgeCheck(
                "APPROVAL",
                "PENDING_ACTION",
                "ABSENT",
                "PRESENT" if _rows(test_db, NetworkPendingActionRecord) else "ABSENT",
                reason="manual_runbook_creates_no_l5_queue_entry",
            ),
            TraceEdgeCheck(
                "PENDING_ACTION",
                "ACTION_OUTCOME",
                "ABSENT",
                "PRESENT" if _rows(test_db, NetworkActionOutcomeRecord) else "ABSENT",
            ),
        ],
        operation_checks=[
            TraceOperationCheck(
                "execute",
                proposal.proposal_id,
                "REJECTED",
                execute_actual,
                error_code=execute_err,
            ),
            TraceOperationCheck(
                "runbook", proposal.proposal_id, "ACCEPTED", "ACCEPTED"
            ),
        ],
        completed_at=datetime.now(UTC).isoformat(),
    )
    verdict = verify_trace(report)
    assert verdict.result == "OK", f"trace broken: {verdict.details}"
    # 硬断言：MANUAL 永远不能声称已执行
    assert runbook["execution_status"] == "NOT_EXECUTED"
    assert runbook["manual_execution_required"] is True
    assert _rows(test_db, NetworkPendingActionRecord) == []


# ---------------------------------------------------------------------------
# 场景 C：POLICY_BLOCKED —— catalog 漂移 fail closed
# ---------------------------------------------------------------------------


def test_scenario_c_policy_blocked(test_db: Database, context: TenantContext):
    _, view = _convene(test_db, context, gateway_catalog_version="cat-old-firmware")

    proposals = _rows(test_db, NetworkCouncilProposalRecord)
    decisions = _rows(test_db, NetworkActionDecisionRecord)
    approvals = _rows(test_db, NetworkActionApprovalRecord)

    report = TraceReport(
        scenario="POLICY_BLOCKED",
        incident_id="sitinc_e2e_1",
        nodes=[
            TraceNode(kind="INCIDENT", id="sitinc_e2e_1", status="ONGOING"),
            TraceNode(kind="COUNCIL", id=view.council_id, status=view.status),
            *(
                TraceNode(
                    kind="PROPOSAL", id=p.proposal_id, status="E1_VALID"
                )
                for p in proposals
            ),
            *(
                TraceNode(
                    kind="ACTION_DECISION",
                    id=d.decision_id,
                    status="BLOCKED",
                    extra={"block_reason": d.block_reason},
                )
                for d in decisions
            ),
        ],
        edge_checks=[
            # Proposal 和 Decision 都必须存在（可审计"建议了什么/为什么被拦"）
            TraceEdgeCheck(
                "PROPOSAL",
                "ACTION_DECISION",
                "PRESENT",
                "PRESENT" if len(decisions) == len(proposals) else "ABSENT",
            ),
            # 不存在的边：BLOCKED 绝不能有 approval / pending / outcome
            TraceEdgeCheck(
                "ACTION_DECISION",
                "APPROVAL",
                "ABSENT",
                "PRESENT" if approvals else "ABSENT",
                reason="blocked_decision_creates_no_approval",
            ),
            TraceEdgeCheck(
                "APPROVAL",
                "PENDING_ACTION",
                "ABSENT",
                "PRESENT" if _rows(test_db, NetworkPendingActionRecord) else "ABSENT",
            ),
            TraceEdgeCheck(
                "PENDING_ACTION",
                "ACTION_OUTCOME",
                "ABSENT",
                "PRESENT" if _rows(test_db, NetworkActionOutcomeRecord) else "ABSENT",
            ),
        ],
        operation_checks=[],
        completed_at=datetime.now(UTC).isoformat(),
    )
    verdict = verify_trace(report)
    assert verdict.result == "OK", f"trace broken: {verdict.details}"
    assert approvals == []
    assert all(not d.allowed for d in decisions)


# ---------------------------------------------------------------------------
# 场景 D：APPROVAL_INVALID —— 失效审批全部不可执行
# ---------------------------------------------------------------------------


def test_scenario_d_approval_invalid_paths(test_db: Database, context: TenantContext):
    _, view = _convene(test_db, context)
    svc = NetworkActionApprovalService(test_db)
    proposal, approval = _find_proposal(test_db, manual=False)
    checks: list[TraceOperationCheck] = []

    def attempt(op: str, fn) -> tuple[str, str | None]:
        try:
            fn()
            return "ACCEPTED", None
        except (ApprovalConflict, SeparationOfDutiesViolation, ManualExecutionNotAllowed) as e:
            return "REJECTED", type(e).__name__

    # 1) SoD：发起人自批
    actual, code = attempt(
        "decide",
        lambda: svc.decide(
            context,
            proposal.proposal_id,
            approval_decision="APPROVED",
            reason="自批尝试",
            expected_version=approval.version,
            decider_subject_id=context.subject_id,
        ),
    )
    checks.append(
        TraceOperationCheck("decide_sod", proposal.proposal_id, "REJECTED", actual, code)
    )

    # 2) If-Match 版本错
    actual, code = attempt(
        "decide",
        lambda: svc.decide(
            context,
            proposal.proposal_id,
            approval_decision="APPROVED",
            reason="版本不对",
            expected_version=approval.version + 99,
            decider_subject_id="approver-2",
        ),
    )
    checks.append(
        TraceOperationCheck(
            "decide_version", proposal.proposal_id, "REJECTED", actual, code
        )
    )

    # 3) 未批准执行
    actual, code = attempt(
        "execute", lambda: svc.execute(context, proposal.proposal_id, idempotency_key="e1")
    )
    checks.append(
        TraceOperationCheck(
            "execute_pending", proposal.proposal_id, "REJECTED", actual, code
        )
    )

    # 4) EXPIRED：拨过期后再 decide
    with test_db.transaction(context) as s:
        row = s.get(NetworkActionApprovalRecord, approval.approval_id)
        row.expires_at = datetime.now(UTC) - timedelta(seconds=1)
    actual, code = attempt(
        "decide",
        lambda: svc.decide(
            context,
            proposal.proposal_id,
            approval_decision="APPROVED",
            reason="过期后才批",
            expected_version=approval.version + 1,  # lazy expire 已 +1
            decider_subject_id="approver-2",
        ),
    )
    checks.append(
        TraceOperationCheck(
            "decide_expired", proposal.proposal_id, "REJECTED", actual, code
        )
    )
    actual, code = attempt(
        "execute", lambda: svc.execute(context, proposal.proposal_id, idempotency_key="e2")
    )
    checks.append(
        TraceOperationCheck(
            "execute_expired", proposal.proposal_id, "REJECTED", actual, code
        )
    )

    # 5) SUPERSEDED：force 重跑后旧审批不可用
    council_svc, _ = _convene(test_db, context)
    council_svc.request_council(context, _request(), force=True)
    stale = next(
        a for a in _rows(test_db, NetworkActionApprovalRecord)
        if a.attempt == 1 and a.status == "SUPERSEDED"
    )
    actual, code = attempt(
        "execute",
        lambda: svc.execute(context, stale.proposal_id, idempotency_key="e3"),
    )
    checks.append(
        TraceOperationCheck(
            "execute_superseded", stale.proposal_id, "REJECTED", actual, code
        )
    )

    report = TraceReport(
        scenario="APPROVAL_INVALID",
        incident_id="sitinc_e2e_1",
        nodes=[
            TraceNode(kind="INCIDENT", id="sitinc_e2e_1", status="ONGOING"),
            TraceNode(kind="COUNCIL", id=view.council_id, status=view.status),
            TraceNode(kind="PROPOSAL", id=proposal.proposal_id, status="E1_VALID"),
            TraceNode(
                kind="APPROVAL", id=approval.approval_id, status="PENDING→EXPIRED/SUPERSEDED"
            ),
        ],
        edge_checks=[
            # 所有失效路径都不允许产生 pending action / outcome
            TraceEdgeCheck(
                "APPROVAL",
                "PENDING_ACTION",
                "ABSENT",
                "PRESENT" if _rows(test_db, NetworkPendingActionRecord) else "ABSENT",
                reason="no_invalid_approval_reaches_l5",
            ),
        ],
        operation_checks=checks,
        completed_at=datetime.now(UTC).isoformat(),
    )
    verdict = verify_trace(report)
    assert verdict.result == "OK", f"trace broken: {verdict.details}"
    assert _rows(test_db, NetworkPendingActionRecord) == []


# ---------------------------------------------------------------------------
# 场景 E：ISOLATION —— 幂等 / 租户 / attempt 边界
# ---------------------------------------------------------------------------


def test_scenario_e_isolation(test_db: Database, context: TenantContext):
    _, view = _convene(test_db, context)
    svc = NetworkActionApprovalService(test_db)
    proposal, approval = _find_proposal(test_db, manual=False)
    svc.decide(
        context,
        proposal.proposal_id,
        approval_decision="APPROVED",
        reason="批准远程下发",
        expected_version=approval.version,
        decider_subject_id="approver-2",
    )

    checks: list[TraceOperationCheck] = []

    # 同幂等键重放 → 幂等成功，不产生第二个 pending action
    svc.execute(context, proposal.proposal_id, idempotency_key="same-key")
    n_before = len(_rows(test_db, NetworkPendingActionRecord))
    r2 = svc.execute(context, proposal.proposal_id, idempotency_key="same-key")
    checks.append(
        TraceOperationCheck(
            "execute_idempotent_replay",
            proposal.proposal_id,
            "ACCEPTED",
            "ACCEPTED" if r2["idempotent_replay"] else "REJECTED",
            detail=f"pending_actions={len(_rows(test_db, NetworkPendingActionRecord))}",
        )
    )
    assert len(_rows(test_db, NetworkPendingActionRecord)) == n_before == 1

    # 异幂等键 → 冲突拒绝
    try:
        svc.execute(context, proposal.proposal_id, idempotency_key="other-key")
        actual = "ACCEPTED"
    except IdempotencyConflict:
        actual = "REJECTED"
    checks.append(
        TraceOperationCheck(
            "execute_conflict_key", proposal.proposal_id, "REJECTED", actual, "IdempotencyConflict"
        )
    )

    # 跨租户 → 不可见
    other = TenantContext(tenant_id="tenant-beta", subject_id="intruder")
    try:
        svc.execute(other, proposal.proposal_id, idempotency_key="x")
        actual = "ACCEPTED"
    except ApprovalNotFound:
        actual = "REJECTED"
    checks.append(
        TraceOperationCheck(
            "execute_cross_tenant", proposal.proposal_id, "REJECTED", actual, "ApprovalNotFound"
        )
    )

    report = TraceReport(
        scenario="ISOLATION",
        incident_id="sitinc_e2e_1",
        nodes=[
            TraceNode(kind="INCIDENT", id="sitinc_e2e_1"),
            TraceNode(kind="COUNCIL", id=view.council_id),
            TraceNode(kind="PROPOSAL", id=proposal.proposal_id),
            TraceNode(kind="APPROVAL", id=approval.approval_id, status="APPROVED"),
            TraceNode(
                kind="PENDING_ACTION",
                id=_rows(test_db, NetworkPendingActionRecord)[0].action_id,
                status="QUEUED",
            ),
        ],
        edge_checks=[
            # 同键重放幂等 → 仍只有 1 条 pending action
            TraceEdgeCheck(
                "APPROVAL",
                "PENDING_ACTION",
                "PRESENT",
                "PRESENT"
                if len(_rows(test_db, NetworkPendingActionRecord)) == 1
                else "ABSENT",
                reason="idempotent_replay_no_duplicate_pending",
            ),
        ],
        operation_checks=checks,
        completed_at=datetime.now(UTC).isoformat(),
    )
    verdict = verify_trace(report)
    assert verdict.result == "OK", f"trace broken: {verdict.details}"


# ---------------------------------------------------------------------------
# T7：reporter / verifier 自测（报告的完整性语义）
# ---------------------------------------------------------------------------


def test_verifier_reports_break_with_first_break() -> None:
    from industrial_ops_agent.network_assurance.e2e_trace import (
        TraceEdgeCheck,
        TraceNode,
        TraceReport,
        verify_trace,
    )

    report = TraceReport(
        scenario="POLICY_BLOCKED",
        incident_id="i1",
        nodes=[TraceNode(kind="PROPOSAL", id="p1")],
        edge_checks=[
            TraceEdgeCheck("ACTION_DECISION", "APPROVAL", "ABSENT", "PRESENT"),
        ],
        operation_checks=[],
    )
    verdict = verify_trace(report)
    assert verdict.result == "BREAK"
    assert verdict.first_break and "ACTION_DECISION" in verdict.first_break


def test_verifier_reports_partial_without_prediction_snapshot() -> None:
    from industrial_ops_agent.network_assurance.e2e_trace import (
        TraceNode,
        TraceReport,
        verify_trace,
    )

    report = TraceReport(
        scenario="REMOTE_HAPPY",
        incident_id="i1",
        nodes=[TraceNode(kind="PREDICTION", id="pred-1")],
        edge_checks=[],
        operation_checks=[],
        prediction_snapshots=[],
    )
    verdict = verify_trace(report)
    assert verdict.result == "PARTIAL"


def test_report_json_roundtrip_is_stable() -> None:
    from industrial_ops_agent.network_assurance.e2e_trace import (
        TraceEdgeCheck,
        TraceNode,
        TraceOperationCheck,
        TraceReport,
    )

    report = TraceReport(
        scenario="ISOLATION",
        incident_id="i1",
        nodes=[TraceNode(kind="PROPOSAL", id="p1", status="E1_VALID")],
        edge_checks=[TraceEdgeCheck("PROPOSAL", "ACTION_DECISION", "PRESENT", "PRESENT")],
        operation_checks=[
            TraceOperationCheck("execute", "p1", "REJECTED", "REJECTED", "IdempotencyConflict")
        ],
        prediction_snapshots=[{"device": "d1", "risk": "LOW"}],
    )
    again = TraceReport.from_json(report.to_json())
    assert again.to_dict() == report.to_dict()


def test_reporter_db_mode_is_partial_not_ok(tmp_path) -> None:
    """--incident-id 模式绝不输出完整 OK（Prediction/被拒操作不可重建）。"""
    import subprocess
    import sys
    from pathlib import Path

    # 1) DB 文件不存在 → 明确报错退出 2（不是静默 OK）
    missing = subprocess.run(
        [
            sys.executable,
            "scripts/verify_end_to_end_trace.py",
            "--incident-id",
            "sitinc_nonexistent",
            "--db-url",
            f"sqlite:///{tmp_path/'missing.db'}",
        ],
        capture_output=True,
        text=True,
        cwd=Path(__file__).resolve().parent.parent,
    )
    assert missing.returncode == 2
    assert "TRACE OK" not in missing.stdout

    # 2) 空 schema 库 → 只有 INCIDENT 节点，结果必须是 PARTIAL（绝不 OK）
    import sqlalchemy as sa

    from industrial_ops_agent.persistence.models import Base

    db_path = tmp_path / "empty.db"
    engine = sa.create_engine(f"sqlite:///{db_path}")
    Base.metadata.create_all(engine)
    engine.dispose()

    result = subprocess.run(
        [
            sys.executable,
            "scripts/verify_end_to_end_trace.py",
            "--incident-id",
            "sitinc_nonexistent",
            "--db-url",
            f"sqlite:///{db_path}",
        ],
        capture_output=True,
        text=True,
        cwd=Path(__file__).resolve().parent.parent,
    )
    assert "TRACE OK" not in result.stdout
    assert "TRACE PARTIAL" in result.stdout
    assert result.returncode == 0

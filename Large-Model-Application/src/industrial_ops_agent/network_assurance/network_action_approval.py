"""Network action approval service (路线图 ⑤, 契约 E2 的后端实现)。

四权分立在本模块的落地：
- **Policy 裁决**由 `action_policy.decide_proposal` 计算（本模块只消费 `ActionDecision`）；
- **人工批准**复用 `ApprovalBindingGuard`（SoD / 版本 / 过期 / 决策闭集——原模块直接调用，
  零复制，`approval.models` 的异常类原样抛出）；
- **执行**：`ExecutionMode.REMOTE_PENDING_ACTION` 走 `queue_action`（载荷来自批准快照，
  消 TOCTOU）；`ExecutionMode.MANUAL_RUNBOOK` 永远 `NOT_EXECUTED`，只产出手册；
- **幂等**：同一批准 + 同一 `idempotency_key` 绝不产生第二个 `network_pending_actions`
  （DB 唯一约束兜底 + 服务层短路）。

命名纪律：Policy 侧叫 `policy_decision`（`network_action_decisions`），人的决定叫
`approval_decision`（`network_action_approval_decisions`）——审计日志永不混用。
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from datetime import UTC, datetime, timedelta
from typing import Any, Final
from uuid import uuid4

from sqlalchemy import select

from industrial_ops_agent.approval.models import ApprovalConflict
from industrial_ops_agent.approval.service import ApprovalBindingGuard
from industrial_ops_agent.domain import as_utc
from industrial_ops_agent.network_assurance.action_policy import (
    ActionDecision,
    ExecutionMode,
)
from industrial_ops_agent.network_assurance.council_contracts import ActionProposal
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import (
    NetworkActionApprovalDecisionRecord,
    NetworkActionApprovalRecord,
    NetworkActionDecisionRecord,
    NetworkCouncilProposalRecord,
    NetworkCouncilRecord,
)
from industrial_ops_agent.persistence.tenant import TenantContext

#: MANUAL 运行手册的渲染版本（工程细节③：与批准快照一同固化，
#: 未来渲染器升级不改变旧批准查看时的语义）。
RUNBOOK_RENDERER_VERSION: Final[str] = "runbook-v1"

#: 与平台 T2 审批一致的 24h 有效期。
APPROVAL_TTL: Final[timedelta] = timedelta(hours=24)


class ApprovalNotFound(LookupError):
    """proposal_id 不存在、对当前租户不可见、或提案被 Policy 拦截而无审批对象。

    BLOCKED 提案的"为什么被拦"应查 ``network_action_decisions.block_reason``，
    Approval 层不兼任 Policy 状态查询（终审：404 语义不区分"不存在"与"被拦"）。
    """


class ManualExecutionNotAllowed(RuntimeError):
    """MANUAL_RUNBOOK 的批准不能通过 execute 执行（P0-3）。"""


class IdempotencyConflict(RuntimeError):
    """同一批准换用不同幂等键 → 409（同键重复请求由服务层短路返回原结果）。"""


def proposal_digest(proposal: ActionProposal) -> str:
    """LLM 建议的不可变摘要（进 proposal/decision 两级记录）。"""
    payload = json.dumps(
        proposal.model_dump(mode="json"),
        sort_keys=True,
        ensure_ascii=False,
        separators=(",", ":"),
    )
    return "sha256:" + hashlib.sha256(payload.encode()).hexdigest()


@dataclass(frozen=True, slots=True)
class ProposalStateView:
    """列表端点的行视图：建议 + 最新裁决 + 审批（可能无）。"""

    proposal_id: str
    proposal_index: int
    proposal_snapshot: dict[str, Any]
    decision: dict[str, Any] | None
    approval: dict[str, Any] | None


class NetworkActionApprovalService:
    def __init__(self, database: Database) -> None:
        self._database = database

    # ------------------------------------------------------------------
    # 会商成功后的批量落库（由 NetworkCouncilService 调用）
    # ------------------------------------------------------------------

    def record_council_outcomes(
        self,
        context: TenantContext,
        *,
        council: NetworkCouncilRecord,
        proposals: list[ActionProposal],
        decisions: list[ActionDecision],
        initiated_by: str,
    ) -> list[str]:
        """持久化：**所有** E1 提案（含 BLOCKED）→ 裁决 → 仅 allowed 建审批。

        force 新 attempt 时，把本会话旧的未决审批置 SUPERSEDED（工程细节：
        被新 attempt 取代 ≠ TTL 过期），已执行的保留执行事实。
        """
        assert len(proposals) == len(decisions)
        now = datetime.now(UTC)
        minted: list[str] = []

        with self._database.transaction(context) as session:
            # force 重跑：旧的未决/未执行批准标记为被取代
            stale = session.scalars(
                select(NetworkActionApprovalRecord).where(
                    NetworkActionApprovalRecord.tenant_id == context.tenant_id,
                    NetworkActionApprovalRecord.council_id == council.council_id,
                    NetworkActionApprovalRecord.status.in_(("PENDING", "APPROVED")),
                    NetworkActionApprovalRecord.executed_at.is_(None),
                    NetworkActionApprovalRecord.attempt < council.attempt_count,
                )
            ).all()
            for row in stale:
                row.status = "SUPERSEDED"
                row.version += 1

            for index, (proposal, decision) in enumerate(zip(proposals, decisions, strict=True)):
                p_digest = proposal_digest(proposal)
                # proposal_id 是**事件身份**（哪次 attempt 的第几条建议），
                # proposal_digest 是**内容身份**（建议内容是否相同）。
                # 不同 attempt 产出相同内容的提案 = 两个不同的 proposal_id、
                # 相同的 proposal_digest——刻意如此，不要把 id 改成 digest 型。
                proposal_id = (
                    f"ncprop-{council.council_id.removeprefix('ncouncil-')[:16]}"
                    f"-a{council.attempt_count}-{index}"
                )
                session.add(
                    NetworkCouncilProposalRecord(
                        proposal_id=proposal_id,
                        tenant_id=context.tenant_id,
                        council_id=council.council_id,
                        attempt=council.attempt_count,
                        proposal_index=index,
                        proposal_snapshot=proposal.model_dump(mode="json"),
                        proposal_digest=p_digest,
                    )
                )
                decision_id = (
                    "ncdec-"
                    + hashlib.sha1(
                        (proposal_id + decision.decision_digest()).encode()
                    ).hexdigest()[:24]
                )
                session.add(
                    NetworkActionDecisionRecord(
                        decision_id=decision_id,
                        proposal_id=proposal_id,
                        tenant_id=context.tenant_id,
                        council_id=council.council_id,
                        attempt=council.attempt_count,
                        proposal_digest=p_digest,
                        normalized_action=decision.normalized_action.canonical(),
                        normalized_action_digest=decision.normalized_action.digest(),
                        allowed=decision.allowed,
                        risk=str(decision.risk),
                        required_preconditions=list(decision.required_preconditions),
                        approval_required=decision.approval_required,
                        block_reason=decision.block_reason,
                        execution_mode=str(decision.execution_mode),
                        catalog_version=decision.catalog_version,
                        risk_policy_version=decision.risk_policy_version,
                        decision_digest=decision.decision_digest(),
                    )
                )
                minted.append(proposal_id)

                if not decision.allowed:
                    # P0-2：Policy 准入被拒 → 没有可批准的对象
                    continue

                approval_id = f"ncapp-{hashlib.sha1(proposal_id.encode()).hexdigest()[:24]}"
                session.add(
                    NetworkActionApprovalRecord(
                        approval_id=approval_id,
                        proposal_id=proposal_id,
                        tenant_id=context.tenant_id,
                        council_id=council.council_id,
                        attempt=council.attempt_count,
                        decision_id=decision_id,
                        decision_digest=decision.decision_digest(),
                        normalized_action=decision.normalized_action.canonical(),
                        approval_payload_digest=decision.normalized_action.digest(),
                        runbook_renderer_version=RUNBOOK_RENDERER_VERSION,
                        status="PENDING",
                        initiated_by=initiated_by[:128],
                        expires_at=now + APPROVAL_TTL,
                        version=1,
                        execution_status="NOT_EXECUTED",
                        manual_execution_required=(
                            decision.execution_mode is ExecutionMode.MANUAL_RUNBOOK
                        ),
                    )
                )
            session.flush()
        return minted

    # ------------------------------------------------------------------
    # 列表
    # ------------------------------------------------------------------

    def list_proposals(self, context: TenantContext, council_id: str) -> list[ProposalStateView]:
        now = datetime.now(UTC)
        with self._database.transaction(context) as session:
            proposals = session.scalars(
                select(NetworkCouncilProposalRecord).where(
                    NetworkCouncilProposalRecord.tenant_id == context.tenant_id,
                    NetworkCouncilProposalRecord.council_id == council_id,
                )
            ).all()
            out: list[ProposalStateView] = []
            for proposal in proposals:
                decision_row = session.scalars(
                    select(NetworkActionDecisionRecord)
                    .where(
                        NetworkActionDecisionRecord.tenant_id == context.tenant_id,
                        NetworkActionDecisionRecord.proposal_id == proposal.proposal_id,
                    )
                    .order_by(NetworkActionDecisionRecord.created_at.desc())
                    .limit(1)
                ).first()
                approval_row = session.scalars(
                    select(NetworkActionApprovalRecord).where(
                        NetworkActionApprovalRecord.tenant_id == context.tenant_id,
                        NetworkActionApprovalRecord.proposal_id == proposal.proposal_id,
                    )
                ).first()
                if (
                    approval_row is not None
                    and approval_row.status == "PENDING"
                    and as_utc(approval_row.expires_at) <= now
                ):
                    approval_row.status = "EXPIRED"
                    approval_row.version += 1
                out.append(
                    ProposalStateView(
                        proposal_id=proposal.proposal_id,
                        proposal_index=proposal.proposal_index,
                        proposal_snapshot=dict(proposal.proposal_snapshot),
                        decision=_decision_dict(decision_row) if decision_row else None,
                        approval=_approval_dict(approval_row) if approval_row else None,
                    )
                )
            out.sort(key=lambda v: v.proposal_index)
            return out

    # ------------------------------------------------------------------
    # 人的批准（approval_decision）
    # ------------------------------------------------------------------

    def _expire_if_due(
        self, context: TenantContext, proposal_id: str
    ) -> None:
        """把到期 PENDING 审批落库为 EXPIRED（独立事务，先于 decide/execute/runbook）。

        必须在**主事务之前**做：Guard 拒绝对象时主事务回滚，若 lazy expire 写在
        同一事务里，EXPIRED 会被一并回滚——DB 里就永远是 PENDING（终审第 4 项）。
        """
        now = datetime.now(UTC)
        with self._database.transaction(context) as session:
            approval = self._load_approval(session, context, proposal_id)
            if approval is None:
                return
            if approval.status == "PENDING" and as_utc(approval.expires_at) <= now:
                approval.status = "EXPIRED"
                approval.version += 1
                session.flush()

    def decide(
        self,
        context: TenantContext,
        proposal_id: str,
        *,
        approval_decision: str,
        reason: str,
        expected_version: int,
        decider_subject_id: str,
    ) -> dict[str, Any]:
        """APPROVE / REJECT。SoD、版本、过期全部由 ApprovalBindingGuard 判定。

        注意参数命名：本方法产出的是 ``approval_decision``（人裁），
        与 Policy 的 ``policy_decision`` 永不混名。
        """
        self._expire_if_due(context, proposal_id)
        now = datetime.now(UTC)
        with self._database.transaction(context) as session:
            approval = self._require_approval(session, context, proposal_id)
            proposal = session.get(NetworkCouncilProposalRecord, approval.proposal_id)
            council = session.get(NetworkCouncilRecord, approval.council_id)
            if proposal is None or council is None:  # pragma: no cover - FK 保证
                raise ApprovalNotFound(proposal_id)

            # P0-2 防线：blocked 提案没有 approval 行 → _require_approval 天然 404。
            # 状态/过期/版本/决策闭集全部交给 guard（零复制语义）。
            ApprovalBindingGuard.require_decidable(
                initiator_subject_id=council.requested_by_subject_id,
                decider_subject_id=decider_subject_id,
                current_status=approval.status,
                current_version=approval.version,
                expected_version=expected_version,
                expires_at=as_utc(approval.expires_at),
                now=now,
                decision=approval_decision,
            )

            decision_row_id = f"ncad-{uuid4().hex[:20]}"
            session.add(
                NetworkActionApprovalDecisionRecord(
                    approval_decision_id=decision_row_id,
                    tenant_id=context.tenant_id,
                    approval_id=approval.approval_id,
                    approval_decision=approval_decision,
                    reason=reason[:500],
                    decider_subject_id=decider_subject_id[:128],
                    approval_payload_digest=approval.approval_payload_digest,
                    decided_at=now,
                )
            )
            approval.status = approval_decision
            approval.version += 1
            if approval_decision == "APPROVED":
                approval.approved_by_subject_id = decider_subject_id[:128]
                approval.approved_at = now
            session.flush()
            return _approval_dict(approval)

    # ------------------------------------------------------------------
    # 执行（仅 REMOTE）
    # ------------------------------------------------------------------

    def execute(
        self,
        context: TenantContext,
        proposal_id: str,
        *,
        idempotency_key: str,
    ) -> dict[str, Any]:
        """CONFIG_CHANGE + APPROVED → queue_action（载荷来自批准快照，P1-1）。

        - MANUAL_RUNBOOK → `ManualExecutionNotAllowed`（API 层映射 409，P0-3）；
        - 幂等：同一批准 + 同一 key → 短路返回原结果；不同 key → 冲突；
        - 参数不收调用方任何值——批准什么就执行什么（消 TOCTOU）。
        """
        self._expire_if_due(context, proposal_id)
        now = datetime.now(UTC)
        with self._database.transaction(context) as session:
            approval = self._require_approval(session, context, proposal_id)

            if approval.manual_execution_required:
                raise ManualExecutionNotAllowed(proposal_id)

            # 已执行的幂等短路 / 冲突
            if approval.executed_idempotency_key:
                if approval.executed_idempotency_key == idempotency_key[:64]:
                    return {
                        "execution_status": approval.execution_status,
                        "queued_action_id": approval.queued_action_id,
                        "idempotent_replay": True,
                    }
                raise IdempotencyConflict(proposal_id)

            # 执行门禁（平台同一 guard：状态/过期/参数哈希）
            rejection = ApprovalBindingGuard.execution_rejection(
                status=approval.status,
                current_version=approval.version,
                expected_version=approval.version,
                expires_at=as_utc(approval.expires_at),
                now=now,
                approved_parameters_hash=approval.approval_payload_digest,
                supplied_parameters_hash=approval.approval_payload_digest,
            )
            if rejection is not None:
                raise ApprovalConflict(rejection)

            normalized = approval.normalized_action
            config_key = str(normalized.get("config_key") or "")
            config_value = str(normalized.get("config_value") or "")
            if not config_key:
                # APPROVED 却不是 config 类——只能是 MANUAL（已拦），防御分支
                raise ManualExecutionNotAllowed(proposal_id)

            from industrial_ops_agent.network_assurance.service import (
                NetworkAssuranceService,
            )

            service = NetworkAssuranceService(self._database)
            queued_action_id = service.queue_action(
                context,
                str(normalized.get("target_gateway") or ""),
                config_key=config_key,
                config_value=config_value,
                issued_by=approval.initiated_by,
                approved_by=approval.approved_by_subject_id,  # 只来自 decision 行
            )
            approval.execution_status = "QUEUED"
            approval.queued_action_id = queued_action_id
            approval.executed_idempotency_key = idempotency_key[:64]
            approval.executed_at = now
            approval.version += 1
            session.flush()
            return {
                "execution_status": approval.execution_status,
                "queued_action_id": queued_action_id,
                "idempotent_replay": False,
            }

    # ------------------------------------------------------------------
    # 受控运行手册（仅 MANUAL）
    # ------------------------------------------------------------------

    def runbook(
        self, context: TenantContext, proposal_id: str
    ) -> dict[str, Any]:
        """从**批准快照**生成运行手册（工程细节②/③：不按当前 Catalog 重解释）。

        返回体显式携带执行语义，UI 永远不能把"已批准手工执行"渲染成"已执行"。
        """
        self._expire_if_due(context, proposal_id)
        with self._database.transaction(context) as session:
            approval = self._require_approval(session, context, proposal_id)
            if not approval.manual_execution_required:
                raise ManualExecutionNotAllowed(
                    f"{proposal_id}: not a manual-runbook proposal"
                )
            if approval.status != "APPROVED":
                # 未批准不给手册（PENDING 先走 decide；REJECTED/EXPIRED/SUPERSEDED 同理）
                raise ApprovalConflict(f"approval is {approval.status}, not APPROVED")

            normalized = approval.normalized_action
            action_id = str(normalized.get("action_id") or "")
            params = dict(normalized.get("action_params") or {})
            argv = ["weaknet", "action", action_id]
            # 参数按 key 排序保证渲染稳定；输出为可直接粘贴的命令
            for key in sorted(params):
                argv.extend([key, str(params[key])])
            command = "sudo " + " ".join(argv)

            return {
                "proposal_id": proposal_id,
                "approval_id": approval.approval_id,
                "command": command,
                "catalog_version": normalized.get("catalog_version"),
                "approved_by": approval.approved_by_subject_id,
                "approved_at": (
                    approval.approved_at.isoformat() if approval.approved_at else None
                ),
                "runbook_renderer_version": approval.runbook_renderer_version,
                "manual_execution_required": True,
                "execution_status": approval.execution_status,
            }

    # ------------------------------------------------------------------
    # 内部
    # ------------------------------------------------------------------

    @staticmethod
    def _load_approval(
        session: Any, context: TenantContext, proposal_id: str
    ) -> NetworkActionApprovalRecord | None:
        return session.scalars(
            select(NetworkActionApprovalRecord).where(
                NetworkActionApprovalRecord.tenant_id == context.tenant_id,
                NetworkActionApprovalRecord.proposal_id == proposal_id,
            )
        ).first()

    @classmethod
    def _require_approval(
        cls, session: Any, context: TenantContext, proposal_id: str
    ) -> NetworkActionApprovalRecord:
        row = cls._load_approval(session, context, proposal_id)
        if row is None:
            raise ApprovalNotFound(proposal_id)
        return row


def _decision_dict(row: NetworkActionDecisionRecord | None) -> dict[str, Any] | None:
    if row is None:
        return None
    return {
        "decision_id": row.decision_id,
        "allowed": row.allowed,
        "risk": row.risk,
        "required_preconditions": list(row.required_preconditions or []),
        "approval_required": row.approval_required,
        "block_reason": row.block_reason,
        "execution_mode": row.execution_mode,
        "catalog_version": row.catalog_version,
        "risk_policy_version": row.risk_policy_version,
        "normalized_action": dict(row.normalized_action or {}),
    }


def _approval_dict(row: NetworkActionApprovalRecord | None) -> dict[str, Any] | None:
    if row is None:
        return None
    return {
        "approval_id": row.approval_id,
        "status": row.status,
        "expires_at": row.expires_at.isoformat(),
        "version": row.version,
        "execution_status": row.execution_status,
        "manual_execution_required": row.manual_execution_required,
        "queued_action_id": row.queued_action_id,
        "approved_by": row.approved_by_subject_id,
        "approved_at": (
            row.approved_at.isoformat() if row.approved_at else None
        ),
        "decision_id": row.decision_id,
    }

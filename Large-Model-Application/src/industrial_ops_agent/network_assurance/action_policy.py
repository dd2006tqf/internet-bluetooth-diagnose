"""Policy layer for Network Council action proposals (路线图 ⑤, 契约 E1 → 决定权)。

四权分立中属于**准入权**的两段：`ActionPolicyValidator`（合法性复核）与
`RiskPolicy`（情境风险与审批要求）。本模块回答两件事：

1. 这条建议**技术上可执行吗**（catalog 版本一致？动作/键/参数在受审阅集合内？）
2. 在当前情境下**风险多大、是否必须人批、走哪条执行通道**（`ActionDecision`）

纪律（代码即契约）：

- **`risk` / `approval_required` / `allowed` 永远不是 LLM 的输出**——它们由本模块的
  确定性规则计算；`CouncilDecision` 里根本没有这些字段（④ closed schema 已保证）。
- **v1：凡 `allowed` 的提案一律 `approval_required=True`**——下发到板端（远程调参）或
  生成受控运行手册都是执行动作，均由人批准。任何免审批都是**正式修改 E2 契约**。
- `allowed=False` 的提案止步于 `ActionDecision` 审计行，**绝不创建 ApprovalRequest**
  ——否则 UI 会同时显示"Policy：禁止执行"与"审批：请批准"的产品语义冲突。

**命名约定（工程细节②）**：本模块产出叫 `policy_decision`（Policy 裁决）；人的批准叫
`approval_decision`。两者在代码、审计日志与 UI 上永不混用。
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from enum import StrEnum
from typing import Final

from industrial_ops_agent.network_assurance.council_contracts import (
    ActionProposal,
    ActionProposalKind,
)
from industrial_ops_agent.network_assurance.edge_action_catalog import (
    TOP_LEVEL_CATALOG_VERSION,
    find_action,
    is_known_config_key,
    validate_action_params,
)
from industrial_ops_agent.network_assurance.wireless_contracts import RiskLevel

#: RiskPolicy 版本：进 ActionDecision、审批快照与审计 digest（P1-5）。
#: 规则变化必须升版——历史审批据此可答"当时依据哪版策略裁决"。
RISK_POLICY_VERSION: Final[str] = "risk-policy-v1"

#: v1 铁律：所有 allowed 提案一律需人工审批（用户拍板，E2 契约未修改前不得放宽）。
V1_APPROVAL_REQUIRED_FOR_ALL: Final[bool] = True


class ExecutionMode(StrEnum):
    """批准后的执行通道。"""

    REMOTE_PENDING_ACTION = "REMOTE_PENDING_ACTION"  # CONFIG_CHANGE → queue_action → 板端
    MANUAL_RUNBOOK = "MANUAL_RUNBOOK"                # ACTION_ID → 受控手册，人到板端执行


def _execution_mode_for(kind: ActionProposalKind) -> ExecutionMode:
    return (
        ExecutionMode.REMOTE_PENDING_ACTION
        if kind is ActionProposalKind.CONFIG_CHANGE
        else ExecutionMode.MANUAL_RUNBOOK
    )


@dataclass(frozen=True, slots=True)
class NormalizedAction:
    """P1-1：审批绑定的**最终规范化载荷**（不是 LLM 原文）。

    ``approval_payload_digest`` 对本对象的 canonical JSON 取哈希；批准之后，
    执行侧只能使用快照里的这份载荷——彻底消除"批准 1000、执行时改成 500"
    的 TOCTOU 空间。
    """

    target_gateway: str
    kind: ActionProposalKind
    action_id: str | None
    action_params: dict[str, str]
    config_key: str | None
    config_value: str | None
    catalog_version: str

    def canonical(self) -> dict[str, object]:
        return {
            "target_gateway": self.target_gateway,
            "kind": str(self.kind),
            "action_id": self.action_id,
            "action_params": dict(sorted(self.action_params.items())),
            "config_key": self.config_key,
            "config_value": self.config_value,
            "catalog_version": self.catalog_version,
        }

    def digest(self) -> str:
        return "sha256:" + hashlib.sha256(
            json.dumps(self.canonical(), sort_keys=True, ensure_ascii=False,
                       separators=(",", ":")).encode()
        ).hexdigest()


@dataclass(frozen=True, slots=True)
class ActionDecision:
    """Policy 的裁决（policy_decision）。

    与人的 ``approval_decision`` 严格区分：前者是机器按 RISK_POLICY_VERSION
    规则给出的准入结论，后者记录谁、何时、为什么批准。
    """

    proposal_digest: str
    normalized_action: NormalizedAction
    allowed: bool
    risk: RiskLevel
    required_preconditions: list[str]
    approval_required: bool
    block_reason: str | None
    execution_mode: ExecutionMode
    catalog_version: str
    risk_policy_version: str

    def decision_digest(self) -> str:
        payload = {
            "proposal_digest": self.proposal_digest,
            "normalized_action": self.normalized_action.canonical(),
            "allowed": self.allowed,
            "risk": str(self.risk),
            "required_preconditions": list(self.required_preconditions),
            "approval_required": self.approval_required,
            "block_reason": self.block_reason,
            "execution_mode": str(self.execution_mode),
            "catalog_version": self.catalog_version,
            "risk_policy_version": self.risk_policy_version,
        }
        return "sha256:" + hashlib.sha256(
            json.dumps(payload, sort_keys=True, ensure_ascii=False,
                       separators=(",", ":")).encode()
        ).hexdigest()


# ---------------------------------------------------------------------------
# 风险表（确定性、可重放；改动必须升 RISK_POLICY_VERSION）
# ---------------------------------------------------------------------------

_READ_ONLY_ACTIONS = frozenset(
    {"CHECK_RESOLVER_CONFIG", "INSPECT_DEFAULT_GATEWAY", "PROBE_PUBLIC_RESOLVER"}
)
_MUTATING_ACTIONS = frozenset({"RESTART_NETWORK_INTERFACE"})
_SAMPLING_KEY_HINTS = ("interval", "timeout", "window")


def _risk_for_action(action_id: str) -> RiskLevel:
    if action_id in _READ_ONLY_ACTIONS:
        return RiskLevel.LOW
    if action_id in _MUTATING_ACTIONS:
        return RiskLevel.HIGH
    # 未在已知集合里——按保守处理（allowed 也会被拦，这里只是字段完整）
    return RiskLevel.MEDIUM


def _risk_for_config(config_key: str) -> RiskLevel:
    if any(hint in config_key for hint in _SAMPLING_KEY_HINTS):
        return RiskLevel.LOW
    return RiskLevel.MEDIUM


def _preconditions(action: ActionProposal, risk: RiskLevel) -> list[str]:
    pre: list[str] = []
    if action.kind is ActionProposalKind.ACTION_ID and action.action_id in _MUTATING_ACTIONS:
        pre.append("确认目标接口在未来 5 分钟内无关键生产流量")
        pre.append("记录接口当前状态以便回滚")
    if risk is RiskLevel.HIGH:
        pre.append("确认执行窗口处于非生产高峰")
    if risk in (RiskLevel.MEDIUM, RiskLevel.HIGH):
        pre.append("记录变更前参数值以便回滚")
    return pre


def decide_proposal(
    proposal: ActionProposal,
    *,
    proposal_digest: str,
    target_gateway: str,
    production_critical: bool,
    cloud_catalog_version: str = TOP_LEVEL_CATALOG_VERSION,
    gateway_catalog_version: str,
) -> ActionDecision:
    """确定性 Policy 裁决（纯函数，可重放）。

    拒绝理由按序检查，第一条命中即 ``allowed=False``：
    catalog 版本漂移 → 动作/键不存在 → 参数越界 → 其余一律放行进审批。
    """
    mode = _execution_mode_for(proposal.kind)
    normalized = NormalizedAction(
        target_gateway=target_gateway,
        kind=proposal.kind,
        action_id=proposal.action_id,
        action_params=dict(proposal.action_params),
        config_key=proposal.config_key,
        config_value=proposal.config_value,
        catalog_version=cloud_catalog_version,
    )

    block_reason: str | None = None
    if cloud_catalog_version != gateway_catalog_version:
        # 部署版本漂移：云端审阅目录 ≠ 目标网关宣称目录 → 不允许执行（P1-2 语义）
        block_reason = (
            f"catalog_version_mismatch: cloud={cloud_catalog_version} "
            f"gateway={gateway_catalog_version}"
        )
    elif proposal.kind is ActionProposalKind.ACTION_ID:
        action_id = proposal.action_id or ""
        if find_action(action_id) is None:
            block_reason = f"unknown_action_id: {action_id}"
        else:
            param_error = validate_action_params(action_id, dict(proposal.action_params))
            if param_error is not None:
                block_reason = param_error
    else:
        assert proposal.config_key is not None
        if not is_known_config_key(proposal.config_key):
            block_reason = f"unknown_config_key: {proposal.config_key}"

    # 风险计算（与阻断无关——BLOCKED 记录同样保留当时的风险评估供审计）
    if proposal.kind is ActionProposalKind.ACTION_ID:
        risk = _risk_for_action(proposal.action_id or "")
    else:
        risk = _risk_for_config(proposal.config_key or "")
    if production_critical and risk is RiskLevel.LOW:
        risk = RiskLevel.MEDIUM  # 生产关键情境：风险下限抬到 MEDIUM

    allowed = block_reason is None
    return ActionDecision(
        proposal_digest=proposal_digest,
        normalized_action=normalized,
        allowed=allowed,
        risk=risk,
        required_preconditions=_preconditions(proposal, risk),
        approval_required=V1_APPROVAL_REQUIRED_FOR_ALL if allowed else False,
        block_reason=block_reason,
        execution_mode=mode,
        catalog_version=cloud_catalog_version,
        risk_policy_version=RISK_POLICY_VERSION,
    )

"""S1 tests: ActionPolicy / RiskPolicy 纯函数裁决（路线图 ⑤）。

核心不变量：
- `risk` / `approval_required` / `allowed` 是 Policy 确定性计算的结果，不是模型输出；
- v1 凡 allowed 必须人工审批（`V1_APPROVAL_REQUIRED_FOR_ALL`）；
- catalog 版本漂移行使准入权（⑤ 首次真正拦截）；
- 同一输入同一 RiskPolicy 版本 → 同一 `decision_digest`（可重放）。
"""

from __future__ import annotations

from industrial_ops_agent.network_assurance.action_policy import (
    RISK_POLICY_VERSION,
    V1_APPROVAL_REQUIRED_FOR_ALL,
    ExecutionMode,
    decide_proposal,
)
from industrial_ops_agent.network_assurance.council_contracts import (
    ActionProposal,
    ActionProposalKind,
    SourceBinding,
    SourceBindingKind,
)
from industrial_ops_agent.network_assurance.edge_action_catalog import TOP_LEVEL_CATALOG_VERSION
from industrial_ops_agent.network_assurance.wireless_contracts import RiskLevel

GATEWAY = "radxa-cubie-a7a"


def _proposal(action_id: str | None = "CHECK_RESOLVER_CONFIG", **over: object) -> ActionProposal:
    base: dict[str, object] = {
        "kind": ActionProposalKind.ACTION_ID if action_id else ActionProposalKind.CONFIG_CHANGE,
        "action_id": action_id,
        "rationale": "现有诊断指向解析链路",
        "source_bindings": [
            SourceBinding(kind=SourceBindingKind.CANONICAL_DIAGNOSIS, ref_id="sha256:diag-1")
        ],
    }
    if action_id is None:
        base.update(
            {
                "action_id": None,
                "config_key": over.pop("config_key", "rtt.interval"),
                "config_value": str(over.pop("config_value", "5s")),
            }
        )
    base.update(over)
    return ActionProposal(**base)  # type: ignore[arg-type]


def _decide(proposal: ActionProposal, **kw: object):
    defaults: dict[str, object] = {
        "proposal_digest": "sha256:pd-1",
        "target_gateway": GATEWAY,
        "production_critical": False,
        "cloud_catalog_version": TOP_LEVEL_CATALOG_VERSION,
        "gateway_catalog_version": TOP_LEVEL_CATALOG_VERSION,
    }
    defaults.update(kw)
    return decide_proposal(proposal, **defaults)  # type: ignore[arg-type]


# ---------------------------------------------------------------------------
# 风险表与执行通道
# ---------------------------------------------------------------------------


def test_read_only_action_is_low_and_manual() -> None:
    decision = _decide(_proposal("CHECK_RESOLVER_CONFIG"))
    assert decision.allowed is True
    assert decision.risk is RiskLevel.LOW
    assert decision.execution_mode is ExecutionMode.MANUAL_RUNBOOK
    assert decision.approval_required is True  # v1 全量审批


def test_mutating_action_is_high() -> None:
    decision = _decide(
        _proposal("RESTART_NETWORK_INTERFACE", action_params={"interface": "wlan0"})
    )
    assert decision.allowed is True
    assert decision.risk is RiskLevel.HIGH
    assert decision.execution_mode is ExecutionMode.MANUAL_RUNBOOK
    assert any("回滚" in p for p in decision.required_preconditions)


def test_sampling_config_is_low_remote() -> None:
    decision = _decide(_proposal(None, config_key="rtt.interval", config_value="5s"))
    assert decision.allowed is True
    assert decision.risk is RiskLevel.LOW
    assert decision.execution_mode is ExecutionMode.REMOTE_PENDING_ACTION


def test_target_config_is_medium() -> None:
    decision = _decide(_proposal(None, config_key="rtt.target", config_value="8.8.8.8"))
    assert decision.risk is RiskLevel.MEDIUM


def test_production_critical_raises_floor_to_medium() -> None:
    decision = _decide(_proposal("CHECK_RESOLVER_CONFIG"), production_critical=True)
    assert decision.risk is RiskLevel.MEDIUM  # LOW 被抬升


def test_v1_always_requires_approval() -> None:
    assert V1_APPROVAL_REQUIRED_FOR_ALL is True
    for p in (
        _proposal("CHECK_RESOLVER_CONFIG"),
        _proposal(None, config_key="rtt.interval", config_value="5s"),
    ):
        assert _decide(p).approval_required is True


# ---------------------------------------------------------------------------
# 准入权（allowed=false 的四种拦法）
# ---------------------------------------------------------------------------


def test_catalog_version_mismatch_blocks() -> None:
    decision = _decide(_proposal("CHECK_RESOLVER_CONFIG"), gateway_catalog_version="cat-old")
    assert decision.allowed is False
    assert "catalog_version_mismatch" in (decision.block_reason or "")
    assert decision.approval_required is False  # BLOCKED 不进入审批


def test_unknown_action_id_blocks() -> None:
    # E1 已拦；这里是 Policy 的复核层（belt and suspenders）
    decision = _decide(
        ActionProposal(
            kind=ActionProposalKind.ACTION_ID,
            action_id="REBOOT_FACTORY_NETWORK_STACK",
            rationale="x",
            source_bindings=[
                SourceBinding(kind=SourceBindingKind.CANONICAL_DIAGNOSIS, ref_id="sha256:d")
            ],
        )
    )
    assert decision.allowed is False
    assert "unknown_action_id" in (decision.block_reason or "")


def test_unreviewed_param_blocks() -> None:
    decision = _decide(
        _proposal("PROBE_PUBLIC_RESOLVER", action_params={"resolver": "1.1.1.1"})
    )
    assert decision.allowed is False
    assert "not in reviewed set" in (decision.block_reason or "")


def test_unknown_config_key_blocks() -> None:
    decision = _decide(_proposal(None, config_key="rtt.nonexistent", config_value="1"))
    assert decision.allowed is False
    assert "unknown_config_key" in (decision.block_reason or "")


# ---------------------------------------------------------------------------
# 可重放与版本
# ---------------------------------------------------------------------------


def test_decision_is_deterministic_and_versioned() -> None:
    a = _decide(_proposal("CHECK_RESOLVER_CONFIG"))
    b = _decide(_proposal("CHECK_RESOLVER_CONFIG"))
    assert a.decision_digest() == b.decision_digest()
    assert a.risk_policy_version == RISK_POLICY_VERSION
    assert a.catalog_version == TOP_LEVEL_CATALOG_VERSION
    # normalized_action 是审批绑定的规范化载荷
    assert a.normalized_action.digest().startswith("sha256:")
    assert a.normalized_action.target_gateway == GATEWAY


def test_blocked_decision_still_carries_risk_for_audit() -> None:
    """即使被拦，风险评估仍留档——审计要能回答'当时评了什么风险'。"""
    decision = _decide(
        _proposal("RESTART_NETWORK_INTERFACE", action_params={"interface": "wlan0"}),
        gateway_catalog_version="cat-old",
    )
    assert decision.allowed is False
    assert decision.risk is RiskLevel.HIGH
    assert decision.block_reason

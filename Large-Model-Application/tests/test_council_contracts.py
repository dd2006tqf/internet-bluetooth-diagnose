"""N1 contract tests for the Network Operations Council (路线图 ④, E1 边界).

The whole point of `ActionProposal` being a *closed* model is that an LLM cannot
overstep even if it tries: `hypothesis`, `observed_pattern`, `risk` and
`approval_required` are not fields, so an overreaching output fails at **parse
time** rather than being caught later by a keyword guardrail.

(契约 E1: Council 只有建议权 —— 建议"做什么"，不判决"风险多大"、不决定"是否需批准"，
 也不再下一次诊断。)
"""

from __future__ import annotations

from datetime import UTC, datetime

import pytest
from pydantic import ValidationError

from industrial_ops_agent.network_assurance.council_contracts import (
    COUNCIL_CONTRACT_VERSION,
    ActionProposal,
    ActionProposalKind,
    CouncilInput,
    CouncilRole,
    CouncilStatus,
    CouncilView,
    DecisionEvidenceContext,
    ExpertOpinion,
    KnowledgeCaseRef,
    KnowledgeContext,
    OperationalConstraints,
    SourceBinding,
    SourceBindingKind,
)

_NOW = datetime(2026, 10, 4, 12, 0, tzinfo=UTC)

#: 越权字段：Council 结构上就不该有这些
_FORBIDDEN_FIELDS = ("hypothesis", "observed_pattern", "risk", "approval_required")


def _proposal(**overrides: object) -> ActionProposal:
    base: dict[str, object] = {
        "kind": ActionProposalKind.ACTION_ID,
        "action_id": "CHECK_RESOLVER_CONFIG",
        "rationale": "基于现有 DNS 诊断，先确认本机解析器配置",
        "proposed_preconditions": ["现场网络保持在线"],
        "source_bindings": [
            SourceBinding(kind=SourceBindingKind.CANONICAL_DIAGNOSIS, ref_id="diag-incident-1")
        ],
    }
    base.update(overrides)
    return ActionProposal(**base)  # type: ignore[arg-type]


# ---------------------------------------------------------------------------
# ActionProposal：结构上无越权字段
# ---------------------------------------------------------------------------


def test_action_proposal_has_no_authority_overreach_fields() -> None:
    """risk / approval_required / hypothesis / observed_pattern 必须是"字段不存在"。"""
    fields = set(ActionProposal.model_fields)
    for forbidden in _FORBIDDEN_FIELDS:
        assert forbidden not in fields, f"ActionProposal 不应有字段 {forbidden}"


@pytest.mark.parametrize("forbidden", _FORBIDDEN_FIELDS)
def test_action_proposal_rejects_overreach_at_parse_time(forbidden: str) -> None:
    """越权输出在 parse 阶段即失败（类型系统优先，不是护栏兜底）。"""
    with pytest.raises(ValidationError):
        _proposal(**{forbidden: "anything"})


def test_action_proposal_requires_exactly_one_target() -> None:
    # 只有 action_id
    p = _proposal(kind=ActionProposalKind.ACTION_ID)
    assert p.action_id == "CHECK_RESOLVER_CONFIG"
    assert p.config_key is None

    # 只有 config_change
    q = _proposal(
        kind=ActionProposalKind.CONFIG_CHANGE,
        action_id=None,
        config_key="rtt.interval",
        config_value="5s",
    )
    assert q.config_key == "rtt.interval"
    assert q.config_value == "5s"
    assert q.action_id is None

    # 两者都给 → 拒绝
    with pytest.raises(ValidationError):
        _proposal(
            kind=ActionProposalKind.CONFIG_CHANGE,
            config_key="rtt.interval",
            config_value="5s",
        )


def test_action_proposal_requires_source_bindings() -> None:
    """没有来源绑定 = 没有依据的建议 —— 结构上不允许。"""
    with pytest.raises(ValidationError):
        _proposal(source_bindings=[])


def test_source_binding_kind_is_closed_enum() -> None:
    assert {k.value for k in SourceBindingKind} == {
        "L1_FACT",
        "CANONICAL_DIAGNOSIS",
        "DEVICE_PREDICTION",
    }
    with pytest.raises(ValidationError):
        SourceBinding(kind="RAG_CASE", ref_id="x")  # type: ignore[arg-type]


# ---------------------------------------------------------------------------
# CouncilInput：prediction_results 可为空；无 SITE 聚合
# ---------------------------------------------------------------------------


def _input(**overrides: object) -> CouncilInput:
    base: dict[str, object] = {
        "incident_id": "sitinc_demo_1",
        "canonical_diagnosis_digest": "sha256:diag",
        "prediction_results": [],
        "evidence_context": DecisionEvidenceContext(
            diagnosis_evidence_ids=["e1", "e2"],
            prediction_evidence_ids=[],
            rf_facts=[],
            kernel_facts=[],
            device_facts=["e1", "e2"],
        ),
        "operational_constraints": OperationalConstraints(
            affected_assets=["radxa-cubie-a7a"],
            production_critical=False,
            action_catalog_version="cat-abc",
            gateway_catalog_version="cat-abc",
        ),
        "knowledge_context": KnowledgeContext(cases=[]),
    }
    base.update(overrides)
    return CouncilInput(**base)  # type: ignore[arg-type]


def test_council_input_allows_empty_prediction_results() -> None:
    """预测是增强输入，不是召集前置——空列表仍可会商。"""
    payload = _input(prediction_results=[])
    assert payload.prediction_results == []


def test_council_input_has_no_site_level_aggregate_field() -> None:
    """绝不把多台设备风险平均成 Site RiskWindow。"""
    fields = set(CouncilInput.model_fields)
    assert "site_risk_window" not in fields
    assert "site_prediction" not in fields
    assert "aggregate_risk" not in fields


def test_council_input_rejects_unknown_fields() -> None:
    with pytest.raises(ValidationError):
        _input(site_risk_window={"risk_level": "HIGH"})


# ---------------------------------------------------------------------------
# ExpertOpinion：专家只能引用事实，不能产出新的 pattern/hypothesis
# ---------------------------------------------------------------------------


def test_expert_opinion_has_no_diagnosis_fields() -> None:
    fields = set(ExpertOpinion.model_fields)
    for forbidden in ("hypothesis", "observed_pattern", "diagnosis"):
        assert forbidden not in fields


def test_expert_opinion_observation_must_cite_facts() -> None:
    opinion = ExpertOpinion(
        role=CouncilRole.RF_SPECTRUM,
        observations=["K12 显示协议栈未见异常"],
        referenced_fact_ids=["e1"],
        recommendation_direction="优先排除空口干扰",
    )
    assert opinion.referenced_fact_ids == ["e1"]

    with pytest.raises(ValidationError):
        ExpertOpinion(
            role=CouncilRole.RF_SPECTRUM,
            observations=["x"],
            referenced_fact_ids=[],
            recommendation_direction="y",
        )


def test_expert_opinion_role_is_closed() -> None:
    assert {r.value for r in CouncilRole} == {
        "RF_SPECTRUM",
        "KERNEL_STACK",
        "OPS_SAFETY",
        "COORDINATOR",
    }


# ---------------------------------------------------------------------------
# KnowledgeContext：进入指纹 ≠ 成为证据
# ---------------------------------------------------------------------------


def test_knowledge_context_is_separate_from_evidence() -> None:
    """knowledge_context 不得成为 evidence_context / source_bindings 的来源。"""
    knowledge = KnowledgeContext(
        cases=[KnowledgeCaseRef(case_id="case-A", case_version="3")],
        retrieval_strategy_version="rag-v1",
    )
    assert knowledge.cases[0].case_id == "case-A"
    # 结构上就是两类对象：不能把 KnowledgeCaseRef 塞进 SourceBinding
    with pytest.raises(ValidationError):
        SourceBinding(kind="KNOWLEDGE_CASE", ref_id="case-A")  # type: ignore[arg-type]


# ---------------------------------------------------------------------------
# CouncilView：生命周期
# ---------------------------------------------------------------------------


def test_council_status_and_view_shape() -> None:
    assert {s.value for s in CouncilStatus} == {
        "QUEUED",
        "RUNNING",
        "REVIEW_PENDING",
        "FAILED",
    }
    view = CouncilView(
        council_id="council-1",
        incident_id="sitinc_demo_1",
        input_fingerprint="fp-1",
        status=CouncilStatus.REVIEW_PENDING,
        stage="COORDINATOR_DONE",
        proposals=[_proposal()],
        failure_code=None,
        requested_by_subject_id="operator-1",
        created_at=_NOW,
        updated_at=_NOW,
        version=1,
    )
    assert view.proposals[0].action_id == "CHECK_RESOLVER_CONFIG"
    # Council 视图同样不携带任何执行/批准语义
    view_fields = set(CouncilView.model_fields)
    for forbidden in ("approved_by", "approved_at", "executed", "queued_actions"):
        assert forbidden not in view_fields


def test_contract_version_is_declared() -> None:
    assert COUNCIL_CONTRACT_VERSION.startswith("network-council-v")

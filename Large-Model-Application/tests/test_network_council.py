"""N3 tests for the Network Council orchestration (路线图 ④, 止于 E1).

Verified behaviours:

  1. 三位专家并行 → COORDINATOR 收敛 → ActionProposal[]（Mock 模型，无网络）；
  2. **E1 Catalog Validation fail closed**：一个非法动作 → 整次 output 不接受，
     Council = FAILED，绝不静默过滤成"半合法 Council"；
  3. **SourceBinding 只能引用当前 CouncilInput 内既存对象**（伪造 / 跨 incident
     引用一律失败）；
  4. 越权字段由 closed schema 在 parse 阶段拒绝；
  5. 空 prediction_results 仍可会商（预测是增强输入非前置）；
  6. 模型失败 → FAILED，绝不编造 ActionProposal。
"""

from __future__ import annotations

import json
from typing import Any

import pytest

from industrial_ops_agent.network_assurance.council_contracts import (
    FAILURE_CATALOG_VALIDATION,
    FAILURE_MODEL,
    FAILURE_SCHEMA_PARSE,
    FAILURE_SOURCE_BINDING,
    CouncilFailure,
    CouncilInput,
    CouncilRole,
    CouncilStatus,
    DecisionEvidenceContext,
    KnowledgeContext,
    OperationalConstraints,
)
from industrial_ops_agent.network_assurance.edge_action_catalog import catalog_version
from industrial_ops_agent.network_assurance.network_council import (
    NetworkCouncilRunner,
    build_coordinator_payload,
    build_specialist_payload,
    validate_proposals,
)

# ---------------------------------------------------------------------------
# Fixtures（无 DB：Council 编排是纯函数 + 模型调用）
# ---------------------------------------------------------------------------


def make_input(**overrides: Any) -> CouncilInput:
    base: dict[str, Any] = {
        "incident_id": "sitinc_demo_1",
        "canonical_diagnosis_digest": "sha256:diag-1",
        "prediction_results": [],
        "evidence_context": DecisionEvidenceContext(
            diagnosis_evidence_ids=["diag-evidence-1"],
            prediction_evidence_ids=[],
            rf_facts=["rf-1"],
            kernel_facts=["k-1"],
            device_facts=["e1", "e2"],
        ),
        "operational_constraints": OperationalConstraints(
            affected_assets=["radxa-cubie-a7a"],
            production_critical=False,
            action_catalog_version=catalog_version(),
            gateway_catalog_version=catalog_version(),
        ),
        "knowledge_context": KnowledgeContext(cases=[]),
    }
    base.update(overrides)
    return CouncilInput(**base)


def _proposal_payload(action_id: str = "CHECK_RESOLVER_CONFIG", **over: Any) -> dict[str, Any]:
    payload: dict[str, Any] = {
        "kind": "ACTION_ID",
        "action_id": action_id,
        "rationale": "现有诊断指向 DNS 解析链路，先确认本机解析器配置",
        "proposed_preconditions": [],
        "source_bindings": [{"kind": "CANONICAL_DIAGNOSIS", "ref_id": "sha256:diag-1"}],
    }
    payload.update(over)
    return payload


# ---------------------------------------------------------------------------
# 1. Happy path
# ---------------------------------------------------------------------------


def test_happy_path_produces_legal_proposals() -> None:
    council_input = make_input()
    opinions = {
        CouncilRole.RF_SPECTRUM: {
            "observations": ["空口未见显著干扰"],
            "referenced_fact_ids": ["rf-1"],
            "recommendation_direction": "优先排除解析链路",
        },
        CouncilRole.KERNEL_STACK: {
            "observations": ["协议栈无异常"],
            "referenced_fact_ids": ["k-1"],
            "recommendation_direction": "建议核查解析器配置",
        },
        CouncilRole.OPS_SAFETY: {
            "observations": ["只读动作对产线无影响"],
            "referenced_fact_ids": ["e1"],
            "recommendation_direction": "可安全执行只读检查",
        },
    }
    coordinator = {"proposals": [_proposal_payload()]}

    def fake_complete(role: CouncilRole, payload: dict[str, Any]) -> dict[str, Any]:
        if role is CouncilRole.COORDINATOR:
            return coordinator
        return opinions[role]

    result = NetworkCouncilRunner(complete=fake_complete).run(council_input)

    assert result.status is CouncilStatus.REVIEW_PENDING
    assert len(result.proposals) == 1
    assert result.proposals[0].action_id == "CHECK_RESOLVER_CONFIG"
    assert len(result.expert_opinions) == 3
    assert {o.role for o in result.expert_opinions} == set(opinions)
    # 三位专家 + 协调官 = 4 次模型调用；失败一律 raise，故成功结果不含 failure_code
    assert result.model_calls == 4
    assert result.stage == "COORDINATOR_DONE"


# ---------------------------------------------------------------------------
# 2. Catalog validation fail closed
# ---------------------------------------------------------------------------


def test_unknown_action_fails_whole_council() -> None:
    council_input = make_input()
    coordinator = {
        "proposals": [
            _proposal_payload("CHECK_RESOLVER_CONFIG"),
            _proposal_payload("REBOOT_FACTORY_NETWORK_STACK"),  # 不存在
        ]
    }

    def fake_complete(role: CouncilRole, payload: dict[str, Any]) -> dict[str, Any]:
        return coordinator if role is CouncilRole.COORDINATOR else _opinion_payload()

    with pytest.raises(CouncilFailure) as exc:
        NetworkCouncilRunner(complete=fake_complete).run(council_input)
    assert exc.value.failure_code == FAILURE_CATALOG_VALIDATION


def test_unreviewed_param_value_fails_whole_council() -> None:
    """resolver 只允许受审阅的 4 个地址；换成 1.1.1.1 必须整体失败。"""
    council_input = make_input()
    coordinator = {
        "proposals": [
            _proposal_payload(
                "PROBE_PUBLIC_RESOLVER",
                action_params={"resolver": "1.1.1.1"},
            )
        ]
    }

    def fake_complete(role: CouncilRole, payload: dict[str, Any]) -> dict[str, Any]:
        return coordinator if role is CouncilRole.COORDINATOR else _opinion_payload()

    with pytest.raises(CouncilFailure) as exc:
        NetworkCouncilRunner(complete=fake_complete).run(council_input)
    assert exc.value.failure_code == FAILURE_CATALOG_VALIDATION


def test_unknown_config_key_fails_whole_council() -> None:
    council_input = make_input()
    coordinator = {
        "proposals": [
            {
                "kind": "CONFIG_CHANGE",
                "config_key": "rtt.nonexistent",
                "config_value": "5s",
                "rationale": "调整采样",
                "proposed_preconditions": [],
                "source_bindings": [
                    {"kind": "CANONICAL_DIAGNOSIS", "ref_id": "sha256:diag-1"}
                ],
            }
        ]
    }

    def fake_complete(role: CouncilRole, payload: dict[str, Any]) -> dict[str, Any]:
        return coordinator if role is CouncilRole.COORDINATOR else _opinion_payload()

    with pytest.raises(CouncilFailure) as exc:
        NetworkCouncilRunner(complete=fake_complete).run(council_input)
    assert exc.value.failure_code == FAILURE_CATALOG_VALIDATION


def test_fail_closed_does_not_return_partial_proposals() -> None:
    """合法 A + 非法 X + 合法 B → 不得返回 A/B（不静默过滤）。"""
    council_input = make_input()

    def fake_complete(role: CouncilRole, payload: dict[str, Any]) -> dict[str, Any]:
        if role is CouncilRole.COORDINATOR:
            return {
                "proposals": [
                    _proposal_payload("CHECK_RESOLVER_CONFIG"),
                    _proposal_payload("NOT_A_REAL_ACTION"),
                    _proposal_payload("INSPECT_DEFAULT_GATEWAY"),
                ]
            }
        return _opinion_payload()

    with pytest.raises(CouncilFailure) as exc:
        NetworkCouncilRunner(complete=fake_complete).run(council_input)
    assert exc.value.failure_code == FAILURE_CATALOG_VALIDATION


# ---------------------------------------------------------------------------
# 3. SourceBinding 必须命中当前输入
# ---------------------------------------------------------------------------


def test_foreign_binding_ref_is_rejected() -> None:
    """引用别的 incident 的 ID（伪造/跨事故）必须失败。"""
    council_input = make_input()
    coordinator = {
        "proposals": [
            _proposal_payload(
                source_bindings=[{"kind": "CANONICAL_DIAGNOSIS", "ref_id": "sha256:OTHER-INCIDENT"}]
            )
        ]
    }

    def fake_complete(role: CouncilRole, payload: dict[str, Any]) -> dict[str, Any]:
        return coordinator if role is CouncilRole.COORDINATOR else _opinion_payload()

    with pytest.raises(CouncilFailure) as exc:
        NetworkCouncilRunner(complete=fake_complete).run(council_input)
    assert exc.value.failure_code == FAILURE_SOURCE_BINDING


def test_knowledge_case_cannot_be_used_as_evidence_binding() -> None:
    """RAG 案例不能冒充证据：KNOWLEDGE_CASE 不是合法 binding kind（parse 即失败）。"""
    council_input = make_input(
        knowledge_context=KnowledgeContext(
            cases=[], retrieval_strategy_version="rag-v1"
        )
    )
    coordinator = {
        "proposals": [
            _proposal_payload(
                source_bindings=[{"kind": "KNOWLEDGE_CASE", "ref_id": "case-A"}]
            )
        ]
    }

    def fake_complete(role: CouncilRole, payload: dict[str, Any]) -> dict[str, Any]:
        return coordinator if role is CouncilRole.COORDINATOR else _opinion_payload()

    with pytest.raises(CouncilFailure) as exc:
        NetworkCouncilRunner(complete=fake_complete).run(council_input)
    assert exc.value.failure_code == FAILURE_SCHEMA_PARSE


def test_l1_fact_binding_from_evidence_context_is_accepted() -> None:
    council_input = make_input()
    coordinator = {
        "proposals": [
            _proposal_payload(
                source_bindings=[{"kind": "L1_FACT", "ref_id": "e1"}]
            )
        ]
    }

    def fake_complete(role: CouncilRole, payload: dict[str, Any]) -> dict[str, Any]:
        return coordinator if role is CouncilRole.COORDINATOR else _opinion_payload()

    result = NetworkCouncilRunner(complete=fake_complete).run(council_input)
    assert result.status is CouncilStatus.REVIEW_PENDING
    assert result.proposals[0].source_bindings[0].ref_id == "e1"


# ---------------------------------------------------------------------------
# 4. 越权字段在 parse 阶段失败
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "forbidden", ["risk", "approval_required", "hypothesis", "observed_pattern"]
)
def test_overreach_fields_rejected_at_parse(forbidden: str) -> None:
    council_input = make_input()
    coordinator = {"proposals": [_proposal_payload(**{forbidden: "HIGH"})]}

    def fake_complete(role: CouncilRole, payload: dict[str, Any]) -> dict[str, Any]:
        return coordinator if role is CouncilRole.COORDINATOR else _opinion_payload()

    with pytest.raises(CouncilFailure) as exc:
        NetworkCouncilRunner(complete=fake_complete).run(council_input)
    assert exc.value.failure_code == FAILURE_SCHEMA_PARSE


# ---------------------------------------------------------------------------
# 5. 空预测列表仍可会商
# ---------------------------------------------------------------------------


def test_empty_prediction_results_do_not_block_council() -> None:
    council_input = make_input(prediction_results=[])
    calls: list[CouncilRole] = []

    def fake_complete(role: CouncilRole, payload: dict[str, Any]) -> dict[str, Any]:
        calls.append(role)
        if role is CouncilRole.COORDINATOR:
            return {"proposals": [_proposal_payload()]}
        return _opinion_payload()

    result = NetworkCouncilRunner(complete=fake_complete).run(council_input)
    assert result.status is CouncilStatus.REVIEW_PENDING
    # 三位专家 + 协调官都跑过
    assert set(calls) == {
        CouncilRole.RF_SPECTRUM,
        CouncilRole.KERNEL_STACK,
        CouncilRole.OPS_SAFETY,
        CouncilRole.COORDINATOR,
    }


def test_payload_carries_predictions_without_site_aggregate() -> None:
    council_input = make_input(prediction_results=[])
    payload = build_coordinator_payload(council_input, [])
    assert payload["prediction_results"] == []
    assert "site_risk_window" not in json.dumps(payload)
    assert "aggregate_risk" not in json.dumps(payload)


# ---------------------------------------------------------------------------
# 6. 模型失败 → FAILED
# ---------------------------------------------------------------------------


def test_model_failure_yields_failed_status() -> None:
    council_input = make_input()

    def fake_complete(role: CouncilRole, payload: dict[str, Any]) -> dict[str, Any]:
        raise RuntimeError("upstream gateway unavailable")

    with pytest.raises(CouncilFailure) as exc:
        NetworkCouncilRunner(complete=fake_complete).run(council_input)
    assert exc.value.failure_code == FAILURE_MODEL


def test_empty_proposals_is_not_a_success() -> None:
    """协调官给出空建议 → 不是"成功但无事"，而是显式失败（避免伪造"已会商"）。"""
    council_input = make_input()

    def fake_complete(role: CouncilRole, payload: dict[str, Any]) -> dict[str, Any]:
        return {"proposals": []} if role is CouncilRole.COORDINATOR else _opinion_payload()

    with pytest.raises(CouncilFailure) as exc:
        NetworkCouncilRunner(complete=fake_complete).run(council_input)
    assert exc.value.failure_code == FAILURE_CATALOG_VALIDATION


# ---------------------------------------------------------------------------
# 辅助：专家意见的标准 payload
# ---------------------------------------------------------------------------


def _opinion_payload() -> dict[str, Any]:
    return {
        "observations": ["事实 A"],
        "referenced_fact_ids": ["e1"],
        "recommendation_direction": "建议方向",
    }


# ---------------------------------------------------------------------------
# validate_proposals 直接单测（纯函数，无模型）
# ---------------------------------------------------------------------------


def test_validate_proposals_reports_all_failures_not_first_only() -> None:
    council_input = make_input()
    from industrial_ops_agent.network_assurance.council_contracts import ActionProposal

    proposals = [
        ActionProposal.model_validate(_proposal_payload("CHECK_RESOLVER_CONFIG")),
        ActionProposal.model_validate(_proposal_payload("NOT_REAL")),
    ]
    failures = validate_proposals(proposals, council_input)
    assert any("unknown action_id" in f for f in failures)


def test_specialist_payload_excludes_coordinator_only_fields() -> None:
    council_input = make_input()
    payload = build_specialist_payload(council_input, CouncilRole.RF_SPECTRUM)
    assert payload["agent_role"] == "RF_SPECTRUM"
    # 专家不得被要求给出 proposals（那是协调官的职责）
    assert "required_proposals" not in payload

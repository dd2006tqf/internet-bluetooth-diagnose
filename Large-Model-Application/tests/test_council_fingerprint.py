"""N5 tests: council_input_fingerprint 与幂等语义（路线图 ④）。

指纹必须覆盖**一切会改变模型实际输入语义的东西**——否则系统会拿旧会商结果
复用给新输入。特别地：

- ``knowledge_context`` 会实际影响专家建议，**必须**进指纹（但它仍然不是证据）；
- ``action_catalog_version`` / prompt bundle hash / contract version 任一变化，
  都会改变 Council 实际看到的内容，因此都必须改变指纹；
- ``prediction_results`` 是有序业务语义：**排序后**参与指纹（同一集合不同顺序
  视为同一输入），但集合内容变化必须改变指纹。

幂等语义：相同指纹复用既有会商；``force=true`` 另开 attempt。
"""

from __future__ import annotations

from typing import Any

import pytest

from industrial_ops_agent.network_assurance.council_contracts import (
    COUNCIL_CONTRACT_VERSION,
    CouncilInput,
    DecisionEvidenceContext,
    KnowledgeCaseRef,
    KnowledgeContext,
    OperationalConstraints,
)
from industrial_ops_agent.network_assurance.council_fingerprint import (
    council_input_fingerprint,
    knowledge_context_fingerprint,
)
from industrial_ops_agent.network_assurance.edge_action_catalog import catalog_version

CAT = catalog_version()
PROMPT_HASH = "sha256:prompt-bundle-1"


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
            action_catalog_version=CAT,
            gateway_catalog_version=CAT,
        ),
        "knowledge_context": KnowledgeContext(cases=[]),
    }
    base.update(overrides)
    return CouncilInput(**base)  # type: ignore[arg-type]


def fp(council_input: CouncilInput, **kw: Any) -> str:
    return council_input_fingerprint(
        council_input,
        prompt_bundle_hash=kw.pop("prompt_bundle_hash", PROMPT_HASH),
        contract_version=kw.pop("contract_version", COUNCIL_CONTRACT_VERSION),
        **kw,
    )


# ---------------------------------------------------------------------------
# 稳定性
# ---------------------------------------------------------------------------


def test_fingerprint_is_deterministic() -> None:
    a = fp(make_input())
    b = fp(make_input())
    assert a == b
    assert len(a) == 64  # sha256 hexdigest


def test_prediction_order_does_not_change_fingerprint() -> None:
    """同一集合不同顺序 → 同一输入（排序后参与指纹）。"""
    payload_a = {
        "subject_type": "DEVICE",
        "subject_id": "AA:01",
        "risk_level": "LOW",
        "window_estimation_status": "UNAVAILABLE",
        "forecast_horizon_hours": 168,
        "prediction_confidence": "LOW",
        "data_sufficiency": "PARTIAL",
        "failure_criterion_id": "c",
        "failure_criterion_version": "v1",
        "model_version": "m",
        "generated_at": "2026-10-04T00:00:00Z",
    }
    payload_b = dict(payload_a, subject_id="AA:02")
    forward = make_input(prediction_results=[payload_a, payload_b])  # type: ignore[list-item]
    reverse = make_input(prediction_results=[payload_b, payload_a])  # type: ignore[list-item]
    assert fp(forward) == fp(reverse)


# ---------------------------------------------------------------------------
# 敏感变化（必须改变指纹）
# ---------------------------------------------------------------------------


def test_incident_change_changes_fingerprint() -> None:
    assert fp(make_input()) != fp(make_input(incident_id="sitinc_other_1"))


def test_diagnosis_change_changes_fingerprint() -> None:
    assert fp(make_input()) != fp(
        make_input(canonical_diagnosis_digest="sha256:diag-2")
    )


def test_evidence_context_change_changes_fingerprint() -> None:
    other = DecisionEvidenceContext(
        diagnosis_evidence_ids=["diag-evidence-1"],
        prediction_evidence_ids=[],
        rf_facts=["rf-1"],
        kernel_facts=["k-1"],
        device_facts=["e1"],  # e2 缺席
    )
    assert fp(make_input()) != fp(make_input(evidence_context=other))


def test_operational_constraints_change_changes_fingerprint() -> None:
    tighter = OperationalConstraints(
        affected_assets=["radxa-cubie-a7a"],
        production_critical=True,  # 变了
        action_catalog_version=CAT,
        gateway_catalog_version=CAT,
    )
    assert fp(make_input()) != fp(make_input(operational_constraints=tighter))


def test_catalog_version_change_changes_fingerprint() -> None:
    drifted = OperationalConstraints(
        affected_assets=["radxa-cubie-a7a"],
        production_critical=False,
        action_catalog_version=CAT,
        gateway_catalog_version="cat-drifted",  # 网关跑旧固件
    )
    assert fp(make_input()) != fp(make_input(operational_constraints=drifted))


def test_prompt_bundle_hash_change_changes_fingerprint() -> None:
    assert fp(make_input()) != fp(make_input(), prompt_bundle_hash="sha256:prompt-bundle-2")


def test_contract_version_change_changes_fingerprint() -> None:
    assert fp(make_input()) != fp(make_input(), contract_version="network-council-v2")


# ---------------------------------------------------------------------------
# knowledge_context：进指纹但不进证据
# ---------------------------------------------------------------------------


def test_knowledge_context_change_changes_fingerprint() -> None:
    """知识库更新后必须触发新会商——否则新知识永不参与。"""
    no_cases = make_input()
    with_cases = make_input(
        knowledge_context=KnowledgeContext(
            cases=[KnowledgeCaseRef(case_id="case-A", case_version="3")]
        )
    )
    assert fp(no_cases) != fp(with_cases)


def test_knowledge_case_version_change_changes_fingerprint() -> None:
    v3 = make_input(
        knowledge_context=KnowledgeContext(
            cases=[KnowledgeCaseRef(case_id="case-A", case_version="3")]
        )
    )
    v4 = make_input(
        knowledge_context=KnowledgeContext(
            cases=[KnowledgeCaseRef(case_id="case-A", case_version="4")]
        )
    )
    assert fp(v3) != fp(v4)


def test_knowledge_case_order_does_not_change_fingerprint() -> None:
    """canonical order 选择 A 案：Prompt 也按 case_id 排序，故指纹同样排序。"""
    case_a = KnowledgeCaseRef(case_id="case-A", case_version="1")
    case_b = KnowledgeCaseRef(case_id="case-B", case_version="1")
    forward = make_input(knowledge_context=KnowledgeContext(cases=[case_a, case_b]))
    reverse = make_input(knowledge_context=KnowledgeContext(cases=[case_b, case_a]))
    assert fp(forward) == fp(reverse)


def test_retrieval_strategy_version_changes_fingerprint() -> None:
    v1 = make_input(
        knowledge_context=KnowledgeContext(
            cases=[KnowledgeCaseRef(case_id="case-A", case_version="1")],
            retrieval_strategy_version="rag-v1",
        )
    )
    v2 = make_input(
        knowledge_context=KnowledgeContext(
            cases=[KnowledgeCaseRef(case_id="case-A", case_version="1")],
            retrieval_strategy_version="rag-v2",
        )
    )
    assert fp(v1) != fp(v2)


def test_knowledge_fingerprint_helper_is_stable_and_version_sensitive() -> None:
    one = KnowledgeContext(cases=[KnowledgeCaseRef(case_id="a", case_version="1")])
    two = KnowledgeContext(cases=[KnowledgeCaseRef(case_id="a", case_version="2")])
    assert knowledge_context_fingerprint(one) == knowledge_context_fingerprint(one)
    assert knowledge_context_fingerprint(one) != knowledge_context_fingerprint(two)


def test_knowledge_fingerprint_does_not_leak_case_text() -> None:
    """指纹只 hash 稳定标识，不含自然语言正文（否则同一批案例措辞变化会改指纹）。"""
    digest = knowledge_context_fingerprint(
        KnowledgeContext(cases=[KnowledgeCaseRef(case_id="case-A", case_version="1")])
    )
    assert "case-A" not in digest


# ---------------------------------------------------------------------------
# 幂等策略（纯函数层）
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "changed",
    [
        {"incident_id": "sitinc_demo_2"},
        {"canonical_diagnosis_digest": "sha256:diag-9"},
    ],
)
def test_any_semantic_change_produces_new_fingerprint(changed: dict[str, Any]) -> None:
    assert fp(make_input()) != fp(make_input(**changed))

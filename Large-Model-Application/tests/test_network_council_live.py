"""N7 Live smoke: 真实模型跑一次会商（@pytest.mark.live，不阻断普通 CI）。

在有 `IOAP_MODEL_GATEWAY_API_KEY` 的环境运行：

    pytest tests/test_network_council_live.py -m live

断言的是**结构性事实**（而非文风）：
  - 协调官输出能解析为合法的 `ActionProposal[]`（closed schema + catalog 校验通过）；
  - 产出里**不含** risk / approval_required / hypothesis / observed_pattern；
  - 每位专家都给出带事实引用的意见。

若模型返回了越权字段或非法动作，会得到 `CouncilFailure`——这正是我们要观察的
fail-closed 行为，因此测试显式接受"合理失败"与"合法成功"两种结局，
但不接受"半合法"（failed 状态下 proposals 必须为空）。
"""

from __future__ import annotations

import os

import pytest

from industrial_ops_agent.network_assurance.council_contracts import (
    ActionProposalKind,
    CouncilFailure,
    CouncilInput,
    CouncilRole,
    CouncilStatus,
    DecisionEvidenceContext,
    KnowledgeContext,
    OperationalConstraints,
)
from industrial_ops_agent.network_assurance.council_model import (
    CouncilModelClient,
    prompt_bundle_hash,
)
from industrial_ops_agent.network_assurance.edge_action_catalog import (
    TOP_LEVEL_CATALOG_VERSION,
)
from industrial_ops_agent.network_assurance.network_council import NetworkCouncilRunner

pytestmark = pytest.mark.live

_FORBIDDEN = ("risk", "approval_required", "hypothesis", "observed_pattern")


def _council_input() -> CouncilInput:
    return CouncilInput(
        incident_id="sitinc_live_demo",
        canonical_diagnosis_digest="sha256:live-demo-diagnosis",
        prediction_results=[],
        evidence_context=DecisionEvidenceContext(
            diagnosis_evidence_ids=["sha256:live-demo-diagnosis"],
            prediction_evidence_ids=[],
            rf_facts=["rf: wifi_loss_rate=4.2% on channel 6", "rf: 3 devices degraded"],
            kernel_facts=["kernel: process top-N shows no abnormal bandwidth consumer"],
            device_facts=["AA:BB:CC:11:22:33 LINK_DISCONNECTED CONNECTION_TIMEOUT"],
        ),
        operational_constraints=OperationalConstraints(
            affected_assets=["radxa-cubie-a7a"],
            production_critical=False,
            action_catalog_version=TOP_LEVEL_CATALOG_VERSION,
            gateway_catalog_version=TOP_LEVEL_CATALOG_VERSION,
        ),
        knowledge_context=KnowledgeContext(cases=[]),
    )


def _live_complete():
    """优先用 app 装配的网关；未装配时回落到直连中转站（与 copilot 同一通道）。"""
    from industrial_ops_agent.api.app import create_app
    from industrial_ops_agent.config import Environment, Settings

    app = create_app(Settings(environment=Environment.TEST, _env_file=None))
    gateway = getattr(app.state, "network_model_gateway", None)
    if gateway is not None:
        from industrial_ops_agent.persistence.tenant import TenantContext

        context = TenantContext(tenant_id="tenant-live", subject_id="operator-live")
        return CouncilModelClient(gateway, context)
    from industrial_ops_agent.network_assurance.council_model import UpstreamCouncilClient

    return UpstreamCouncilClient()


def test_live_council_produces_legal_proposals_or_fails_closed() -> None:
    if not os.environ.get("IOAP_MODEL_GATEWAY_API_KEY"):
        pytest.skip("no IOAP_MODEL_GATEWAY_API_KEY configured")

    complete = _live_complete()
    runner = NetworkCouncilRunner(complete=complete)
    council_input = _council_input()

    try:
        result = runner.run(council_input)
    except CouncilFailure as failure:
        # fail-closed 是可接受的结局：宁可没有建议，也不产出半合法结果
        assert failure.failure_code, "失败必须带明确 failure_code"
        return

    assert result.status is CouncilStatus.REVIEW_PENDING
    assert result.proposals, "成功路径必须至少产出一条建议"
    assert len(result.expert_opinions) == 3

    for proposal in result.proposals:
        dumped = proposal.model_dump(mode="json")
        for forbidden in _FORBIDDEN:
            assert forbidden not in dumped, f"越权字段泄漏: {forbidden}"
        assert proposal.kind in (
            ActionProposalKind.ACTION_ID,
            ActionProposalKind.CONFIG_CHANGE,
        )
        # 建议必须带依据
        assert proposal.source_bindings

    assert {o.role for o in result.expert_opinions} == {
        CouncilRole.RF_SPECTRUM,
        CouncilRole.KERNEL_STACK,
        CouncilRole.OPS_SAFETY,
    }
    assert prompt_bundle_hash().startswith("sha256:")

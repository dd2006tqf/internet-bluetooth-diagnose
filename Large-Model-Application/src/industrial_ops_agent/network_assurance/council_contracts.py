"""Network Operations Council contracts (路线图 ④, 契约 D → E1).

本模块只定义**建议**（E1 `ActionProposal`），不定义准入与批准：

- Council 有建议权：说"做什么、为什么值得做"；
- Council **没有**准入权与批准权：`risk` / `allowed` / `approval_required` /
  `approved_by` 一律**不是**本模块的字段（属于 ⑤ 的 Policy 与 Approval）。

`ActionProposal` 与 `ExpertOpinion` 都是 closed model（`extra="forbid"`）：
LLM 若试图输出 `hypothesis` / `observed_pattern` / `risk` / `approval_required`，
会在 **schema parse 阶段**直接失败——越权由类型系统首先拒绝，护栏只做语义层兜底。
"""

from __future__ import annotations

from datetime import datetime
from enum import StrEnum
from typing import Any

from pydantic import BaseModel, ConfigDict, Field, model_validator

from industrial_ops_agent.network_assurance.wireless_contracts import PredictionResult

#: 契约版本：进入 council_input_fingerprint（契约 D/E1 演化可追溯）
COUNCIL_CONTRACT_VERSION = "network-council-v1"


class _ClosedModel(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)


# ---------------------------------------------------------------------------
# 角色
# ---------------------------------------------------------------------------


class CouncilRole(StrEnum):
    """四个角色：三位专业专家 + 一位收敛协调官。

    与旧的机械维保委员会（SAFETY/PARTS/DISPATCH）并列而非替换——旧模块无测试
    覆盖，本轮不动它。
    """

    RF_SPECTRUM = "RF_SPECTRUM"      # 空口与频谱：Wi-Fi 丢包、频段竞争、RSSI 轨迹
    KERNEL_STACK = "KERNEL_STACK"    # 内核协议栈：进程画像、skb_drop、网关自身负载
    OPS_SAFETY = "OPS_SAFETY"        # 现场运维安全：业务影响、动作可行性与风险前提
    COORDINATOR = "COORDINATOR"      # 收敛：综合三方意见产出 ActionProposal[]


#: 参与并行推理的三位专家（COORDINATOR 在其后单独运行）
SPECIALIST_ROLES: tuple[CouncilRole, ...] = (
    CouncilRole.RF_SPECTRUM,
    CouncilRole.KERNEL_STACK,
    CouncilRole.OPS_SAFETY,
)


# ---------------------------------------------------------------------------
# 来源绑定（typed ref）
# ---------------------------------------------------------------------------


class SourceBindingKind(StrEnum):
    """证据来源类别。刻意**不含** knowledge/RAG——知识不是证据。"""

    L1_FACT = "L1_FACT"                    # L1 事实（event_id / snapshot_id / history_id）
    CANONICAL_DIAGNOSIS = "CANONICAL_DIAGNOSIS"  # L2 确定性诊断产物
    DEVICE_PREDICTION = "DEVICE_PREDICTION"      # L3 预测产物


class SourceBinding(_ClosedModel):
    """一条建议所依据的既存对象引用。

    校验在服务层完成：``binding ∈ allowed_bindings(current CouncilInput)``——
    不是"数据库里存在这个 ID 就算合法"。这样同时堵死模型伪造 ID、跨 incident
    引证、跨 tenant 引证，以及 RAG 案例冒充证据。
    """

    kind: SourceBindingKind
    ref_id: str = Field(min_length=1, max_length=255)


# ---------------------------------------------------------------------------
# E1：Council 的建议（无 risk / 无 approval / 无新诊断）
# ---------------------------------------------------------------------------


class ActionProposalKind(StrEnum):
    ACTION_ID = "ACTION_ID"          # 板端 ActionRegistry 白名单动作
    CONFIG_CHANGE = "CONFIG_CHANGE"  # 板端可调参数键


class ActionProposal(_ClosedModel):
    """Council 的唯一输出形态：**建议**。

    字段集刻意最小且 closed：

    - ``kind`` + 二选一的目标（``action_id`` 或 ``config_key/config_value``）；
    - ``rationale`` —— 产品语义固定为"**为什么这个动作值得执行**"，
      不是"再进行一次故障诊断"（引用既有诊断合法，重新下诊断结论非法）；
    - ``proposed_preconditions`` —— 专家认为的执行前提（**仅建议**，
      真正的前置条件由 ⑤ 的 Policy 计算）；
    - ``source_bindings`` —— 必须指向当前 CouncilInput 内的既存对象。

    绝不包含：``risk`` / ``allowed`` / ``approval_required`` / ``approved_by`` /
    ``hypothesis`` / ``observed_pattern``。
    """

    kind: ActionProposalKind
    action_id: str | None = Field(default=None, max_length=128)
    #: 动作参数（仅 ACTION_ID 形态）。取值必须命中受审阅白名单——由 E1
    #: ProposalCatalogValidator 校验；板端执行前还会再校验一次（服务端从不
    #: 假设自己的检查是终局）。
    action_params: dict[str, str] = Field(default_factory=dict)
    config_key: str | None = Field(default=None, max_length=128)
    config_value: str | None = Field(default=None, max_length=512)
    rationale: str = Field(min_length=1, max_length=2000)
    proposed_preconditions: list[str] = Field(default_factory=list, max_length=16)
    source_bindings: list[SourceBinding] = Field(min_length=1, max_length=16)

    @model_validator(mode="after")
    def _exactly_one_target(self) -> ActionProposal:
        if self.kind is ActionProposalKind.ACTION_ID:
            if not self.action_id:
                raise ValueError("ACTION_ID proposal requires action_id")
            if self.config_key is not None or self.config_value is not None:
                raise ValueError("ACTION_ID proposal must not carry config_key/config_value")
        else:
            if not self.config_key or self.config_value is None:
                raise ValueError("CONFIG_CHANGE proposal requires config_key and config_value")
            if self.action_id is not None:
                raise ValueError("CONFIG_CHANGE proposal must not carry action_id")
            if self.action_params:
                raise ValueError("CONFIG_CHANGE proposal must not carry action_params")
        return self


class ExpertOpinion(_ClosedModel):
    """一位专家的一轮意见。

    专家**可以**引用、比较、权衡 L1 事实，但**不能**产出新的
    ``ObservedPattern`` / ``Hypothesis``——那些属于 L2 诊断层，本轮不重构。

    ``referenced_fact_ids`` 必填：没有事实依据的意见不接受。
    """

    role: CouncilRole
    observations: list[str] = Field(min_length=1, max_length=20)
    referenced_fact_ids: list[str] = Field(min_length=1, max_length=64)
    recommendation_direction: str = Field(min_length=1, max_length=1000)


# ---------------------------------------------------------------------------
# CouncilInput（契约 D 的只读输入）
# ---------------------------------------------------------------------------


class DecisionEvidenceContext(_ClosedModel):
    """Council 可引用的事实上下文——全部来自 L1 已落库事实或其幂等摘要。

    专家靠这里的材料做专业权衡，而不是重新做一遍诊断。
    """

    diagnosis_evidence_ids: list[str] = Field(default_factory=list)
    prediction_evidence_ids: list[str] = Field(default_factory=list)
    rf_facts: list[str] = Field(default_factory=list)
    kernel_facts: list[str] = Field(default_factory=list)
    device_facts: list[str] = Field(default_factory=list)


class OperationalConstraints(_ClosedModel):
    """运维与部署约束。

    ``action_catalog_version`` / ``gateway_catalog_version`` 用于暴露**部署版本
    漂移**：源码级白名单镜像只能保证"仓库内 Python == 仓库内 C++"，不能保证
    "云端版本 == 目标网关版本"。v1 部署前提是两者一致，不一致时由 ⑤ 的 Policy
    阻断（本轮只承载数据与约束声明）。
    """

    affected_assets: list[str] = Field(default_factory=list)
    production_critical: bool = False
    action_catalog_version: str = Field(min_length=1, max_length=64)
    gateway_catalog_version: str = Field(min_length=1, max_length=64)


class KnowledgeCaseRef(_ClosedModel):
    """RAG 案例的稳定标识（只 hash 这个，不 hash 自然语言 prompt）。"""

    case_id: str = Field(min_length=1, max_length=128)
    case_version: str = Field(min_length=1, max_length=64)


class KnowledgeContext(_ClosedModel):
    """历史案例参考——**不是证据**。

    - 不进 ``DecisionEvidenceContext``；
    - 不进任何 ``SourceBinding``；
    - 但**必须**进 ``council_input_fingerprint``：它会实际影响专家建议，
      若不入缓存身份，知识库更新后系统会复用旧会商结果、新知识永不参与。
    """

    cases: list[KnowledgeCaseRef] = Field(default_factory=list, max_length=32)
    retrieval_strategy_version: str = Field(default="rag-v1", max_length=64)


class CouncilInput(_ClosedModel):
    """契约 D 的只读输入包。

    ``prediction_results`` 是**受影响设备的 DEVICE 预测列表，且可以为空**：
    预测是增强输入而非召集前置。刻意**没有** site 级聚合字段——绝不把多台设备
    风险平均成一个 ``Site RiskWindow``。
    """

    incident_id: str = Field(min_length=1, max_length=128)
    canonical_diagnosis_digest: str = Field(min_length=1, max_length=128)
    prediction_results: list[PredictionResult] = Field(default_factory=list, max_length=64)
    evidence_context: DecisionEvidenceContext
    operational_constraints: OperationalConstraints
    knowledge_context: KnowledgeContext

    def allowed_binding_ref_ids(self) -> dict[SourceBindingKind, set[str]]:
        """当前输入内允许被引用的对象集合（W8 的唯一依据）。"""
        return {
            SourceBindingKind.L1_FACT: set(self.evidence_context.device_facts)
            | set(self.evidence_context.rf_facts)
            | set(self.evidence_context.kernel_facts),
            SourceBindingKind.CANONICAL_DIAGNOSIS: set(
                self.evidence_context.diagnosis_evidence_ids
            )
            | {self.canonical_diagnosis_digest},
            SourceBindingKind.DEVICE_PREDICTION: {
                f"{p.subject_type}:{p.subject_id}"
                for p in self.prediction_results
                if hasattr(p, "subject_id")
            }
            | set(self.evidence_context.prediction_evidence_ids),
        }


# ---------------------------------------------------------------------------
# Council 生命周期视图（无执行/批准语义）
# ---------------------------------------------------------------------------


class CouncilStatus(StrEnum):
    """终态只有 REVIEW_PENDING（产出建议待人工处置）与 FAILED（不产出半合法结果）。"""

    QUEUED = "QUEUED"
    RUNNING = "RUNNING"
    REVIEW_PENDING = "REVIEW_PENDING"
    FAILED = "FAILED"


class CouncilView(_ClosedModel):
    """会商结果视图。

    刻意不含 ``approved_by`` / ``approved_at`` / ``executed`` / ``queued_actions``：
    这些属于 ⑤ 的批准与执行链路，Council 视图在结构上就不表达它们。
    """

    council_id: str
    incident_id: str
    input_fingerprint: str
    status: CouncilStatus
    stage: str
    proposals: list[ActionProposal] = Field(default_factory=list)
    expert_opinions: list[ExpertOpinion] = Field(default_factory=list)
    failure_code: str | None = None
    requested_by_subject_id: str
    created_at: datetime
    updated_at: datetime
    version: int = Field(ge=1)


class CouncilFailure(RuntimeError):
    """会商失败（fail closed：不产出半合法 Council）。"""

    def __init__(self, failure_code: str, detail: str = "") -> None:
        self.failure_code = failure_code
        self.detail = detail
        super().__init__(f"{failure_code}: {detail}" if detail else failure_code)


#: Council 失败码（写进既有失败码风格，供 ⑤ 与审计使用）
FAILURE_SCHEMA_PARSE = "COUNCIL_SCHEMA_PARSE_FAILED"
FAILURE_CATALOG_VALIDATION = "PROPOSAL_CATALOG_VALIDATION_FAILED"
FAILURE_SOURCE_BINDING = "PROPOSAL_SOURCE_BINDING_FAILED"
FAILURE_KNOWLEDGE_ISOLATION = "KNOWLEDGE_ISOLATION_VIOLATION"
FAILURE_MODEL = "COUNCIL_MODEL_FAILED"


def binding_is_allowed(binding: SourceBinding, allowed: dict[SourceBindingKind, set[str]]) -> bool:
    """W8 的唯一判据：绑定必须命中当前 CouncilInput 内的既存对象。"""
    return binding.ref_id in allowed.get(binding.kind, set())


def coerce_proposal_payload(payload: Any) -> ActionProposal:
    """把模型返回的原始 dict 解析为 ActionProposal（越权字段在此失败）。"""
    return ActionProposal.model_validate(payload)

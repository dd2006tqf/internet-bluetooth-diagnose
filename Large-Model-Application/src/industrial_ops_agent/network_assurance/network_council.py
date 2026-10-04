"""Network Operations Council runner (路线图 ④, 止于 E1 ActionProposal[])。

管线（严格顺序，任一环节失败即整体 fail closed）：

    CouncilInput
      → 3 Specialists 并行（RF_SPECTRUM / KERNEL_STACK / OPS_SAFETY）
      → Coordinator（收敛）
      → closed-schema parse          ← 越权字段在此失败
      → E1 ProposalCatalogValidator  ← 合法性：动作/键/参数必须在受审阅白名单内
      → SourceBinding 校验           ← 必须命中当前 CouncilInput 内既存对象
      → CouncilResult(ActionProposal[])

三条纪律（代码即契约）：

1. **fail closed**：任一 proposal 非法 → 整次 coordinator output 不接受。
   绝不"删掉非法项、保存其余"——静默过滤会改变协调官的原始决策语义，
   也让"半合法 Council"看起来像完整会商。
2. **越权由类型系统首先拒绝**：`ActionProposal` 是 closed model，
   `risk` / `approval_required` / `hypothesis` / `observed_pattern` 会在
   parse 阶段失败。护栏只做语义层二次兜底（例如 rationale 重下根因结论）。
3. **本模块不触达 L5**：不 import `queue_action`、不写 `pending_actions`、
   不产生 ApprovalRequest —— 那些全部属于 ⑤。

本模块是纯逻辑 + 一个注入的模型调用函数（`complete`），因此完全可在无网络、
无数据库的单元测试中运行。
"""

from __future__ import annotations

import json
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass, field
from datetime import UTC, datetime
from typing import Any
from uuid import uuid4

from pydantic import ValidationError

from industrial_ops_agent.network_assurance.council_contracts import (
    FAILURE_CATALOG_VALIDATION,
    FAILURE_KNOWLEDGE_ISOLATION,
    FAILURE_MODEL,
    FAILURE_SCHEMA_PARSE,
    FAILURE_SOURCE_BINDING,
    SPECIALIST_ROLES,
    ActionProposal,
    ActionProposalKind,
    CouncilFailure,
    CouncilInput,
    CouncilRole,
    CouncilStatus,
    ExpertOpinion,
    binding_is_allowed,
)
from industrial_ops_agent.network_assurance.edge_action_catalog import (
    ACTION_CATALOG,
    CONFIG_KEYS,
    find_action,
    is_known_config_key,
    validate_action_params,
)

#: 模型调用签名：给定角色与该角色的 payload，返回解析后的 JSON 对象。
#: 生产实现走 ModelGateway / upstream；测试注入 Mock，无需网络。
CompleteFn = Callable[[CouncilRole, dict[str, Any]], dict[str, Any]]


@dataclass(frozen=True, slots=True)
class CouncilResult:
    """本轮会商的产出（不含任何批准/执行语义）。"""

    council_id: str
    incident_id: str
    status: CouncilStatus
    stage: str
    proposals: list[ActionProposal] = field(default_factory=list)
    expert_opinions: list[ExpertOpinion] = field(default_factory=list)
    model_calls: int = 0


# ---------------------------------------------------------------------------
# Payload 构造
# ---------------------------------------------------------------------------


def _input_summary(council_input: CouncilInput) -> dict[str, Any]:
    """送入模型的只读输入摘要（事实 → 模式/假说 → 预测 → 约束）。

    刻意**不含** knowledge_context：知识只作为解释背景，由提示词层面注入，
    不进 payload 的证据区（否则会诱导模型把案例当证据引用）。
    """
    return {
        "incident_id": council_input.incident_id,
        "canonical_diagnosis_digest": council_input.canonical_diagnosis_digest,
        "prediction_results": [
            (
                p.model_dump(mode="json")
                if hasattr(p, "model_dump")
                else dict(p)  # pragma: no cover - 兼容映射输入
            )
            for p in council_input.prediction_results
        ],
        "evidence_context": council_input.evidence_context.model_dump(mode="json"),
        "operational_constraints": council_input.operational_constraints.model_dump(
            mode="json"
        ),
    }


def build_specialist_payload(
    council_input: CouncilInput, role: CouncilRole
) -> dict[str, Any]:
    """专家 payload：只要求意见，不要求建议清单（那是协调官的职责）。"""
    return {
        "agent_role": str(role),
        "advisory_only": True,
        "business_side_effects_allowed": False,
        "instruction": (
            "引用已有事实做专业权衡。不要输出 hypothesis 或 observed_pattern："
            "重新下诊断结论不是本角色的职责。"
        ),
        **_input_summary(council_input),
    }


def build_coordinator_payload(
    council_input: CouncilInput, opinions: Sequence[ExpertOpinion]
) -> dict[str, Any]:
    """协调官 payload：要求产出 ActionProposal[]（**不含** risk/approval）。

    **必须携带受审阅的动作目录**：提示词要求"只提目录里的动作"，如果 payload
    里没有目录，模型只能凭空编造 action_id（实测：它发明了
    ``rf_verify_channel_configuration`` 这类不存在的动作，随后被 E1 拦截
    fail-closed）。给定目录是 E1 能真正发挥作用的前提——校验的意义是
    "从合法集合里选"，不是"让模型猜一个再打回去"。
    """
    allowed = council_input.allowed_binding_ref_ids()
    return {
        "agent_role": str(CouncilRole.COORDINATOR),
        "advisory_only": True,
        "business_side_effects_allowed": False,
        "specialist_opinions": [o.model_dump(mode="json") for o in opinions],
        "allowed_source_binding_ref_ids": {k.value: sorted(v) for k, v in allowed.items()},
        "action_catalog": [
            {
                "action_id": spec.action_id,
                "description": spec.description,
                "params": [
                    {
                        "name": p.name,
                        "type": p.type,
                        "required": p.required,
                        "allowed_values": list(p.allowed_values),
                    }
                    for p in spec.params
                ],
            }
            for spec in ACTION_CATALOG.values()
        ],
        "config_keys": sorted(CONFIG_KEYS),
        "instruction": (
            "输出 proposals：每条包含 kind、目标（action_id 或 config_key/config_value）、"
            "rationale（为什么这个动作值得执行）与 source_bindings（必须引用上面允许的 ID）。"
            "action_id 必须来自 action_catalog，config_key 必须来自 config_keys。"
            "不要输出 risk、approval_required、hypothesis 或 observed_pattern。"
        ),
        **_input_summary(council_input),
    }


# ---------------------------------------------------------------------------
# E1 Catalog Validation（只校验合法性，不算 risk / allowed / approval）
# ---------------------------------------------------------------------------


def validate_proposals(
    proposals: Sequence[ActionProposal], council_input: CouncilInput
) -> list[str]:
    """返回全部违规原因（不是只报第一条——便于一次性修正与审计）。"""
    failures: list[str] = []
    if not proposals:
        failures.append("coordinator produced no proposals")
        return failures

    allowed = council_input.allowed_binding_ref_ids()

    for index, proposal in enumerate(proposals):
        where = f"proposal[{index}]"

        # (a) 技术可执行性：动作 / 配置键 / 参数必须命中受审阅白名单
        if proposal.kind is ActionProposalKind.ACTION_ID:
            assert proposal.action_id is not None  # model_validator 已保证
            if find_action(proposal.action_id) is None:
                failures.append(f"{where}: unknown action_id {proposal.action_id!r}")
        else:
            assert proposal.config_key is not None
            if not is_known_config_key(proposal.config_key):
                failures.append(
                    f"{where}: unknown config_key {proposal.config_key!r}"
                )

        # (b) 参数级校验（仅 ACTION_ID 形态；CONFIG_CHANGE 的取值由板端范围校验）。
        # 只在动作本身已通过 (a) 时跑——否则未知 action_id 会以两种措辞报两遍，
        # 掩盖真正的第一条原因。
        if proposal.action_params and find_action(proposal.action_id or "") is not None:
            reason = validate_action_params(proposal.action_id or "", proposal.action_params)
            if reason:
                failures.append(f"{where}: {reason}")

        # (c) 来源绑定必须命中当前输入内的既存对象
        for binding in proposal.source_bindings:
            if not binding_is_allowed(binding, allowed):
                failures.append(
                    f"{where}: binding {binding.kind}={binding.ref_id!r} "
                    f"not present in this council input"
                )

        # (d) 知识隔离：RAG 案例绝不能作为证据来源（结构性兜底）
        if any(binding.kind.value not in {"L1_FACT", "CANONICAL_DIAGNOSIS", "DEVICE_PREDICTION"}
               for binding in proposal.source_bindings):
            failures.append(f"{where}: non-evidence binding kind used")

    # 部署版本漂移（cloud catalog ≠ gateway catalog）**不在此处判定**：
    # 那是"当前情境允许不允许执行"的准入问题，属于 ⑤ 的 Policy。
    # ④ 只负责把两侧版本作为数据承载下来（OperationalConstraints + 指纹），
    # 使漂移可被检测、可影响缓存身份。v1 部署前提是两者一致。

    return failures


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------


class NetworkCouncilRunner:
    """同步进程内执行（v1 不引入 Temporal；与 WirelessDiagnosisService 同构）。"""

    def __init__(self, complete: CompleteFn) -> None:
        self._complete = complete

    def run(self, council_input: CouncilInput) -> CouncilResult:
        calls = 0

        # 1) 专家并行 —— 顺序执行即等价（无共享状态），并保持可读的错误定位
        opinions: list[ExpertOpinion] = []
        for role in SPECIALIST_ROLES:
            payload = build_specialist_payload(council_input, role)
            try:
                raw = self._complete(role, payload)
            except Exception as exc:  # noqa: BLE001 - 上游失败一律 fail closed
                raise CouncilFailure(FAILURE_MODEL, f"{role}: {exc}") from exc
            calls += 1
            try:
                opinions.append(
                    ExpertOpinion(
                        role=role,
                        observations=list(raw.get("observations", [])),
                        referenced_fact_ids=list(raw.get("referenced_fact_ids", [])),
                        recommendation_direction=str(
                            raw.get("recommendation_direction", "")
                        ),
                    )
                )
            except ValidationError as exc:
                raise CouncilFailure(
                    FAILURE_SCHEMA_PARSE, f"{role} opinion invalid: {_first_error(exc)}"
                ) from exc

        # 2) 协调官收敛
        coordinator_payload = build_coordinator_payload(council_input, opinions)
        try:
            raw_coordinator = self._complete(CouncilRole.COORDINATOR, coordinator_payload)
        except Exception as exc:  # noqa: BLE001
            raise CouncilFailure(FAILURE_MODEL, f"COORDINATOR: {exc}") from exc
        calls += 1

        # 3) closed-schema parse（越权字段在此失败）
        raw_proposals = raw_coordinator.get("proposals")
        if not isinstance(raw_proposals, list):
            raise CouncilFailure(FAILURE_SCHEMA_PARSE, "coordinator output has no proposals list")
        proposals: list[ActionProposal] = []
        for item in raw_proposals:
            try:
                proposals.append(ActionProposal.model_validate(item))
            except ValidationError as exc:
                # 越权字段 / 目标二选一违规 / 空绑定 —— 全部止步于此
                raise CouncilFailure(
                    FAILURE_SCHEMA_PARSE,
                    f"proposal rejected at parse: {_first_error(exc)}",
                ) from exc

        # 4) E1 Catalog Validation + 来源绑定（fail closed：不静默过滤）
        failures = validate_proposals(proposals, council_input)
        if failures:
            code = FAILURE_CATALOG_VALIDATION
            if any("binding" in f for f in failures):
                code = FAILURE_SOURCE_BINDING
            if any("non-evidence binding kind" in f for f in failures):
                code = FAILURE_KNOWLEDGE_ISOLATION
            raise CouncilFailure(code, "; ".join(failures))

        return CouncilResult(
            council_id=f"ncouncil-{uuid4().hex[:12]}",
            incident_id=council_input.incident_id,
            status=CouncilStatus.REVIEW_PENDING,
            stage="COORDINATOR_DONE",
            proposals=proposals,
            expert_opinions=opinions,
            model_calls=calls,
        )


def _first_error(exc: ValidationError) -> str:
    errors = exc.errors()
    if not errors:
        return "unknown validation error"
    first = errors[0]
    loc = ".".join(str(part) for part in first.get("loc", ()))
    return f"{loc}: {first.get('msg', '')}".strip(": ")


# ---------------------------------------------------------------------------
# 辅助：模型响应 → JSON 文本（供生产实现的完整调用使用）
# ---------------------------------------------------------------------------


def dumps_council_payload(payload: Mapping[str, Any]) -> str:
    return json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def utc_now() -> datetime:
    return datetime.now(UTC)

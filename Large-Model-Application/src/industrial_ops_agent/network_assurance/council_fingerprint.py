"""council_input_fingerprint —— 会商输入的缓存身份（路线图 ④，契约 D）。

原则（一句）：**凡能改变模型实际输入语义的东西，都必须改变 fingerprint。**

覆盖项与理由：

| 输入 | 为什么进指纹 |
|---|---|
| `incident_id` | 不同事故绝不可共用会商 |
| `canonical_diagnosis_digest` | L2 结论变了，专家看到的前提就变了 |
| `sorted(DEVICE predictions)` | 预测是建议的重要依据；**排序后**参与（同集合不同顺序是同一输入） |
| `evidence_context` | 专家引用的 L1 事实集合 |
| `operational_constraints`（含 catalog 版本） | 约束与部署版本漂移会改变可选动作域 |
| `knowledge_context_fingerprint` | RAG 案例会实际影响建议；不入指纹则知识库更新后永远复用旧会商 |
| prompt bundle hash | 提示词变了，模型行为就变了 |
| `COUNCIL_CONTRACT_VERSION` | 契约演化必须可追溯 |

**知识进指纹 ≠ 知识是证据**：`knowledge_context` 绝不进 `evidence_context`、
绝不进任何 `SourceBinding`、也绝不进护栏认可的 Evidence ID 集合。它只因为会
影响模型输出而必须影响缓存身份。

canonical order 选择 **A 案**：Prompt 与指纹都按 `case_id` 排序，因此两种检索
排序在语义上等价（同一批案例、同一份内容 → 同一指纹）。若将来改为保留 retrieval
rank，则必须把 `[(rank, case_id, case_version)]` 纳入指纹（用户终审明确要求二者
一致）。
"""

from __future__ import annotations

import hashlib
import json
from typing import Any

from industrial_ops_agent.network_assurance.council_contracts import (
    COUNCIL_CONTRACT_VERSION,
    CouncilInput,
    KnowledgeContext,
)

_PREDICTION_KEYS = (
    "subject_type",
    "subject_id",
    "risk_level",
    "window_estimation_status",
    "window_earliest_hours",
    "window_latest_hours",
    "forecast_horizon_hours",
    "prediction_confidence",
    "data_sufficiency",
    "drivers",
    "trend_evidence",
    "failure_criterion_id",
    "failure_criterion_version",
    "model_version",
    # 刻意排除 generated_at：它随调用时刻变化，与输入语义无关，
    # 入指纹会让"同一输入"每次都被判为新输入。
)


def knowledge_context_fingerprint(knowledge: KnowledgeContext) -> str:
    """只 hash 稳定标识：``sorted(case_id, case_version)`` + 检索策略版本。

    **不得**把拼接后的自然语言 prompt 全量当指纹——案例正文的措辞微调不应
    触发重新会商，而案例集合/版本变化必须触发。
    """
    payload = {
        "cases": sorted(
            (case.case_id, case.case_version) for case in knowledge.cases
        ),
        "retrieval_strategy_version": knowledge.retrieval_strategy_version,
    }
    return _sha256(payload)


def _prediction_canonical(prediction: Any) -> dict[str, Any]:
    if hasattr(prediction, "model_dump"):
        dumped = prediction.model_dump(mode="json")
    else:  # pragma: no cover - 兼容 dict 输入
        dumped = dict(prediction)
    return {key: dumped.get(key) for key in _PREDICTION_KEYS}


def council_input_fingerprint(
    council_input: CouncilInput,
    *,
    prompt_bundle_hash: str,
    contract_version: str = COUNCIL_CONTRACT_VERSION,
) -> str:
    """返回 64 位十六进制指纹。"""
    predictions = sorted(
        _canonical_json(_prediction_canonical(p)) for p in council_input.prediction_results
    )
    payload = {
        "incident_id": council_input.incident_id,
        "canonical_diagnosis_digest": council_input.canonical_diagnosis_digest,
        "predictions": predictions,
        "evidence_context": _canonical_json(
            council_input.evidence_context.model_dump(mode="json")
        ),
        "operational_constraints": _canonical_json(
            council_input.operational_constraints.model_dump(mode="json")
        ),
        "knowledge_context": knowledge_context_fingerprint(council_input.knowledge_context),
        "prompt_bundle_hash": prompt_bundle_hash,
        "contract_version": contract_version,
    }
    return _sha256(payload)


# ---------------------------------------------------------------------------
# 内部工具
# ---------------------------------------------------------------------------


def _canonical_json(value: Any) -> str:
    return json.dumps(value, sort_keys=True, ensure_ascii=False, separators=(",", ":"))


def _sha256(value: Any) -> str:
    payload = value if isinstance(value, str) else _canonical_json(value)
    return hashlib.sha256(payload.encode()).hexdigest()

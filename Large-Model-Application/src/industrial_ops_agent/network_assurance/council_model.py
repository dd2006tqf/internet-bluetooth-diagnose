"""Council model plumbing (路线图 ④): prompt bundle → ModelGateway → parsed JSON.

把 `NetworkCouncilRunner` 需要的 `CompleteFn` 接到真实的 `ModelGateway` 上。

失败语义：任何上游异常都向上抛出，由 Runner 统一转成 `CouncilFailure(FAILURE_MODEL)`
并让 Council 记录为 FAILED——**绝不编造建议**。
"""

from __future__ import annotations

import json
import logging
from datetime import UTC, datetime, timedelta
from typing import Any, Protocol

from industrial_ops_agent.network_assurance.council_contracts import CouncilRole
from industrial_ops_agent.network_assurance.network_council import dumps_council_payload
from industrial_ops_agent.prompting.registry import (
    NETWORK_COUNCIL_PROMPT_BUNDLE_ID,
    PromptBundleNotDeployed,
    default_prompt_registry,
)

LOGGER = logging.getLogger("industrial_ops_agent.network_assurance.council_model")

#: 单个角色的推理超时（Council 是 4 次连续调用，总预算见调用方）
DEFAULT_ROLE_TIMEOUT_SECONDS = 90.0


class CouncilModelGateway(Protocol):
    """与 `ModelGateway.complete` 兼容的最小协议（避免此处 import 具体实现）。"""

    def complete(self, context: Any, request: Any) -> Any: ...


def load_council_prompt():
    """读取受审阅的 Council 提示词包（未部署时抛 PromptBundleNotDeployed）。"""
    return default_prompt_registry().get(NETWORK_COUNCIL_PROMPT_BUNDLE_ID)


def prompt_bundle_hash() -> str:
    """提示词包内容哈希——进入 council_input_fingerprint。"""
    return load_council_prompt().content_hash


class CouncilModelClient:
    """把 (role, payload) 转成一次网关调用并解析 JSON 结果。

    ``context`` 由调用方提供（含 tenant/subject 等边界信息）；
    ``GatewayRequest`` 的构造刻意集中在 `_build_request`，便于测试替换。
    """

    def __init__(
        self,
        gateway: Any,
        context: Any,
        *,
        model_alias: str = "industrial-diagnosis",
        timeout_seconds: float = DEFAULT_ROLE_TIMEOUT_SECONDS,
    ) -> None:
        self._gateway = gateway
        self._context = context
        self._model_alias = model_alias
        self._timeout = timeout_seconds

    def __call__(self, role: CouncilRole, payload: dict[str, Any]) -> dict[str, Any]:
        request = self._build_request(role, payload)
        response = self._gateway.complete(self._context, request)
        return _parse_response(response)

    def _build_request(self, role: CouncilRole, payload: dict[str, Any]) -> Any:
        from industrial_ops_agent.model_gateway.service import GatewayRequest

        bundle = load_council_prompt()
        request_id = f"ncouncil-{role.lower()}-{datetime.now(UTC).timestamp():.0f}"
        return GatewayRequest(
            inference_request_id=request_id,
            model_alias=self._model_alias,
            subject_id=getattr(self._context, "subject_id", "edge-automation"),
            trace_id=request_id,
            request_class="NETWORK_OPERATIONS_COUNCIL",
            data_classification="CONFIDENTIAL",
            messages=(
                {"role": "system", "content": bundle.render_system()},
                {"role": "user", "content": dumps_council_payload(payload)},
            ),
            response_schema_name=bundle.output_schema_name,
            response_schema=_response_schema(role),
            deadline=datetime.now(UTC) + timedelta(seconds=self._timeout),
            max_output_tokens=4096,
            temperature=0.0,
            prompt_bundle_id=bundle.prompt_bundle_id,
            prompt_bundle_hash=bundle.content_hash,
        )


def _response_schema(role: CouncilRole) -> dict[str, Any]:
    """角色的严格输出 schema。

    关键：**协调官的 schema 里没有** ``risk`` / ``approval_required`` /
    ``hypothesis`` / ``observed_pattern``。这类字段在网关层就会被结构化输出
    约束排除；即便模型硬塞，`ActionProposal`（closed model）仍会在 parse 阶段拒绝。
    """
    string_array = {
        "type": "array",
        "maxItems": 20,
        "items": {"type": "string", "minLength": 1, "maxLength": 500},
    }
    if role is not CouncilRole.COORDINATOR:
        return {
            "type": "object",
            "additionalProperties": False,
            "required": [
                "observations",
                "referenced_fact_ids",
                "recommendation_direction",
            ],
            "properties": {
                "observations": string_array,
                "referenced_fact_ids": string_array,
                "recommendation_direction": {"type": "string", "minLength": 1, "maxLength": 1000},
            },
        }
    binding = {
        "type": "object",
        "additionalProperties": False,
        "required": ["kind", "ref_id"],
        "properties": {
            "kind": {
                "type": "string",
                "enum": ["L1_FACT", "CANONICAL_DIAGNOSIS", "DEVICE_PREDICTION"],
            },
            "ref_id": {"type": "string", "minLength": 1, "maxLength": 255},
        },
    }
    proposal = {
        "type": "object",
        "additionalProperties": False,
        "required": ["kind", "rationale", "source_bindings"],
        "properties": {
            "kind": {"type": "string", "enum": ["ACTION_ID", "CONFIG_CHANGE"]},
            "action_id": {"type": ["string", "null"], "maxLength": 128},
            "action_params": {
                "type": "object",
                "additionalProperties": {"type": "string"},
            },
            "config_key": {"type": ["string", "null"], "maxLength": 128},
            "config_value": {"type": ["string", "null"], "maxLength": 512},
            "rationale": {"type": "string", "minLength": 1, "maxLength": 2000},
            "proposed_preconditions": string_array,
            "source_bindings": {"type": "array", "minItems": 1, "items": binding},
        },
    }
    return {
        "type": "object",
        "additionalProperties": False,
        "required": ["proposals"],
        "properties": {
            "proposals": {"type": "array", "maxItems": 16, "items": proposal},
        },
    }


def _parse_response(response: Any) -> dict[str, Any]:
    """从网关响应里取出 JSON 对象（兼容 message.content / reasoning 两种形态）。"""
    content = getattr(response, "content", None)
    if content is None:
        message = getattr(response, "message", None)
        if isinstance(message, dict):
            content = message.get("content") or message.get("reasoning")
        else:
            content = message
    if isinstance(content, dict):
        return content
    if not isinstance(content, str):
        raise CouncilModelResponseError("gateway response carried no textual content")
    text = content.strip()
    if text.startswith("```"):
        text = text.split("\n", 1)[-1]
        text = text.rsplit("```", 1)[0]
    try:
        parsed = json.loads(text)
    except json.JSONDecodeError as exc:
        raise CouncilModelResponseError(f"gateway response was not JSON: {exc}") from exc
    if not isinstance(parsed, dict):
        raise CouncilModelResponseError("gateway response was not a JSON object")
    return parsed


class CouncilModelResponseError(RuntimeError):
    """网关返回了无法解析的内容（由 Runner 转成 FAILED）。"""


class UpstreamCouncilClient:
    """直连中转站的 Council 模型客户端（与 copilot / wireless_diagnosis 同一通道）。

    复用 ``upstream.complete_json``：读热配置（model_config）或环境变量，
    走 OpenAI 兼容 ``/chat/completions``，``response_format=json_object``，
    temperature=0。未配置 key 时返回 None —— 这里转成异常，让 Runner 归入
    ``FAILURE_MODEL``（fail closed，不编造建议）。
    """

    def __init__(self, *, max_tokens: int = 8192) -> None:
        # 4096 仍可能不够：协调官 payload 携带动作目录 + 3 份专家意见，
        # 中文 rationale 展开在多设备场景下实测到达 2500+ 字符触发截断。
        # 8192 彻底消除 finish=length 截断风险。
        self._max_tokens = max_tokens

    def __call__(self, role: CouncilRole, payload: dict[str, Any]) -> dict[str, Any]:
        from industrial_ops_agent.network_assurance.upstream import complete_json

        bundle = load_council_prompt()
        # 直连通道没有网关那层 `response_format=json_schema` 约束，因此必须把
        # 该角色的输出 schema 显式写进提示词——否则模型会自造结构
        # （实测：它回了 {"action","target_asset",...} 这种自己的形状，
        #  导致专家意见 parse 失败、Council 整体 FAILED）。
        schema = json.dumps(_response_schema(role), ensure_ascii=False)
        system = (
            bundle.render_system()
            + "\n\nReturn ONLY a JSON object matching exactly this JSON Schema "
            + "(no markdown fences, no extra fields):\n"
            + schema
        )
        parsed = complete_json(
            system, dumps_council_payload(payload), max_tokens=self._max_tokens
        )
        if parsed is None:
            raise CouncilModelResponseError(
                "upstream gateway unavailable or returned no JSON"
            )
        return parsed


__all__ = [
    "CouncilModelClient",
    "CouncilModelGateway",
    "CouncilModelResponseError",
    "PromptBundleNotDeployed",
    "UpstreamCouncilClient",
    "load_council_prompt",
    "prompt_bundle_hash",
]

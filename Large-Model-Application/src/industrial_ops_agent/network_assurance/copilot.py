"""Network copilot: causal diagnosis over the immutable WeakNet snapshot.

## Why this is deterministic first

The copilot answers operator questions about why a device's network is
degraded. The failure mode that matters most is a confident answer that
inverts the causal order the edge evaluator already established (blaming DNS
above an unreachable gateway, calling the transport broken because a server
returned 500). A language model can produce that inversion; the snapshot
cannot — it *is* the ground truth.

So the causal chain is derived deterministically from the snapshot's own
states, mirroring ``overall_policy.hpp``'s ordering. When a governed model
gateway is configured, the model is asked to explain that chain in richer
prose, and its structured report is checked by
:class:`~industrial_ops_agent.guardrails.network_causal.NetworkCausalGuardrail`
before it is shown. A blocked or failed model report falls back to the
deterministic answer — the operator always gets an answer consistent with
the snapshot, just sometimes a plainer one.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from datetime import UTC, datetime, timedelta
from typing import Any
from uuid import uuid4

from industrial_ops_agent.guardrails.network_causal import (
    CausalContext,
    NetworkCausalGuardrail,
)
from industrial_ops_agent.model_gateway.context_manifest import ContextReference
from industrial_ops_agent.model_gateway.service import (
    EnvironmentAwareModelResolver,
    GatewayRequest,
    ModelGateway,
    ModelGatewayError,
)
from industrial_ops_agent.network_assurance.service import NetworkAssuranceNotFound
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.tenant import TenantContext
from industrial_ops_agent.prompting.registry import (
    NETWORK_CAUSAL_DIAGNOSIS_PROMPT_BUNDLE_ID,
    PromptBundleNotDeployed,
    default_prompt_registry,
)

#: Matched case-insensitively against asset ids appearing in the question, so
#: "what happened to line-a-gateway" binds to that device without a picker.
_WORD = r"[A-Za-z0-9._-]+"

#: The gateway transport model alias. The copilot explains an edge verdict, it
#: does not re-evaluate it, so it reuses the governed diagnosis alias rather
#: than introducing a second model route to govern.
_COPILOT_MODEL_ALIAS = "industrial-diagnosis"

#: Output cap. The diagnosis report is a short structured object; a larger
#: budget only buys longer hedging.
_MAX_OUTPUT_TOKENS = 1_024

#: Wall-clock budget for one copilot inference. Interactive panel.
_INFERENCE_TIMEOUT_SECONDS = 30.0


@dataclass(frozen=True, slots=True)
class CopilotAnswer:
    """A diagnosis that has already passed the causal guardrail."""

    asset_id: str
    overall_state: str
    primary_issue: str | None
    causal_chain: tuple[dict[str, str], ...]
    evidence_refs: tuple[str, ...]
    recommended_actions: tuple[str, ...]
    answer: str
    model_used: bool


class NetworkCopilotService:
    """Answer operator questions from the snapshot, guarded against inversion."""

    def __init__(
        self,
        database: Database,
        network_assurance_service: Any,
        *,
        model_gateway: ModelGateway | None = None,
        model_resolver: EnvironmentAwareModelResolver | None = None,
        guardrail: NetworkCausalGuardrail | None = None,
    ) -> None:
        self._database = database
        self._assurance = network_assurance_service
        self._model_gateway = model_gateway
        self._model_resolver = model_resolver
        self._guardrail = guardrail or NetworkCausalGuardrail()

    def diagnose(
        self,
        context: TenantContext,
        question: str,
        *,
        asset_id: str | None = None,
    ) -> CopilotAnswer:
        """Diagnose one device, or the fleet's worst-scoring device."""

        if asset_id is None:
            asset_id = self._select_asset(context, question)
        try:
            detail = self._assurance.get_asset(context, asset_id)
        except NetworkAssuranceNotFound as exc:
            raise NetworkAssuranceNotFound(asset_id) from exc

        snapshot = (detail.latest_experience or {}) if detail else {}
        timeline = self._assurance.timeline(context, asset_id, window="24h")

        answer = self._deterministic_answer(asset_id, snapshot)
        model_report = self._model_report(
            context,
            asset_id,
            snapshot=snapshot,
            timeline=[point.__dict__ for point in timeline[-20:]],
        )
        if model_report is not None:
            verdict = self._guardrail.inspect_json_report(
                model_report,
                CausalContext(device_id=asset_id, snapshot=snapshot),
            )
            if verdict.decision == "ALLOWED":
                return self._from_model_report(asset_id, model_report, answer)
        return answer

    # ------------------------------------------------------------------
    # Device selection
    # ------------------------------------------------------------------

    def _select_asset(self, context: TenantContext, question: str) -> str:
        """Pick the device the question is about.

        An explicit asset id in the question wins. Otherwise the question is
        about "the problem", and the fleet's worst-scoring device is the one
        the operator is most plausibly looking at.
        """

        import re

        assets = self._assurance.list_assets(context)
        if not assets:
            raise NetworkAssuranceNotFound("no managed edge devices for this tenant")
        words = set(re.findall(_WORD, question))
        for asset in assets:
            if asset.asset_id in words:
                return str(asset.asset_id)
        worst = min(assets, key=lambda asset: asset.display_score)
        return str(worst.asset_id)

    # ------------------------------------------------------------------
    # Deterministic causal chain
    # ------------------------------------------------------------------

    def _deterministic_answer(self, asset_id: str, snapshot: dict[str, Any]) -> CopilotAnswer:
        overall = str(snapshot.get("overall_state", "UNKNOWN"))
        primary = snapshot.get("primary_issue")
        primary_text = primary if isinstance(primary, str) else None

        network = self._sle_states(snapshot, "network_health")
        service = self._sle_states(snapshot, "service_health")
        evidence = self._evidence_metrics(snapshot)

        chain, actions = self._causal_chain(network, service, evidence)

        lines = [f"设备 {asset_id} 当前评估：{overall}。"]
        if primary_text:
            lines.append(f"边缘判定的首要问题：{primary_text}。")
        lines.append("因果链（依据设备不可变快照，按边缘评估的因果排序）：")
        for step in chain:
            lines.append(f"{step['step']}. {step['explanation']}")
        if actions:
            lines.append("建议（未执行，仅提议）：" + "；".join(actions) + "。")

        return CopilotAnswer(
            asset_id=asset_id,
            overall_state=overall,
            primary_issue=primary_text,
            causal_chain=tuple(chain),
            evidence_refs=tuple(sorted(evidence)),
            recommended_actions=tuple(actions),
            answer="\n".join(lines),
            model_used=False,
        )

    def _causal_chain(
        self,
        network: dict[str, str],
        service: dict[str, str],
        evidence: frozenset[str],
    ) -> tuple[list[dict[str, str]], list[str]]:
        """Derive the causal chain in the evaluator's own ordering.

        Reachability is the one-vote override: when it is BAD, every
        higher-layer symptom is a consequence of the gateway being
        unreachable, never a cause (R1). A received HTTP response of any
        status proves the transport, so a failing service check there is
        reported as server-side (R2).
        """

        chain: list[dict[str, str]] = []
        actions: list[str] = []
        step = 1

        reachability = network.get("reachability")
        if reachability == "BAD":
            chain.append(
                {
                    "step": str(step),
                    "explanation": "网关不可达（reachability=BAD），这是边缘评估的一票否决项："
                    "在此之下的任何高层症状（解析、传输、服务）都只能视为它的后果，不是原因。",
                }
            )
            step += 1
            actions.append("检查设备到网关的链路与网关本身，恢复二层/三层连通后再复核上层结论")
            return chain, actions

        dns = network.get("dns_resolution")
        if dns in {"BAD", "DEGRADED"}:
            chain.append(
                {
                    "step": str(step),
                    "explanation": f"DNS 解析状态为 {dns}（网关可达），说明解析链路本身出现劣化。",
                }
            )
            step += 1
            actions.append("核对设备配置的 DNS 服务器与解析延迟指标")

        transport = network.get("transport")
        if transport in {"BAD", "DEGRADED"}:
            chain.append(
                {
                    "step": str(step),
                    "explanation": f"传输层状态为 {transport}（网关可达）。",
                }
            )
            step += 1
            actions.append("结合重传/时延指标定位传输劣化区段")

        for name, state in sorted(service.items()):
            if state == "GOOD":
                continue
            if state == "BAD" and "http" in name:
                # R2's mirror image: this module never calls the network down
                # over an HTTP error status — the response arriving proves
                # the path works; the fault is server-side.
                chain.append(
                    {
                        "step": str(step),
                        "explanation": f"服务检查 {name}=BAD：若为 HTTP 错误响应，传输已证明可用，"
                        "故障在服务端而非网络。",
                    }
                )
            else:
                chain.append(
                    {
                        "step": str(step),
                        "explanation": f"服务检查 {name}={state}。",
                    }
                )
            step += 1

        if not chain:
            chain.append(
                {
                    "step": "1",
                    "explanation": "快照各维度未显示明确劣化。证据不足时不给结论（与边缘评估的"
                    "“无新证据不推进状态”规则一致），建议补充观测。",
                }
            )
            actions.append("延长观测窗口，关注下一次评估周期")
        elif evidence:
            chain.append(
                {
                    "step": str(step),
                    "explanation": f"以上判断引用的设备证据指标：{', '.join(sorted(evidence))}。",
                }
            )

        return chain, actions

    def _sle_states(self, snapshot: dict[str, Any], group: str) -> dict[str, str]:
        section = snapshot.get(group)
        if not isinstance(section, dict):
            return {}
        states: dict[str, str] = {}
        for name, result in section.items():
            if isinstance(result, dict) and isinstance(result.get("state"), str):
                states[name] = result["state"]
        return states

    def _evidence_metrics(self, snapshot: dict[str, Any]) -> frozenset[str]:
        metrics: set[str] = set()
        for group in ("network_health", "service_health"):
            section = snapshot.get(group)
            if not isinstance(section, dict):
                continue
            for result in section.values():
                if not isinstance(result, dict):
                    continue
                for item in result.get("evidence", []) or []:
                    if isinstance(item, dict) and isinstance(item.get("metric"), str):
                        metrics.add(item["metric"].lower())
        return frozenset(metrics)

    # ------------------------------------------------------------------
    # Model path (optional, guarded)
    # ------------------------------------------------------------------

    def _model_report(
        self,
        context: TenantContext,
        asset_id: str,
        *,
        snapshot: dict[str, Any],
        timeline: list[dict[str, Any]],
    ) -> dict[str, Any] | None:
        """Ask the governed model to explain the chain; None means fall back."""

        if self._model_gateway is None or self._model_resolver is None:
            return None
        try:
            prompt = default_prompt_registry().get(NETWORK_CAUSAL_DIAGNOSIS_PROMPT_BUNDLE_ID)
            resolved = self._model_resolver.resolve(context, _COPILOT_MODEL_ALIAS)
        except (PromptBundleNotDeployed, ModelGatewayError):
            return None

        payload = {
            "device_id": asset_id,
            "device_snapshot": snapshot,
            "timeline": timeline,
            "instruction": (
                "Explain the causal chain behind the edge's own assessment. "
                "Do not re-evaluate it; an unreachable gateway outranks every "
                "higher-layer conclusion."
            ),
        }
        request = GatewayRequest(
            inference_request_id=f"inference-copilot-{uuid4().hex}",
            model_alias=_COPILOT_MODEL_ALIAS,
            required_release_id=resolved.release_id,
            subject_id=context.subject_id,
            trace_id=f"network-copilot:{asset_id}",
            request_class="DIAGNOSIS",
            data_classification="CONFIDENTIAL",
            messages=(
                {"role": "system", "content": prompt.render_system()},
                {
                    "role": "user",
                    "content": json.dumps(
                        payload,
                        ensure_ascii=False,
                        sort_keys=True,
                        separators=(",", ":"),
                    ),
                },
            ),
            response_schema_name=prompt.output_schema_name,
            response_schema=_report_schema(),
            deadline=datetime.now(UTC) + timedelta(seconds=_INFERENCE_TIMEOUT_SECONDS),
            max_output_tokens=_MAX_OUTPUT_TOKENS,
            temperature=0.0,
            prompt_bundle_id=prompt.prompt_bundle_id,
            prompt_bundle_hash=prompt.content_hash,
            context_evidence=(
                ContextReference(
                    reference_id=f"snapshot:{asset_id}",
                    content_hash=resolved.manifest_hash,
                ),
            ),
        )
        try:
            response = self._loop_run(self._model_gateway.complete(context, request))
        except ModelGatewayError:
            return None
        content = response.content
        return content if isinstance(content, dict) else None

    def _loop_run(self, coroutine: Any) -> Any:
        """Run the gateway coroutine from this sync service boundary.

        The FastAPI handler runs in the event loop, so ``diagnose`` is called
        with a pre-resolved coroutine only when the caller drives an async
        context; the route entrypoint awaits :meth:`diagnose_async` instead.
        """

        import asyncio

        return asyncio.get_running_loop().run_until_complete(coroutine)

    async def diagnose_async(
        self,
        context: TenantContext,
        question: str,
        *,
        asset_id: str | None = None,
    ) -> CopilotAnswer:
        """Async entrypoint for the HTTP route; mirrors :meth:`diagnose`."""

        if asset_id is None:
            asset_id = self._select_asset(context, question)
        try:
            detail = self._assurance.get_asset(context, asset_id)
        except NetworkAssuranceNotFound as exc:
            raise NetworkAssuranceNotFound(asset_id) from exc

        snapshot = (detail.latest_experience or {}) if detail else {}
        timeline = await self._assurance_timeline_async(context, asset_id)

        answer = self._deterministic_answer(asset_id, snapshot)
        model_report = None
        if self._model_gateway is not None and self._model_resolver is not None:
            model_report = self._model_report_with(
                context,
                asset_id,
                snapshot=snapshot,
                timeline=[point.__dict__ for point in timeline[-20:]],
            )
            if model_report is not None:
                verdict = self._guardrail.inspect_json_report(
                    model_report,
                    CausalContext(device_id=asset_id, snapshot=snapshot),
                )
                if verdict.decision == "ALLOWED":
                    return self._from_model_report(asset_id, model_report, answer)
        return answer

    async def _assurance_timeline_async(
        self,
        context: TenantContext,
        asset_id: str,
    ) -> list[Any]:
        points: list[Any] = list(
            self._assurance.timeline(context, asset_id, window="24h")
        )
        return points

    def _model_report_with(
        self,
        context: TenantContext,
        asset_id: str,
        *,
        snapshot: dict[str, Any],
        timeline: list[dict[str, Any]],
    ) -> dict[str, Any] | None:
        """Synchronous model call wrapper; returns None on any gateway failure."""

        if self._model_gateway is None or self._model_resolver is None:
            return None
        try:
            prompt = default_prompt_registry().get(NETWORK_CAUSAL_DIAGNOSIS_PROMPT_BUNDLE_ID)
            resolved = self._model_resolver.resolve(context, _COPILOT_MODEL_ALIAS)
        except (PromptBundleNotDeployed, ModelGatewayError):
            return None
        payload = {
            "device_id": asset_id,
            "device_snapshot": snapshot,
            "timeline": timeline,
            "instruction": (
                "Explain the causal chain behind the edge's own assessment. "
                "Do not re-evaluate it; an unreachable gateway outranks every "
                "higher-layer conclusion."
            ),
        }
        request = GatewayRequest(
            inference_request_id=f"inference-copilot-{uuid4().hex}",
            model_alias=_COPILOT_MODEL_ALIAS,
            required_release_id=resolved.release_id,
            subject_id=context.subject_id,
            trace_id=f"network-copilot:{asset_id}",
            request_class="DIAGNOSIS",
            data_classification="CONFIDENTIAL",
            messages=(
                {"role": "system", "content": prompt.render_system()},
                {
                    "role": "user",
                    "content": json.dumps(
                        payload,
                        ensure_ascii=False,
                        sort_keys=True,
                        separators=(",", ":"),
                    ),
                },
            ),
            response_schema_name=prompt.output_schema_name,
            response_schema=_report_schema(),
            deadline=datetime.now(UTC) + timedelta(seconds=_INFERENCE_TIMEOUT_SECONDS),
            max_output_tokens=_MAX_OUTPUT_TOKENS,
            temperature=0.0,
            prompt_bundle_id=prompt.prompt_bundle_id,
            prompt_bundle_hash=prompt.content_hash,
            context_evidence=(
                ContextReference(
                    reference_id=f"snapshot:{asset_id}",
                    content_hash=resolved.manifest_hash,
                ),
            ),
        )
        try:
            response = self._loop_run(self._model_gateway.complete(context, request))
        except ModelGatewayError:
            return None
        content = response.content
        return content if isinstance(content, dict) else None

    def _from_model_report(
        self,
        asset_id: str,
        report: dict[str, Any],
        fallback: CopilotAnswer,
    ) -> CopilotAnswer:
        chain_raw = report.get("causal_chain")
        chain: list[dict[str, str]] = []
        if isinstance(chain_raw, list):
            for item in chain_raw:
                if (
                    isinstance(item, dict)
                    and isinstance(item.get("step"), str)
                    and isinstance(item.get("explanation"), str)
                ):
                    chain.append({"step": item["step"], "explanation": item["explanation"]})
        if not chain:
            return fallback
        primary = report.get("primary_issue")
        overall = report.get("overall_state")
        lines = [f"设备 {asset_id} 诊断（模型解释，已通过因果护栏校验）："]
        for step in chain:
            lines.append(f"{step['step']}. {step['explanation']}")
        refs = report.get("evidence_refs")
        if isinstance(refs, list) and refs:
            lines.append("引用证据：" + ", ".join(str(r) for r in refs) + "。")
        actions = report.get("recommended_actions")
        if isinstance(actions, list) and actions:
            lines.append("建议（未执行，仅提议）：" + "；".join(str(a) for a in actions) + "。")
        return CopilotAnswer(
            asset_id=asset_id,
            overall_state=str(overall) if isinstance(overall, str) else fallback.overall_state,
            primary_issue=str(primary) if isinstance(primary, str) else fallback.primary_issue,
            causal_chain=tuple(chain),
            evidence_refs=tuple(str(r) for r in refs) if isinstance(refs, list) else (),
            recommended_actions=(
                tuple(str(a) for a in actions) if isinstance(actions, list) else ()
            ),
            answer="\n".join(lines),
            model_used=True,
        )


def _report_schema() -> dict[str, Any]:
    """JSON schema for the model's structured diagnosis report."""

    step_object = {
        "type": "object",
        "additionalProperties": False,
        "required": ["step", "explanation"],
        "properties": {
            "step": {"type": "string", "minLength": 1, "maxLength": 8},
            "explanation": {"type": "string", "minLength": 1, "maxLength": 1_000},
        },
    }
    return {
        "type": "object",
        "additionalProperties": False,
        "required": [
            "overall_state",
            "causal_chain",
            "primary_issue",
            "evidence_refs",
            "recommended_actions",
        ],
        "properties": {
            "overall_state": {
                "type": "string",
                "enum": ["GOOD", "DEGRADED", "BAD", "UNKNOWN"],
            },
            "causal_chain": {
                "type": "array",
                "minItems": 1,
                "maxItems": 10,
                "items": step_object,
            },
            "primary_issue": {"type": ["string", "null"], "maxLength": 500},
            "evidence_refs": {
                "type": "array",
                "maxItems": 20,
                "items": {"type": "string", "minLength": 1, "maxLength": 128},
            },
            "recommended_actions": {
                "type": "array",
                "maxItems": 10,
                "items": {"type": "string", "minLength": 1, "maxLength": 500},
            },
        },
    }

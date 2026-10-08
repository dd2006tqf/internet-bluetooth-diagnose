"""Edge telemetry intake and operator-facing network assurance endpoints.

## Two distinct trust boundaries

The device endpoints (``/edge/...``) are authenticated by a device token plus
an Ed25519 signature, *not* by OIDC. An unattended edge device has no user
identity to present, and giving it a service account would make every device
share one principal. Devices are therefore authenticated as themselves.

The operator endpoints authenticate through the normal OIDC dependency and
authorize through the policy boundary, because reading a fleet's network
posture across tenants is an ordinary user action with ordinary RBAC.

Keeping the two paths in one module is deliberate: they describe the same
resource, and splitting them would scatter the tenant-scoping rules that
connect them.

## Why the raw body is read before parsing

``await request.body()`` is used instead of declaring a Pydantic body
parameter, because FastAPI would parse (and thus re-serialize for anything
downstream) the payload before the signature could be checked. Verification
must happen against the bytes that actually arrived. See
:mod:`industrial_ops_agent.network_assurance.signing`.
"""

from __future__ import annotations

import json
import logging
from datetime import datetime
from typing import Annotated, Any, Literal

from fastapi import APIRouter, Body, Depends, Header, Query, Request, Response
from pydantic import BaseModel, Field, ValidationError

from industrial_ops_agent.api.dependencies import (
    get_authorizer,
    get_edge_telemetry_verifier,
    get_identity,
    get_network_action_approval_service,
    get_network_assurance_service,
    get_network_copilot_service,
    get_network_council_service,
    get_risk_prediction_service,
    get_wireless_diagnosis_service,
)
from industrial_ops_agent.api.errors import STANDARD_ERROR_RESPONSES, AppError
from industrial_ops_agent.api.versions import numeric_precondition as _version
from industrial_ops_agent.auth.identity import IdentityContext
from industrial_ops_agent.auth.policy import Action, Authorizer, ResourceContext
from industrial_ops_agent.network_assurance.contracts import (
    MAX_TELEMETRY_BODY_BYTES,
    NetworkActionResults,
    NetworkAssetDetail,
    NetworkAssetSummary,
    NetworkTelemetryBatch,
    NetworkTimelinePoint,
    SiteSummary,
)
from industrial_ops_agent.network_assurance.copilot import NetworkCopilotService
from industrial_ops_agent.network_assurance.model_config import (
    CopilotModelConfigRequest,
    CopilotModelConfigResponse,
    CopilotModelTestRequest,
    CopilotModelTestResponse,
    get_model_config_manager,
)
from industrial_ops_agent.network_assurance.service import (
    NetworkAssuranceConflict,
    NetworkAssuranceNotFound,
    NetworkAssuranceService,
    edge_subject_id,
)
from industrial_ops_agent.network_assurance.signing import (
    EdgeTelemetryVerifier,
    NetworkSignatureError,
)
from industrial_ops_agent.network_assurance.wireless_contracts import (
    PredictionResult,
    WirelessEventIngestResult,
    WirelessEventUplinkBatch,
)
from industrial_ops_agent.persistence.tenant import TenantContext, validate_boundary_identifier

router = APIRouter(prefix="/network", tags=["network-assurance"])


class EdgeTelemetryAcceptedResponse(BaseModel):
    """Acknowledgement returned to the device.

    ``pending_actions`` rides on the response rather than a separate channel
    so a device behind NAT needs only outbound connectivity. The device
    applies them and reports outcomes on its next upload.
    """

    accepted: int
    duplicates: int
    pending_actions: list[dict[str, Any]] = Field(default_factory=list)


class NetworkTimelineResponse(BaseModel):
    asset_id: str
    window: str
    points: list[NetworkTimelinePoint]


class NetworkActionRequest(BaseModel):
    config_key: str = Field(min_length=3, max_length=128)
    config_value: str = Field(min_length=1, max_length=512)


class NetworkActionResponse(BaseModel):
    action_id: str
    status: str


class CopilotQuestionRequest(BaseModel):
    question: str = Field(min_length=1, max_length=2_000)


class CopilotAnswerResponse(BaseModel):
    asset_id: str
    overall_state: str
    primary_issue: str | None
    causal_chain: list[dict[str, str]]
    evidence_refs: list[str]
    recommended_actions: list[str]
    answer: str
    model_used: bool


async def _read_verified_body(
    request: Request,
    verifier: EdgeTelemetryVerifier,
) -> bytes:
    """Authenticate a device submission and return the bytes it signed.

    Device credentials travel in headers and are read from the request
    directly: this helper is awaited inline by the route (not resolved
    through `Depends`), so `Header(...)` parameter declarations on it would
    never be populated by FastAPI and every request would fail as
    `edge_credentials_missing`.

    Every rejection is reported identically to the caller. The reason codes
    below appear only in server-side logs; distinguishing "unknown key id"
    from "bad signature" in a response would let an unauthenticated peer
    enumerate provisioned key ids.
    """

    key_id = request.headers.get("X-Edge-Key-Id")
    token = request.headers.get("X-Edge-Token")
    signature = request.headers.get("X-Edge-Signature")
    if key_id is None or token is None or signature is None:
        raise _unauthorized("edge_credentials_missing")

    raw = await request.body()
    if not raw:
        raise _bad_request("edge_body_empty")
    if len(raw) > MAX_TELEMETRY_BODY_BYTES:
        raise AppError(
            status_code=413,
            code="edge_body_too_large",
            category="validation",
            message="Telemetry payload exceeds the accepted size",
        )

    try:
        body = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise _bad_request("edge_body_not_json") from exc
    if not isinstance(body, dict):
        raise _bad_request("edge_body_not_an_object")

    # The device id is read out of the (still unverified) body purely to bind
    # it into the signing check; it is not trusted until verify() returns.
    device_id = _extract_device_id(body)

    try:
        verifier.verify(
            presented_key_id=key_id,
            presented_token=token.encode(),
            signature_hex=signature,
            body_bytes=raw,
            device_id=device_id,
        )
    except NetworkSignatureError as exc:
        raise _unauthorized(exc.reason) from exc

    return raw


def _extract_device_id(body: dict[str, object]) -> str:
    # 优先支持 action-results 报文直接在顶层携带的 device_id
    top_device_id = body.get("device_id")
    if isinstance(top_device_id, str) and top_device_id:
        return top_device_id

    snapshots = body.get("snapshots")
    if not isinstance(snapshots, list) or not snapshots:
        raise _bad_request("edge_snapshots_missing")
    first = snapshots[0]
    if not isinstance(first, dict):
        raise _bad_request("edge_snapshot_not_an_object")
    device_id = first.get("device_id")
    if not isinstance(device_id, str) or not device_id:
        raise _bad_request("edge_device_id_missing")
    return device_id


def _unauthorized(reason: str) -> AppError:
    return AppError(
        status_code=401,
        code="edge_authentication_failed",
        category="authentication",
        message="Edge telemetry authentication failed",
    )


def _bad_request(code: str) -> AppError:
    return AppError(
        status_code=400,
        code=code,
        category="validation",
        message="Edge telemetry payload is not acceptable",
    )


def _edge_tenant_id(request: Request, verifier: EdgeTelemetryVerifier) -> str:
    """Resolve the tenant for one device submission, server side first.

    The ``X-Edge-Tenant`` header is fully device-controlled: trusting it
    outright lets any provisioned device write into any tenant by editing
    one header. When the deployment binds the trust anchor to a tenant
    (``network_edge_telemetry_tenant_id``), that configured value is the
    authority — a disagreeing header is rejected as a provisioning bug or a
    spoof attempt, and an absent header is fine. The header only keeps
    deciding when no binding is configured (multi-tenant anchors and
    rehearsals), preserving existing deployments.

    Called *after* signature verification so the header comparison happens
    on authenticated requests only.
    """

    bound = verifier.tenant_id
    if bound is not None:
        claimed = request.headers.get("X-Edge-Tenant")
        if claimed is not None and claimed != bound:
            raise AppError(
                status_code=403,
                code="edge_tenant_mismatch",
                category="authorization",
                message="Edge device tenant does not match the configured trust anchor",
            )
        return bound
    claimed = request.headers.get("X-Edge-Tenant")
    if claimed is None:
        raise _bad_request("edge_tenant_invalid")
    return claimed


# ----------------------------------------------------------------------
# Device-facing endpoints
# ----------------------------------------------------------------------


@router.post(
    "/edge/telemetry",
    response_model=EdgeTelemetryAcceptedResponse,
    responses=STANDARD_ERROR_RESPONSES,
    summary="Ingest a signed batch of edge network assessments",
)
async def ingest_edge_telemetry(
    request: Request,
    verifier: Annotated[EdgeTelemetryVerifier, Depends(get_edge_telemetry_verifier)],
    service: Annotated[NetworkAssuranceService, Depends(get_network_assurance_service)],
) -> EdgeTelemetryAcceptedResponse:
    """Accept a batch, tolerating replay after a lost acknowledgement."""

    raw = await _read_verified_body(request, verifier)

    try:
        batch = NetworkTelemetryBatch.model_validate_json(raw)
    except ValidationError as exc:
        # A signature-valid but structurally invalid payload means the device
        # is running an incompatible version, which is worth failing loudly
        # rather than partially accepting.
        import logging

        logging.getLogger("uvicorn.error").error(
            "422 edge validation failure: %s, raw: %s",
            exc.errors(),
            raw.decode(errors="replace")[:500],
        )
        raise AppError(
            status_code=422,
            code="edge_payload_invalid",
            category="validation",
            message="Edge telemetry payload failed contract validation",
            details={"errors": exc.error_count()},
        ) from exc

    try:
        context = TenantContext(
            tenant_id=_edge_tenant_id(request, verifier),
            subject_id=edge_subject_id(batch.snapshots[0].device_id),
        )
    except ValueError as exc:
        raise _bad_request("edge_tenant_invalid") from exc

    try:
        result = service.ingest_batch(context, batch, key_id=verifier.key_id)
    except NetworkAssuranceConflict as exc:
        raise AppError(
            status_code=409,
            code="edge_device_tenant_conflict",
            category="conflict",
            message="Device is registered to another tenant",
        ) from exc

    return EdgeTelemetryAcceptedResponse(
        accepted=result.accepted,
        duplicates=result.duplicates,
        pending_actions=list(result.pending_actions),
    )


@router.post(
    "/edge/wireless-events",
    response_model=WirelessEventIngestResult,
    responses=STANDARD_ERROR_RESPONSES,
    summary="Ingest a signed batch of edge wireless events/incidents/baselines",
)
async def ingest_edge_wireless_events(
    request: Request,
    verifier: Annotated[EdgeTelemetryVerifier, Depends(get_edge_telemetry_verifier)],
    service: Annotated[NetworkAssuranceService, Depends(get_network_assurance_service)],
) -> WirelessEventIngestResult:
    """接受板端无线事实上行（Phase 4a 独立端点，决策 D1）。

    与遥测端点共享验签前置（`_read_verified_body`：先验签后解析，拒绝
    原因不回显），但幂等键分域：这里按 event_id / incident_id 计数，
    不看 network_epoch/sequence。重放是常态路径（板端游标在确认前不前移）。
    """

    raw = await _read_verified_body(request, verifier)

    try:
        batch = WirelessEventUplinkBatch.model_validate_json(raw)
    except ValidationError as exc:
        import logging

        logging.getLogger("uvicorn.error").error(
            "422 edge wireless validation failure: %s, raw: %s",
            exc.errors(),
            raw.decode(errors="replace")[:500],
        )
        raise AppError(
            status_code=422,
            code="edge_payload_invalid",
            category="validation",
            message="Edge wireless payload failed contract validation",
            details={"errors": exc.error_count()},
        ) from exc

    try:
        context = TenantContext(
            tenant_id=_edge_tenant_id(request, verifier),
            subject_id=edge_subject_id(batch.device_id),
        )
    except ValueError as exc:
        raise _bad_request("edge_tenant_invalid") from exc

    try:
        return service.ingest_wireless_batch(context, batch, key_id=verifier.key_id)
    except NetworkAssuranceConflict as exc:
        raise AppError(
            status_code=409,
            code="edge_device_tenant_conflict",
            category="conflict",
            message="Device is registered to another tenant",
        ) from exc


@router.post(
    "/edge/action-results",
    status_code=204,
    responses=STANDARD_ERROR_RESPONSES,
    summary="Report outcomes of previously delivered configuration actions",
)
async def report_edge_action_results(
    request: Request,
    verifier: Annotated[EdgeTelemetryVerifier, Depends(get_edge_telemetry_verifier)],
    service: Annotated[NetworkAssuranceService, Depends(get_network_assurance_service)],
) -> Response:
    raw = await _read_verified_body(request, verifier)
    try:
        results = NetworkActionResults.model_validate_json(raw)
    except ValidationError as exc:
        raise AppError(
            status_code=422,
            code="edge_payload_invalid",
            category="validation",
            message="Action result payload failed contract validation",
            details={"errors": exc.error_count()},
        ) from exc

    try:
        context = TenantContext(
            tenant_id=_edge_tenant_id(request, verifier),
            subject_id=edge_subject_id(results.device_id),
        )
    except ValueError as exc:
        raise _bad_request("edge_tenant_invalid") from exc
    service.record_action_results(context, results)
    return Response(status_code=204)


# ----------------------------------------------------------------------
# Operator-facing endpoints
# ----------------------------------------------------------------------


@router.get(
    "/assets",
    response_model=list[NetworkAssetSummary],
    responses=STANDARD_ERROR_RESPONSES,
    summary="List managed edge devices and their current network posture",
)
async def list_network_assets(
    request: Request,
    identity: Annotated[IdentityContext, Depends(get_identity)],
    authorizer: Annotated[Authorizer, Depends(get_authorizer)],
    service: Annotated[NetworkAssuranceService, Depends(get_network_assurance_service)],
) -> list[NetworkAssetSummary]:
    authorizer.require(
        identity,
        Action.READ_NETWORK_ASSURANCE,
        ResourceContext(identity.tenant_id),
        request_id=getattr(request.state, "request_id", "unavailable"),
    )
    return service.list_assets(identity.tenant_context)


@router.get(
    "/sites",
    response_model=list[SiteSummary],
    responses=STANDARD_ERROR_RESPONSES,
    summary="List field sites and the gateways reporting facts for each",
)
async def list_field_sites(
    request: Request,
    identity: Annotated[IdentityContext, Depends(get_identity)],
    authorizer: Annotated[Authorizer, Depends(get_authorizer)],
    service: Annotated[NetworkAssuranceService, Depends(get_network_assurance_service)],
) -> list[SiteSummary]:
    """M2 现场聚合查询出口：回答"这个现场有哪几台网关"。

    诊断侧的跨网关窗口聚合发生在 wireless_diagnosis 内部，运维此前没有
    任何入口看到现场级的网关清单。site/gateway 身份只随事实上行携带，
    所以清单由三张事实表取并集得出（见 ``service.list_sites``）。
    """

    authorizer.require(
        identity,
        Action.READ_NETWORK_ASSURANCE,
        ResourceContext(identity.tenant_id),
        request_id=getattr(request.state, "request_id", "unavailable"),
    )
    return service.list_sites(identity.tenant_context)


@router.get(
    "/assets/{asset_id}",
    response_model=NetworkAssetDetail,
    responses=STANDARD_ERROR_RESPONSES,
    summary="Read one managed edge device across all five dimensions",
)
async def get_network_asset(
    request: Request,
    asset_id: str,
    identity: Annotated[IdentityContext, Depends(get_identity)],
    authorizer: Annotated[Authorizer, Depends(get_authorizer)],
    service: Annotated[NetworkAssuranceService, Depends(get_network_assurance_service)],
) -> NetworkAssetDetail:
    _require_read_scope(request, identity, authorizer, asset_id)
    try:
        return service.get_asset(identity.tenant_context, asset_id)
    except NetworkAssuranceNotFound as exc:
        raise _not_found(asset_id) from exc


@router.get(
    "/assets/{asset_id}/timeline",
    response_model=NetworkTimelineResponse,
    responses=STANDARD_ERROR_RESPONSES,
    summary="Read the assessment timeline that shows how a fault evolved",
)
async def get_network_timeline(
    request: Request,
    asset_id: str,
    identity: Annotated[IdentityContext, Depends(get_identity)],
    authorizer: Annotated[Authorizer, Depends(get_authorizer)],
    service: Annotated[NetworkAssuranceService, Depends(get_network_assurance_service)],
    window: Annotated[str, Query(pattern=r"^(15m|1h|6h|24h)$")] = "15m",
) -> NetworkTimelineResponse:
    _require_read_scope(request, identity, authorizer, asset_id)
    try:
        points = service.timeline(identity.tenant_context, asset_id, window=window)
    except NetworkAssuranceNotFound as exc:
        raise _not_found(asset_id) from exc
    return NetworkTimelineResponse(asset_id=asset_id, window=window, points=points)


@router.post(
    "/assets/{asset_id}/actions",
    response_model=NetworkActionResponse,
    responses=STANDARD_ERROR_RESPONSES,
    summary="Queue a configuration change for an edge device",
)
async def queue_network_action(
    request: Request,
    asset_id: str,
    body: Annotated[NetworkActionRequest, Body()],
    identity: Annotated[IdentityContext, Depends(get_identity)],
    authorizer: Annotated[Authorizer, Depends(get_authorizer)],
    service: Annotated[NetworkAssuranceService, Depends(get_network_assurance_service)],
) -> NetworkActionResponse:
    """Queue a change; the device applies and re-validates it on next upload.

    Deliberately *not* forwarded to the device here. The edge owns a
    parameter whitelist with range checks, and it is the only party that can
    observe whether a change took effect. The server records intent; the
    device records outcome.
    """

    authorizer.require(
        identity,
        Action.MANAGE_NETWORK_DEVICE,
        ResourceContext(identity.tenant_id, resource_id=asset_id),
        request_id=getattr(request.state, "request_id", "unavailable"),
    )
    try:
        action_id = service.queue_action(
            identity.tenant_context,
            asset_id,
            config_key=body.config_key,
            config_value=body.config_value,
            issued_by=identity.subject_id,
        )
    except NetworkAssuranceNotFound as exc:
        raise _not_found(asset_id) from exc
    return NetworkActionResponse(action_id=action_id, status="QUEUED")


@router.post(
    "/copilot",
    response_model=CopilotAnswerResponse,
    responses=STANDARD_ERROR_RESPONSES,
    summary="Explain why a device's network is degraded, guarded against causal inversion",
)
async def network_copilot(
    request: Request,
    body: Annotated[CopilotQuestionRequest, Body()],
    identity: Annotated[IdentityContext, Depends(get_identity)],
    authorizer: Annotated[Authorizer, Depends(get_authorizer)],
    service: Annotated[NetworkAssuranceService, Depends(get_network_assurance_service)],
    copilot: Annotated[NetworkCopilotService, Depends(get_network_copilot_service)],
) -> CopilotAnswerResponse:
    """Diagnose from the immutable snapshot, never against it.

    The deterministic chain is derived first and the model (when configured)
    only explains it; a blocked or failed model report falls back to the
    deterministic answer, so the panel never shows a claim the snapshot
    contradicts.
    """

    try:
        answer = await copilot.diagnose_async(identity.tenant_context, body.question)
    except NetworkAssuranceNotFound as exc:
        raise _not_found(str(exc)) from exc
    return CopilotAnswerResponse(
        asset_id=answer.asset_id,
        overall_state=answer.overall_state,
        primary_issue=answer.primary_issue,
        causal_chain=list(answer.causal_chain),
        evidence_refs=list(answer.evidence_refs),
        recommended_actions=list(answer.recommended_actions),
        answer=answer.answer,
        model_used=answer.model_used,
    )


@router.get(
    "/copilot/config",
    response_model=CopilotModelConfigResponse,
    responses=STANDARD_ERROR_RESPONSES,
    summary="Get current hot model gateway configuration",
)
async def get_copilot_model_config(
    request: Request,
    identity: Annotated[IdentityContext, Depends(get_identity)],
    authorizer: Annotated[Authorizer, Depends(get_authorizer)],
) -> CopilotModelConfigResponse:
    authorizer.require(
        identity,
        Action.READ_NETWORK_ASSURANCE,
        ResourceContext(identity.tenant_id),
        request_id=getattr(request.state, "request_id", "unavailable"),
    )
    return get_model_config_manager().get_public_view()


@router.post(
    "/copilot/config",
    response_model=CopilotModelConfigResponse,
    responses=STANDARD_ERROR_RESPONSES,
    summary="Update hot model gateway configuration without restart",
)
async def update_copilot_model_config(
    request: Request,
    body: Annotated[CopilotModelConfigRequest, Body()],
    identity: Annotated[IdentityContext, Depends(get_identity)],
    authorizer: Annotated[Authorizer, Depends(get_authorizer)],
) -> CopilotModelConfigResponse:
    authorizer.require(
        identity,
        Action.MANAGE_NETWORK_DEVICE,
        ResourceContext(identity.tenant_id),
        request_id=getattr(request.state, "request_id", "unavailable"),
    )
    return get_model_config_manager().update_config(body)


@router.post(
    "/copilot/config/test",
    response_model=CopilotModelTestResponse,
    responses=STANDARD_ERROR_RESPONSES,
    summary="Test model gateway connection with provided parameters",
)
async def test_copilot_model_connection(
    request: Request,
    body: Annotated[CopilotModelTestRequest, Body()],
    identity: Annotated[IdentityContext, Depends(get_identity)],
    authorizer: Annotated[Authorizer, Depends(get_authorizer)],
) -> CopilotModelTestResponse:
    authorizer.require(
        identity,
        Action.READ_NETWORK_ASSURANCE,
        ResourceContext(identity.tenant_id),
        request_id=getattr(request.state, "request_id", "unavailable"),
    )
    return await get_model_config_manager().test_connection(body)


def _require_read_scope(
    request: Request,
    identity: IdentityContext,
    authorizer: Authorizer,
    asset_id: str,
) -> None:
    validate_boundary_identifier(asset_id, field="asset_id")
    # 网络设备属于网络基础设施资源（Network Infrastructure Asset），而非
    # 工业生产线工单设备（Pump/Motor 等工业设备）。操作人员拥有
    # READ_NETWORK_ASSURANCE 权限即可在租户内按设备标识读取网络体检状态。
    # 避免将 resource_id 错误绑定到工业 asset_id 导致非工业工单账号因 device_scope_denied 被拒。
    authorizer.require(
        identity,
        Action.READ_NETWORK_ASSURANCE,
        ResourceContext(identity.tenant_id, resource_id=asset_id),
        request_id=getattr(request.state, "request_id", "unavailable"),
    )


def _not_found(asset_id: str) -> AppError:
    return AppError(
        status_code=404,
        code="network_asset_not_found",
        category="not_found",
        message="Network asset not found",
        details={"asset_id": asset_id},
    )


# ============================================================================
# Phase 4b: 区域无线事故智能因果诊断端点
# ============================================================================


@router.get(
    "/assurance/incidents/{incident_id}/diagnosis",
    responses=STANDARD_ERROR_RESPONSES,
    summary="Get wireless incident causal diagnosis (deterministic-first, zero model overhead)",
)
async def get_wireless_incident_diagnosis(
    request: Request,
    incident_id: str,
    identity: Annotated[IdentityContext, Depends(get_identity)],
    authorizer: Annotated[Authorizer, Depends(get_authorizer)],
    service: Annotated[Any, Depends(get_wireless_diagnosis_service)],
) -> Any:
    """只读快速获取诊断结论。

    - 命中任何记录（不管是纯确定性还是带大模型润色）直接返回；
    - 若无记录，秒级跑完确定性推理入库（llm_used=false）后返回；
    - GET 绝对不调用模型，保证零调用成本与超低时延。
    """
    authorizer.require(
        identity,
        Action.READ_NETWORK_ASSURANCE,
        ResourceContext(identity.tenant_id),
        request_id=getattr(request.state, "request_id", "unavailable"),
    )
    from industrial_ops_agent.network_assurance.wireless_diagnosis import WirelessIncidentNotFound

    try:
        res = service.get_diagnosis(identity.tenant_context, incident_id)
    except WirelessIncidentNotFound as exc:
        raise AppError(
            status_code=404,
            code="wireless_incident_not_found",
            category="not_found",
            message="Wireless site incident not found",
            details={"incident_id": incident_id},
        ) from exc
    return res.model_dump()


@router.post(
    "/assurance/incidents/{incident_id}/diagnosis",
    responses=STANDARD_ERROR_RESPONSES,
    summary="Deep diagnose wireless incident with LLM enrichment and causal guardrail",
)
async def post_wireless_incident_diagnosis(
    request: Request,
    incident_id: str,
    identity: Annotated[IdentityContext, Depends(get_identity)],
    authorizer: Annotated[Authorizer, Depends(get_authorizer)],
    service: Annotated[Any, Depends(get_wireless_diagnosis_service)],
    force: Annotated[bool, Query()] = False,
) -> Any:
    """触发深度诊断与大模型解释润色。

    - 确定性 Canonical 结论在模型调用前即确立，大模型无权改动假说；
    - 经过 W1~W6 安全护栏审查；违规即刻降级展示确定性结论；
    - GET 产出的 llm_used=false 记录绝不会阻断本接口执行 LLM enrichment。
    """
    authorizer.require(
        identity,
        Action.START_DIAGNOSIS,
        ResourceContext(identity.tenant_id, resource_id=incident_id),
        request_id=getattr(request.state, "request_id", "unavailable"),
    )
    from industrial_ops_agent.network_assurance.wireless_diagnosis import WirelessIncidentNotFound

    try:
        res = service.diagnose_incident(
            identity.tenant_context, incident_id, force=force, identity=identity
        )
    except WirelessIncidentNotFound as exc:
        raise AppError(
            status_code=404,
            code="wireless_incident_not_found",
            category="not_found",
            message="Wireless site incident not found",
            details={"incident_id": incident_id},
        ) from exc
    return res.model_dump()


# ============================================================================
# Phase 4b & 前端可视化：指定网关下的无线区域事故与外围设备列表
# ============================================================================


class SiteIncidentItem(BaseModel):
    incident_id: str
    asset_id: str
    site_id: str
    gateway_id: str
    started_at_ms: int
    last_event_ms: int
    resolved_at_ms: int | None
    affected_devices: int
    state: str
    suspected_cause: str | None
    evidence_event_ids: list[str] = Field(default_factory=list)


class WirelessDeviceBaselineItem(BaseModel):
    baseline_id: str
    asset_id: str
    site_id: str
    gateway_id: str
    device_address: str
    address_type: str
    protocol: str
    baseline_rssi_dbm: int | None
    min_seen_rssi_dbm: int | None
    max_seen_rssi_dbm: int | None
    baseline_sample_count: int
    state: str
    first_seen_ms: int | None
    last_seen_ms: int | None


class WirelessDeviceEventItem(BaseModel):
    event_id: str
    asset_id: str
    site_id: str
    gateway_id: str
    protocol: str
    device_address: str
    address_type: str
    hci_index: int
    event_type: str
    ts_ms: int
    rssi_at_event_dbm: int | None
    raw_reason_code: int
    reason: str
    source: str
    source_detail: str
    details_json: str


@router.get(
    "/assets/{asset_id}/incidents",
    response_model=list[SiteIncidentItem],
    responses=STANDARD_ERROR_RESPONSES,
    summary="List wireless site incidents recorded by this gateway asset",
)
async def list_asset_site_incidents(
    request: Request,
    asset_id: str,
    identity: Annotated[IdentityContext, Depends(get_identity)],
    authorizer: Annotated[Authorizer, Depends(get_authorizer)],
    service: Annotated[NetworkAssuranceService, Depends(get_network_assurance_service)],
) -> list[SiteIncidentItem]:
    _require_read_scope(request, identity, authorizer, asset_id)
    from sqlalchemy import select

    from industrial_ops_agent.persistence.models import NetworkSiteIncidentRecord

    with service._database.transaction(identity.tenant_context) as session:
        stmt = (
            select(NetworkSiteIncidentRecord)
            .where(
                NetworkSiteIncidentRecord.tenant_id == identity.tenant_id,
                NetworkSiteIncidentRecord.asset_id == asset_id,
            )
            .order_by(NetworkSiteIncidentRecord.started_at_ms.desc())
            .limit(100)
        )
        records = session.scalars(stmt).all()
        return [
            SiteIncidentItem(
                incident_id=r.incident_id,
                asset_id=r.asset_id,
                site_id=r.site_id,
                gateway_id=r.gateway_id,
                started_at_ms=r.started_at_ms,
                last_event_ms=r.last_event_ms,
                resolved_at_ms=r.resolved_at_ms,
                affected_devices=r.affected_devices,
                state=r.state,
                suspected_cause=r.suspected_cause,
                evidence_event_ids=list(r.evidence_event_ids_json or []),
            )
            for r in records
        ]


@router.get(
    "/assets/{asset_id}/wireless-devices",
    response_model=list[WirelessDeviceBaselineItem],
    responses=STANDARD_ERROR_RESPONSES,
    summary="List peripheral wireless devices observed by this gateway asset",
)
async def list_asset_wireless_devices(
    request: Request,
    asset_id: str,
    identity: Annotated[IdentityContext, Depends(get_identity)],
    authorizer: Annotated[Authorizer, Depends(get_authorizer)],
    service: Annotated[NetworkAssuranceService, Depends(get_network_assurance_service)],
) -> list[WirelessDeviceBaselineItem]:
    _require_read_scope(request, identity, authorizer, asset_id)
    from sqlalchemy import select

    from industrial_ops_agent.persistence.models import NetworkDeviceBaselineRecord

    with service._database.transaction(identity.tenant_context) as session:
        stmt = (
            select(NetworkDeviceBaselineRecord)
            .where(
                NetworkDeviceBaselineRecord.tenant_id == identity.tenant_id,
                NetworkDeviceBaselineRecord.asset_id == asset_id,
            )
            .order_by(NetworkDeviceBaselineRecord.last_seen_ms.desc().nullslast())
            .limit(200)
        )
        records = session.scalars(stmt).all()
        return [
            WirelessDeviceBaselineItem(
                baseline_id=r.baseline_id,
                asset_id=r.asset_id,
                site_id=r.site_id,
                gateway_id=r.gateway_id,
                device_address=r.device_address,
                address_type=r.address_type,
                protocol=r.protocol,
                baseline_rssi_dbm=r.baseline_rssi_dbm,
                min_seen_rssi_dbm=r.min_seen_rssi_dbm,
                max_seen_rssi_dbm=r.max_seen_rssi_dbm,
                baseline_sample_count=r.baseline_sample_count,
                state=r.state,
                first_seen_ms=r.first_seen_ms,
                last_seen_ms=r.last_seen_ms,
            )
            for r in records
        ]


@router.get(
    "/assets/{asset_id}/wireless-events",
    response_model=list[WirelessDeviceEventItem],
    responses=STANDARD_ERROR_RESPONSES,
    summary="List normalized wireless device events recorded by this gateway asset",
)
async def list_asset_wireless_events(
    request: Request,
    asset_id: str,
    identity: Annotated[IdentityContext, Depends(get_identity)],
    authorizer: Annotated[Authorizer, Depends(get_authorizer)],
    service: Annotated[NetworkAssuranceService, Depends(get_network_assurance_service)],
) -> list[WirelessDeviceEventItem]:
    _require_read_scope(request, identity, authorizer, asset_id)
    from sqlalchemy import select

    from industrial_ops_agent.persistence.models import NetworkWirelessEventRecord

    with service._database.transaction(identity.tenant_context) as session:
        stmt = (
            select(NetworkWirelessEventRecord)
            .where(
                NetworkWirelessEventRecord.tenant_id == identity.tenant_id,
                NetworkWirelessEventRecord.asset_id == asset_id,
            )
            .order_by(NetworkWirelessEventRecord.ts_ms.desc())
            .limit(200)
        )
        records = session.scalars(stmt).all()
        return [
            WirelessDeviceEventItem(
                event_id=r.event_id,
                asset_id=r.asset_id,
                site_id=r.site_id,
                gateway_id=r.gateway_id,
                protocol=r.protocol,
                device_address=r.device_address,
                address_type=r.address_type,
                hci_index=r.hci_index,
                event_type=r.event_type,
                ts_ms=r.ts_ms,
                rssi_at_event_dbm=r.rssi_at_event_dbm,
                raw_reason_code=r.raw_reason_code,
                reason=r.reason,
                source=r.source,
                source_detail=r.source_detail,
                details_json=r.details_json,
            )
            for r in records
        ]




@router.get(
    "/assets/{asset_id}/kernel-observations",
    responses=STANDARD_ERROR_RESPONSES,
    summary="Latest deep kernel observations (process top-N, skb drop attribution)",
)
async def get_asset_kernel_observations(
    request: Request,
    asset_id: str,
    identity: Annotated[IdentityContext, Depends(get_identity)],
    authorizer: Annotated[Authorizer, Depends(get_authorizer)],
    service: Annotated[NetworkAssuranceService, Depends(get_network_assurance_service)],
) -> dict[str, Any]:
    """最近一批上行的深度内核观测（只读）。

    返回 `{"process_top": [...], "skb_drop_hist": {...}}`，任一维度缺失时该键
    省略——与板端"没有数据就不发"的纪律一致，前端据此隐藏对应卡片而不是显示
    伪造的零值。
    """

    _require_read_scope(request, identity, authorizer, asset_id)
    return service.get_kernel_snapshot_extras(identity.tenant_context, asset_id)


# ============================================================================
# L3 Prediction: 单设备风险窗口（路线图 ③）
# ============================================================================


@router.get(
    "/assets/{asset_id}/wireless-devices/{device_address}/risk-prediction",
    response_model=PredictionResult,
    responses=STANDARD_ERROR_RESPONSES,
    summary="Predict the link-failure risk window for one wireless device (L3)",
)
async def get_device_risk_prediction(
    request: Request,
    asset_id: str,
    device_address: str,
    identity: Annotated[IdentityContext, Depends(get_identity)],
    authorizer: Annotated[Authorizer, Depends(get_authorizer)],
    risk_service: Annotated[Any, Depends(get_risk_prediction_service)],
    window_hours: Annotated[int, Query(ge=24, le=1440)] = 168,
) -> PredictionResult:
    """契约 C 的三态之一：RiskWindow（AVAILABLE/UNAVAILABLE）或 InsufficientPrediction。

    纯读路径：L3 是确定性算法，不调用任何大模型；GET 语义，不写任何表。
    """
    _require_read_scope(request, identity, authorizer, asset_id)
    return risk_service.predict_device(
        identity.tenant_context,
        asset_id,
        device_address,
        horizon_hours=window_hours,
    )


# ============================================================================
# ④ Network Operations Council —— 会商（止于 E1 ActionProposal[]）
#
# 刻意**没有**任何审批/执行端点：批准与下发属于 ⑤。
# ============================================================================


class CouncilProposalResponse(BaseModel):
    kind: str
    action_id: str | None = None
    action_params: dict[str, str] = Field(default_factory=dict)
    config_key: str | None = None
    config_value: str | None = None
    rationale: str
    proposed_preconditions: list[str] = Field(default_factory=list)
    source_bindings: list[dict[str, str]] = Field(default_factory=list)


class CouncilExpertOpinionResponse(BaseModel):
    role: str
    observations: list[str] = Field(default_factory=list)
    referenced_fact_ids: list[str] = Field(default_factory=list)
    recommendation_direction: str = ""


class NetworkCouncilResponse(BaseModel):
    council_id: str
    incident_id: str
    input_fingerprint: str
    status: str
    stage: str
    proposals: list[CouncilProposalResponse] = Field(default_factory=list)
    expert_opinions: list[CouncilExpertOpinionResponse] = Field(default_factory=list)
    failure_code: str | None = None
    requested_by_subject_id: str
    created_at: datetime
    updated_at: datetime
    version: int


def _council_payload(view: Any) -> NetworkCouncilResponse:
    return NetworkCouncilResponse(
        council_id=view.council_id,
        incident_id=view.incident_id,
        input_fingerprint=view.input_fingerprint,
        status=str(view.status),
        stage=view.stage,
        proposals=[
            CouncilProposalResponse(
                kind=str(p.kind),
                action_id=p.action_id,
                action_params=dict(p.action_params),
                config_key=p.config_key,
                config_value=p.config_value,
                rationale=p.rationale,
                proposed_preconditions=list(p.proposed_preconditions),
                source_bindings=[
                    {"kind": str(b.kind), "ref_id": b.ref_id} for b in p.source_bindings
                ],
            )
            for p in view.proposals
        ],
        expert_opinions=[
            CouncilExpertOpinionResponse(
                role=str(o.role),
                observations=list(o.observations),
                referenced_fact_ids=list(o.referenced_fact_ids),
                recommendation_direction=o.recommendation_direction,
            )
            for o in view.expert_opinions
        ],
        failure_code=view.failure_code,
        requested_by_subject_id=view.requested_by_subject_id,
        created_at=view.created_at,
        updated_at=view.updated_at,
        version=view.version,
    )


def _council_not_found(incident_id: str) -> AppError:
    return AppError(
        status_code=404,
        code="wireless_incident_not_found",
        category="not_found",
        message="Wireless site incident not found",
        details={"incident_id": incident_id},
    )


@router.get(
    "/assurance/incidents/{incident_id}/council",
    response_model=NetworkCouncilResponse,
    responses=STANDARD_ERROR_RESPONSES,
    summary="Read the latest network operations council result (advisory only)",
)
async def get_network_council(
    request: Request,
    incident_id: str,
    identity: Annotated[IdentityContext, Depends(get_identity)],
    authorizer: Annotated[Authorizer, Depends(get_authorizer)],
    council: Annotated[Any, Depends(get_network_council_service)],
) -> NetworkCouncilResponse:
    authorizer.require(
        identity,
        Action.READ_NETWORK_COUNCIL,
        ResourceContext(identity.tenant_id, resource_id=incident_id),
        request_id=getattr(request.state, "request_id", "unavailable"),
    )
    view = council.get_council(identity.tenant_context, incident_id)
    if view is None:
        raise _council_not_found(incident_id)
    return _council_payload(view)


@router.post(
    "/assurance/incidents/{incident_id}/council",
    response_model=NetworkCouncilResponse,
    responses=STANDARD_ERROR_RESPONSES,
    summary="Convene the network operations council for one incident (advisory proposals only)",
)
async def post_network_council(
    request: Request,
    incident_id: str,
    identity: Annotated[IdentityContext, Depends(get_identity)],
    authorizer: Annotated[Authorizer, Depends(get_authorizer)],
    council: Annotated[Any, Depends(get_network_council_service)],
    risk_service: Annotated[Any, Depends(get_risk_prediction_service)],
    force: Annotated[bool, Query()] = False,
) -> NetworkCouncilResponse:
    """召集会商。

    - 三专家并行 + 协调官收敛，产出 **ActionProposal[]（建议）**；
    - 相同输入指纹复用既有结果；``force=true`` 另开 attempt；
    - 任一环节失败（schema / catalog / 来源绑定 / 模型）→ 记录 FAILED 并返回 409，
      **绝不返回"半合法"建议**；
    - 本端点不产生任何审批或下发（那是 ⑤）。
    """
    authorizer.require(
        identity,
        Action.REQUEST_NETWORK_COUNCIL,
        ResourceContext(identity.tenant_id, resource_id=incident_id),
        request_id=getattr(request.state, "request_id", "unavailable"),
    )
    from industrial_ops_agent.network_assurance.council_contracts import CouncilFailure
    from industrial_ops_agent.network_assurance.network_council_service import (
        NetworkCouncilNotFound,
    )

    try:
        request_input = council.build_request_from_tables(
            identity.tenant_context, incident_id, risk_service=risk_service
        )
    except NetworkCouncilNotFound as exc:
        raise _council_not_found(incident_id) from exc

    try:
        view = council.request_council(identity.tenant_context, request_input, force=force)
    except CouncilFailure as exc:
        logging.getLogger("industrial_ops_agent.network_assurance").error(
            "COUNCIL_FAILURE: %s: %s", exc.failure_code, exc
        )
        raise AppError(
            status_code=409,
            code="network_council_failed",
            category="conflict",
            message="Network council did not produce a valid proposal set",
            details={"failure_code": exc.failure_code},
        ) from exc
    return _council_payload(view)


# ============================================================================
# ⑤ 提案审批与执行（proposal_id 寻址；policy_decision / approval_decision 分离）
# ============================================================================


class CouncilProposalStateResponse(BaseModel):
    proposal_id: str
    proposal_index: int
    proposal_snapshot: dict[str, Any]
    decision: dict[str, Any] | None
    approval: dict[str, Any] | None


class ProposalDecisionBody(BaseModel):
    decision: Literal["APPROVED", "REJECTED"]
    reason: str = Field(min_length=8, max_length=500)


@router.get(
    "/assurance/incidents/{incident_id}/council/proposals",
    response_model=list[CouncilProposalStateResponse],
    responses=STANDARD_ERROR_RESPONSES,
    summary="List council proposals with policy decisions and approval states",
)
async def list_council_proposals(
    request: Request,
    incident_id: str,
    identity: Annotated[IdentityContext, Depends(get_identity)],
    authorizer: Annotated[Authorizer, Depends(get_authorizer)],
    council: Annotated[Any, Depends(get_network_council_service)],
    approvals: Annotated[Any, Depends(get_network_action_approval_service)],
) -> list[CouncilProposalStateResponse]:
    authorizer.require(
        identity,
        Action.READ_NETWORK_COUNCIL,
        ResourceContext(identity.tenant_id, resource_id=incident_id),
        request_id=getattr(request.state, "request_id", "unavailable"),
    )
    view = council.get_council(identity.tenant_context, incident_id)
    if view is None:
        raise _council_not_found(incident_id)
    states = approvals.list_proposals(identity.tenant_context, view.council_id)
    return [
        CouncilProposalStateResponse(
            proposal_id=s.proposal_id,
            proposal_index=s.proposal_index,
            proposal_snapshot=s.proposal_snapshot,
            decision=s.decision,
            approval=s.approval,
        )
        for s in states
    ]


def _approval_error(exc: Exception) -> AppError:
    """审批域异常 → HTTP 映射（与平台 approvals 路由同一错误分类）。"""
    from industrial_ops_agent.approval.models import (
        ApprovalConflict,
        SeparationOfDutiesViolation,
    )
    from industrial_ops_agent.network_assurance.network_action_approval import (
        ApprovalNotFound,
        IdempotencyConflict,
        ManualExecutionNotAllowed,
    )

    if isinstance(exc, ApprovalNotFound):
        return AppError(
            status_code=404,
            code="proposal_not_found",
            category="not_found",
            message="Council proposal not found",
            details={"proposal_id": str(exc)},
        )
    if isinstance(exc, SeparationOfDutiesViolation):
        return AppError(
            status_code=403,
            code="separation_of_duties",
            category="forbidden",
            message="The proposer cannot approve their own proposal",
        )
    if isinstance(exc, ManualExecutionNotAllowed):
        return AppError(
            status_code=409,
            code="manual_execution_not_allowed",
            category="conflict",
            message="This proposal requires manual runbook execution, not /execute",
        )
    if isinstance(exc, IdempotencyConflict):
        return AppError(
            status_code=409,
            code="idempotency_conflict",
            category="conflict",
            message="Idempotency-Key does not match the key used for the first execution",
        )
    if isinstance(exc, ApprovalConflict):
        return AppError(
            status_code=409,
            code="approval_conflict",
            category="conflict",
            message="Approval state or version precondition failed",
            details={"reason": str(exc)},
        )
    raise exc


@router.post(
    "/assurance/proposals/{proposal_id}/decision",
    responses=STANDARD_ERROR_RESPONSES,
    summary="Record a human approval decision on one council proposal",
)
async def decide_council_proposal(
    request: Request,
    proposal_id: str,
    body: Annotated[ProposalDecisionBody, Body()],
    if_match: Annotated[str, Header(alias="If-Match")],
    identity: Annotated[IdentityContext, Depends(get_identity)],
    authorizer: Annotated[Authorizer, Depends(get_authorizer)],
    approvals: Annotated[Any, Depends(get_network_action_approval_service)],
) -> dict[str, Any]:
    """批准的是**Policy 裁决后的确定动作**（decision），不是 LLM 原文。

    - `If-Match` 为乐观并发（审批 version）；
    - SoD：发起人不能批准自己发起的会商提案（复用平台 guard）。
    """
    authorizer.require(
        identity,
        Action.APPROVE_NETWORK_ACTION,
        ResourceContext(identity.tenant_id, resource_id=proposal_id),
        request_id=getattr(request.state, "request_id", "unavailable"),
    )
    try:
        return approvals.decide(
            identity.tenant_context,
            proposal_id,
            approval_decision=body.decision,
            reason=body.reason,
            expected_version=_version(if_match),
            decider_subject_id=identity.subject_id,
        )
    except Exception as exc:  # noqa: BLE001 - 统一映射为 HTTP 语义
        raise _approval_error(exc) from exc


@router.post(
    "/assurance/proposals/{proposal_id}/execute",
    responses=STANDARD_ERROR_RESPONSES,
    summary="Execute an approved REMOTE proposal (payload comes from the approved snapshot)",
)
async def execute_council_proposal(
    request: Request,
    proposal_id: str,
    idempotency_key: Annotated[str, Header(alias="Idempotency-Key")],
    identity: Annotated[IdentityContext, Depends(get_identity)],
    authorizer: Annotated[Authorizer, Depends(get_authorizer)],
    approvals: Annotated[Any, Depends(get_network_action_approval_service)],
) -> dict[str, Any]:
    """只收 ``proposal_id + idempotency_key``——执行载荷从批准快照读取
    （P1-1，消 TOCTOU）；MANUAL 提案返回 409（P0-3，运行手册走 /runbook）。
    """
    authorizer.require(
        identity,
        Action.EXECUTE_NETWORK_ACTION,
        ResourceContext(identity.tenant_id, resource_id=proposal_id),
        request_id=getattr(request.state, "request_id", "unavailable"),
    )
    try:
        return approvals.execute(
            identity.tenant_context,
            proposal_id,
            idempotency_key=idempotency_key,
        )
    except Exception as exc:  # noqa: BLE001
        raise _approval_error(exc) from exc


@router.get(
    "/assurance/proposals/{proposal_id}/runbook",
    responses=STANDARD_ERROR_RESPONSES,
    summary="Controlled runbook for an approved ACTION_ID proposal (never executed remotely)",
)
async def council_proposal_runbook(
    request: Request,
    proposal_id: str,
    identity: Annotated[IdentityContext, Depends(get_identity)],
    authorizer: Annotated[Authorizer, Depends(get_authorizer)],
    approvals: Annotated[Any, Depends(get_network_action_approval_service)],
) -> dict[str, Any]:
    """从**批准快照**生成受控运行手册（不按当前 Catalog 重解释）。

    返回体含 ``execution_status=NOT_EXECUTED`` 与 ``manual_execution_required=true``：
    UI 永远不能把"已批准手工执行"渲染成"已执行"。
    """
    authorizer.require(
        identity,
        Action.READ_NETWORK_COUNCIL,
        ResourceContext(identity.tenant_id, resource_id=proposal_id),
        request_id=getattr(request.state, "request_id", "unavailable"),
    )
    try:
        return approvals.runbook(identity.tenant_context, proposal_id)
    except Exception as exc:  # noqa: BLE001
        raise _approval_error(exc) from exc

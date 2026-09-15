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
from typing import Annotated

from fastapi import APIRouter, Body, Depends, Header, Query, Request, Response
from pydantic import BaseModel, Field, ValidationError

from industrial_ops_agent.api.dependencies import (
    get_authorizer,
    get_edge_telemetry_verifier,
    get_identity,
    get_network_assurance_service,
    get_network_copilot_service,
)
from industrial_ops_agent.api.errors import STANDARD_ERROR_RESPONSES, AppError
from industrial_ops_agent.auth.identity import IdentityContext
from industrial_ops_agent.auth.policy import Action, Authorizer, ResourceContext
from industrial_ops_agent.network_assurance.contracts import (
    MAX_TELEMETRY_BODY_BYTES,
    NetworkActionResults,
    NetworkAssetDetail,
    NetworkAssetSummary,
    NetworkTelemetryBatch,
    NetworkTimelinePoint,
)
from industrial_ops_agent.network_assurance.copilot import NetworkCopilotService
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
    pending_actions: list[dict[str, str]] = Field(default_factory=list)


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
    *,
    key_id: Annotated[str | None, Header(alias="X-Edge-Key-Id")] = None,
    token: Annotated[str | None, Header(alias="X-Edge-Token")] = None,
    signature: Annotated[str | None, Header(alias="X-Edge-Signature")] = None,
) -> bytes:
    """Authenticate a device submission and return the bytes it signed.

    Every rejection is reported identically to the caller. The reason codes
    below appear only in server-side logs; distinguishing "unknown key id"
    from "bad signature" in a response would let an unauthenticated peer
    enumerate provisioned key ids.
    """

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
    tenant_id: Annotated[str, Header(alias="X-Edge-Tenant")],
) -> EdgeTelemetryAcceptedResponse:
    """Accept a batch, tolerating replay after a lost acknowledgement."""

    raw = await _read_verified_body(request, verifier)

    try:
        batch = NetworkTelemetryBatch.model_validate_json(raw)
    except ValidationError as exc:
        # A signature-valid but structurally invalid payload means the device
        # is running an incompatible version, which is worth failing loudly
        # rather than partially accepting.
        raise AppError(
            status_code=422,
            code="edge_payload_invalid",
            category="validation",
            message="Edge telemetry payload failed contract validation",
            details={"errors": exc.error_count()},
        ) from exc

    try:
        context = TenantContext(
            tenant_id=tenant_id,
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
    "/edge/action-results",
    status_code=204,
    responses=STANDARD_ERROR_RESPONSES,
    summary="Report outcomes of previously delivered configuration actions",
)
async def report_edge_action_results(
    request: Request,
    verifier: Annotated[EdgeTelemetryVerifier, Depends(get_edge_telemetry_verifier)],
    service: Annotated[NetworkAssuranceService, Depends(get_network_assurance_service)],
    tenant_id: Annotated[str, Header(alias="X-Edge-Tenant")],
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

    context = TenantContext(
        tenant_id=tenant_id,
        subject_id=edge_subject_id(results.device_id),
    )
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
        ResourceContext(identity.tenant_id, resource_id=asset_id, asset_id=asset_id),
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


def _require_read_scope(
    request: Request,
    identity: IdentityContext,
    authorizer: Authorizer,
    asset_id: str,
) -> None:
    validate_boundary_identifier(asset_id, field="asset_id")
    authorizer.require(
        identity,
        Action.READ_NETWORK_ASSURANCE,
        ResourceContext(identity.tenant_id, resource_id=asset_id, asset_id=asset_id),
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

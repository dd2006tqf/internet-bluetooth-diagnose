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

import base64
import binascii
import hashlib
import json
import logging
from datetime import datetime
from typing import Annotated, Any, Literal

from fastapi import APIRouter, Body, Depends, Header, Query, Request, Response
from pydantic import BaseModel, Field, ValidationError

from industrial_ops_agent.api.dependencies import (
    get_authorizer,
    get_database,
    get_edge_downlink_signer,
    get_edge_telemetry_verifier,
    get_identity,
    get_network_action_approval_service,
    get_network_assurance_service,
    get_network_copilot_service,
    get_network_council_service,
    get_object_store,
    get_risk_prediction_service,
    get_wireless_diagnosis_service,
)
from industrial_ops_agent.api.errors import STANDARD_ERROR_RESPONSES, AppError
from industrial_ops_agent.api.versions import numeric_precondition as _version
from industrial_ops_agent.auth.identity import IdentityContext
from industrial_ops_agent.auth.policy import Action, Authorizer, ResourceContext
from industrial_ops_agent.media.service import TenantObjectStore
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
from industrial_ops_agent.network_assurance.edge_action_catalog import (
    is_known_config_key,
)
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
    EdgeDownlinkActionSigner,
    EdgeTelemetryVerifier,
    NetworkSignatureError,
)
from industrial_ops_agent.network_assurance.snapshot_contracts import (
    SnapshotImage,
    SnapshotImageItem,
    SnapshotImageResult,
)
from industrial_ops_agent.network_assurance.snapshot_quota import SnapshotQuotaService
from industrial_ops_agent.network_assurance.video_contracts import (
    VideoClipChunk,
    VideoClipChunkResult,
    VideoClipItem,
    VideoClipManifest,
    VideoClipManifestResult,
)
from industrial_ops_agent.network_assurance.wireless_contracts import (
    PredictionResult,
    WirelessEventIngestResult,
    WirelessEventUplinkBatch,
)
from industrial_ops_agent.object_store.integrity import (
    DigestMismatchError,
    verify_retrieved_digest,
)
from industrial_ops_agent.object_store.keys import (
    edge_snapshot_image_object_key,
    edge_video_chunk_object_key,
    tenant_object_key_from_value,
)
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.tenant import TenantContext, validate_boundary_identifier

router = APIRouter(prefix="/network", tags=["network-assurance"])


class EdgeTelemetryAcceptedResponse(BaseModel):
    """Acknowledgement returned to the device.

    ``pending_actions`` rides on the response rather than a separate channel
    so a device behind NAT needs only outbound connectivity. The device
    applies them and reports outcomes on its next upload.

    S1#1: when a downlink signing key is provisioned the exact bytes of the
    ``pending_actions`` array carry an Ed25519 detached signature
    (``pending_actions_signature`` hex + ``pending_actions_key_id`` label).
    Unprovisioned deployments leave both ``null`` (legacy-board compatible);
    a board with ``edge.cloud_public_key_path`` set treats null as unsigned
    and rejects the whole batch fail-closed.
    """

    accepted: int
    duplicates: int
    pending_actions: list[dict[str, Any]] = Field(default_factory=list)
    pending_actions_signature: str | None = None
    pending_actions_key_id: str | None = None


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
    _downlink_signer: Annotated[
        EdgeDownlinkActionSigner | None, Depends(get_edge_downlink_signer)
    ],
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
        from hashlib import sha256

        logging.getLogger("uvicorn.error").error(
            "422 edge validation failure: %s, payload_sha256=%s bytes=%d errors=%d",
            exc.errors(),
            sha256(raw).hexdigest()[:16],
            len(raw),
            exc.error_count(),
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

    actions = list(result.pending_actions)
    signature: str | None = None
    key_id: str | None = None
    if actions and (signer := _downlink_signer) is not None:
        signature, key_id = signer.sign_actions(actions)
    return EdgeTelemetryAcceptedResponse(
        accepted=result.accepted,
        duplicates=result.duplicates,
        pending_actions=actions,
        pending_actions_signature=signature,
        pending_actions_key_id=key_id,
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
        from hashlib import sha256

        logging.getLogger("uvicorn.error").error(
            "422 edge wireless validation failure: %s, payload_sha256=%s bytes=%d errors=%d",
            exc.errors(),
            sha256(raw).hexdigest()[:16],
            len(raw),
            exc.error_count(),
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
    "/edge/video-clips",
    response_model=VideoClipManifestResult,
    responses=STANDARD_ERROR_RESPONSES,
    summary="Register a signed manifest for an edge evidence video clip",
)
async def register_edge_video_clip(
    request: Request,
    verifier: Annotated[EdgeTelemetryVerifier, Depends(get_edge_telemetry_verifier)],
    service: Annotated[NetworkAssuranceService, Depends(get_network_assurance_service)],
) -> VideoClipManifestResult:
    """登记一个待上传的留证片段清单。

    为什么清单和分片是两个端点而不是一个：片段是几十 MB 量级，而所有 edge
    端点共享 1 MB 的原始体上限。抬掉这个上限会同时抬掉**每个** edge 端点的
    内存与解析代价——一次畸形请求就能让一个 worker 占住几十 MB。清单很小
    （几百字节），分片才大，所以只有分片需要拆。

    为什么分片载荷是 base64 而不是裸二进制：``_read_verified_body`` 靠报文里
    的 ``device_id`` 把签名绑定到设备。二进制报文里没有 JSON 可解析，设备身份
    就只能改从 header 取，于是同一套 Ed25519 信任锚在两类端点上出现两种绑定
    方式。代价是 33% 体积膨胀，而留证是事件触发的偶发行为，不是持续推流。
    """

    raw = await _read_verified_body(request, verifier)

    try:
        manifest = VideoClipManifest.model_validate_json(raw)
    except ValidationError as exc:
        import logging
        from hashlib import sha256

        logging.getLogger("uvicorn.error").error(
            "422 edge video-clip manifest validation failure: %s, "
            "payload_sha256=%s bytes=%d errors=%d",
            exc.errors(),
            sha256(raw).hexdigest()[:16],
            len(raw),
            exc.error_count(),
        )
        raise AppError(
            status_code=422,
            code="edge_payload_invalid",
            category="validation",
            message="Edge video clip manifest failed contract validation",
            details={"errors": exc.error_count()},
        ) from exc

    try:
        context = TenantContext(
            tenant_id=_edge_tenant_id(request, verifier),
            subject_id=edge_subject_id(manifest.device_id),
        )
    except ValueError as exc:
        raise _bad_request("edge_tenant_invalid") from exc

    try:
        return service.register_video_clip(context, manifest, key_id=verifier.key_id)
    except NetworkAssuranceConflict as exc:
        # 三种冲突语义不同但对外同一个码：同 clip_id 异内容、并发登记、
        # 设备属于别的租户。对设备而言正确反应都是"不要重试这段"。
        raise AppError(
            status_code=409,
            code="edge_video_clip_conflict",
            category="conflict",
            message="Video clip conflicts with an existing registration",
        ) from exc


@router.post(
    "/edge/video-clips/{clip_id}/chunks/{chunk_index}",
    response_model=VideoClipChunkResult,
    responses=STANDARD_ERROR_RESPONSES,
    summary="Upload one signed chunk of an edge evidence video clip",
)
async def upload_edge_video_clip_chunk(
    request: Request,
    clip_id: str,
    chunk_index: int,
    verifier: Annotated[EdgeTelemetryVerifier, Depends(get_edge_telemetry_verifier)],
    service: Annotated[NetworkAssuranceService, Depends(get_network_assurance_service)],
    object_store: Annotated[TenantObjectStore, Depends(get_object_store)],
) -> VideoClipChunkResult:
    """接收片段中的一个分片。

    ``clip_id`` / ``chunk_index`` 出现在路径里是**冗余**的（报文里也有），
    这是刻意的：看起来更啰嗦，但让路径本身就能作为幂等键被网关/重试层使用，
    并且允许在解析 1 MB 报文之前先做一次廉价的形状校验。两者不一致直接
    拒绝——接受它就等于允许"报到 A 片段名下、实际写进 B 片段"。

    写入顺序是 **对象存储 → 数据库**，不可颠倒：先写库再写对象会得到
    「行存在但内容不存在」，那比孤儿对象糟得多——行会声称分片已送达，
    片段可能被判成 COMPLETE 而实际取不出内容。
    """

    try:
        validate_boundary_identifier(clip_id, field="clip_id")
    except ValueError as exc:
        raise _bad_request("edge_video_clip_id_invalid") from exc

    raw = await _read_verified_body(request, verifier)

    try:
        chunk = VideoClipChunk.model_validate_json(raw)
    except ValidationError as exc:
        import logging
        from hashlib import sha256

        logging.getLogger("uvicorn.error").error(
            "422 edge video-clip chunk validation failure: %s, "
            "payload_sha256=%s bytes=%d errors=%d",
            exc.errors(),
            sha256(raw).hexdigest()[:16],
            len(raw),
            exc.error_count(),
        )
        raise AppError(
            status_code=422,
            code="edge_payload_invalid",
            category="validation",
            message="Edge video clip chunk failed contract validation",
            details={"errors": exc.error_count()},
        ) from exc

    if chunk.clip_id != clip_id or chunk.chunk_index != chunk_index:
        # 路径与报文不一致说明板端拼错了 URL，而不是内容有问题。回 400
        # 而不是 409：这不是"与已有状态冲突"，重试同一个请求永远不会成功。
        raise AppError(
            status_code=400,
            code="edge_video_clip_path_mismatch",
            category="validation",
            message="Chunk path does not match the signed payload",
        )

    try:
        payload = base64.b64decode(chunk.payload_b64, validate=True)
    except (binascii.Error, ValueError) as exc:
        raise AppError(
            status_code=422,
            code="edge_video_clip_payload_not_base64",
            category="validation",
            message="Chunk payload is not valid base64",
        ) from exc

    # 顺序不可换：先解 base64、再算摘要、最后比对声明值。先比长度的话，
    # 一个只改了 padding 的报文就能让两者都"看起来对"。
    if len(payload) != chunk.size_bytes:
        raise AppError(
            status_code=422,
            code="edge_video_clip_chunk_size_mismatch",
            category="validation",
            message="Chunk payload length does not match its declared size",
            details={"declared": chunk.size_bytes, "actual": len(payload)},
        )
    if hashlib.sha256(payload).hexdigest() != chunk.content_sha256:
        raise AppError(
            status_code=422,
            code="edge_video_clip_chunk_digest_mismatch",
            category="validation",
            message="Chunk payload digest does not match its declared sha256",
        )

    try:
        context = TenantContext(
            tenant_id=_edge_tenant_id(request, verifier),
            subject_id=edge_subject_id(chunk.device_id),
        )
    except ValueError as exc:
        raise _bad_request("edge_tenant_invalid") from exc

    key = edge_video_chunk_object_key(context, chunk.clip_id, chunk.chunk_index)
    await object_store.put(context, key, payload)

    try:
        return service.store_video_clip_chunk(context, chunk, object_key=key.value)
    except NetworkAssuranceNotFound as exc:
        raise AppError(
            status_code=404,
            code="edge_video_clip_not_registered",
            category="not_found",
            message="Video clip manifest has not been registered",
        ) from exc
    except NetworkAssuranceConflict as exc:
        raise AppError(
            status_code=409,
            code="edge_video_clip_chunk_conflict",
            category="conflict",
            message="Video clip chunk conflicts with an existing upload",
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


@router.post(
    "/edge/snapshot-images",
    response_model=SnapshotImageResult,
    responses=STANDARD_ERROR_RESPONSES,
    summary="Upload one signed still image captured at an incident",
)
async def upload_edge_snapshot_image(
    request: Request,
    verifier: Annotated[EdgeTelemetryVerifier, Depends(get_edge_telemetry_verifier)],
    service: Annotated[NetworkAssuranceService, Depends(get_network_assurance_service)],
    object_store: Annotated[TenantObjectStore, Depends(get_object_store)],
    database: Annotated[Database, Depends(get_database)],
) -> SnapshotImageResult:
    """接收一张事故静帧。

    这张图不走 ``video-clips`` 那条清单 + 分片两段式，理由见
    :mod:`industrial_ops_agent.network_assurance.snapshot_contracts`：静帧是
    单张、百 KB 量级，引入分片只是把两个失败点换成一个失败点，而且
    ``fps`` / ``expected_chunks`` 这些字段在图片上没有意义——没有意义的字段
    一旦落库就会被下游当事实读。

    三重校验的顺序与分片端点完全一致，且必须在**写对象存储之前**全部通过，
    否则一个坏载荷会留下孤儿对象：

    1. base64 解码（``validate=True``，不接受缺 padding 或非法字符）；
    2. 解码后字节数 == ``size_bytes``（防 pad 截断把截短的内容说成完整）；
    3. sha256(解码后字节) == ``content_sha256``（防内容被换）。

    先解 base64、再算摘要、最后比对声明值。先比长度的话，一个只改了
    padding 的报文就能让两者都"看起来对"。
    """

    raw = await _read_verified_body(request, verifier)

    try:
        image = SnapshotImage.model_validate_json(raw)
    except ValidationError as exc:
        logging.getLogger("uvicorn.error").error(
            "422 edge snapshot image validation failure: %s, "
            "payload_sha256=%s bytes=%d errors=%d",
            exc.errors(),
            hashlib.sha256(raw).hexdigest()[:16],
            len(raw),
            exc.error_count(),
        )
        raise AppError(
            status_code=422,
            code="edge_payload_invalid",
            category="validation",
            message="Edge snapshot image failed contract validation",
            details={"errors": exc.error_count()},
        ) from exc

    try:
        payload = base64.b64decode(image.payload_b64, validate=True)
    except (binascii.Error, ValueError) as exc:
        raise AppError(
            status_code=422,
            code="edge_snapshot_image_payload_not_base64",
            category="validation",
            message="Snapshot image payload is not valid base64",
        ) from exc

    if len(payload) != image.size_bytes:
        raise AppError(
            status_code=422,
            code="edge_snapshot_image_size_mismatch",
            category="validation",
            message="Snapshot image payload length does not match its declared size",
            details={"declared": image.size_bytes, "actual": len(payload)},
        )
    if hashlib.sha256(payload).hexdigest() != image.content_sha256:
        raise AppError(
            status_code=422,
            code="edge_snapshot_image_digest_mismatch",
            category="validation",
            message="Snapshot image payload digest does not match its declared sha256",
        )

    try:
        context = TenantContext(
            tenant_id=_edge_tenant_id(request, verifier),
            subject_id=edge_subject_id(image.device_id),
        )
    except ValueError as exc:
        raise _bad_request("edge_tenant_invalid") from exc

    key = edge_snapshot_image_object_key(context, image.image_id)
    await object_store.put(context, key, payload)

    try:
        result = service.register_snapshot_image(
            context, image, object_key=key.value, key_id=verifier.key_id
        )
    except NetworkAssuranceConflict as exc:
        raise AppError(
            status_code=409,
            code="edge_snapshot_image_conflict",
            category="conflict",
            message="Snapshot image conflicts with an existing upload",
        ) from exc

    if result.accepted:
        # 配额淘汰**只在真的写入之后**跑，理由见
        # ``network_assurance/snapshot_quota.py`` 模块头：先淘汰再写，
        # 一旦写失败就是白删。重放（duplicate）不触发——没有新字节。
        await _enforce_snapshot_quota(
            request, database, context, object_store, image.image_id
        )
    return result


async def _enforce_snapshot_quota(
    request: Request,
    database: Database,
    context: TenantContext,
    object_store: TenantObjectStore,
    image_id: str,
) -> None:
    """把租户静帧总量压回配额。**失败绝不影响上传结果。**

    图已经落盘、行也登记了。这时候让请求失败，板端会重试，而重试会撞上
    ``image_id`` 冲突被拒——把「配额这一轮没算干净」放大成「这张证据永远
    存不进去」。所以这里吞掉所有异常只记日志：下一张图、下一次保留期回收
    都会接着收拾。

    配额为 ``None``（默认）时直接返回，连一次 ``SUM`` 都不做。
    """

    quota = getattr(request.app.state.settings, "network_snapshot_quota_bytes", None)
    if not quota:
        return
    try:
        plan = await SnapshotQuotaService(database).enforce(
            context,
            object_store,
            quota_bytes=quota,
            keep_image_id=image_id,
        )
    except Exception:  # noqa: BLE001 - 配额失败不能让一张已经存下的证据变成重试冲突
        logging.getLogger("uvicorn.error").exception(
            "snapshot quota enforcement failed: tenant=%s image_id=%s",
            context.tenant_id,
            image_id,
        )
        return
    if plan.evicted:
        logging.getLogger("uvicorn.error").info(
            "snapshot quota evicted oldest images: tenant=%s evicted=%d freed_bytes=%d "
            "over_quota=%s",
            context.tenant_id,
            plan.evicted,
            plan.evicted_bytes,
            plan.over_quota,
        )


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

    # S2：直连接口收紧三件套。
    # 1) 权限从 MANAGE_NETWORK_DEVICE（"看/管设备"）升为 EXECUTE_NETWORK_ACTION
    #    （与 Council /execute 同一把执行权），管理/审批角色不再能绕过审批链直发。
    authorizer.require(
        identity,
        Action.EXECUTE_NETWORK_ACTION,
        ResourceContext(identity.tenant_id, resource_id=asset_id),
        request_id=getattr(request.state, "request_id", "unavailable"),
    )
    # 2) 目录 fail-closed：云端远程可变更的配置键全集就是受审阅白名单
    #    （edge_action_catalog.CONFIG_KEYS == 板端 trialableKeyNames 镜像）。
    #    目录外的键（含 identity/enabled/active_probe.* 等"已知但非 trialable"
    #    的板端键）一律拒绝：远程变更这些键没有审批记录可挂靠，必须走 Council
    #    审批链产出 ApprovedAction，或本地 weaknet-cli 执行。
    if not is_known_config_key(body.config_key):
        raise AppError(
            status_code=409,
            code="edge_config_key_requires_approval",
            category="conflict",
            message=(
                "config_key is outside the reviewed remote-change whitelist; "
                "queue it through the council approval chain or change it locally"
            ),
        )
    # 3) 白名单（== trialable）键保留直发：板端 180s 试用看门狗 +
    #    generation 防重放仍然兜底，出错自动回滚。
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
    "/assets/{asset_id}/video-clips",
    response_model=list[VideoClipItem],
    responses=STANDARD_ERROR_RESPONSES,
    summary="List evidence video clips uploaded by this gateway asset",
)
async def list_asset_video_clips(
    request: Request,
    asset_id: str,
    identity: Annotated[IdentityContext, Depends(get_identity)],
    authorizer: Annotated[Authorizer, Depends(get_authorizer)],
    service: Annotated[NetworkAssuranceService, Depends(get_network_assurance_service)],
    incident_id: Annotated[str | None, Query(max_length=128)] = None,
    limit: Annotated[int, Query(ge=1, le=200)] = 50,
) -> list[VideoClipItem]:
    """运营端列出某网关的留证片段（只读摘要，不含载荷）。

    这是**必须存在的消费者**：没有它，上面两个 edge 端点收上来的数据在
    平台里没有任何出口，那才是真正的「孤儿表面」——收得进来、看不到。
    按 ``incident_id`` 过滤是事故详情页的主要用法（"这次事故有什么现场
    画面"），所以它在这里是一等参数而不是让调用方自己再筛一遍。
    """

    _require_read_scope(request, identity, authorizer, asset_id)
    return service.list_video_clips(
        identity.tenant_context,
        asset_id=asset_id,
        incident_id=incident_id,
        limit=limit,
    )


@router.get(
    "/assets/{asset_id}/video-clips/{clip_id}/content",
    responses={
        **STANDARD_ERROR_RESPONSES,
        200: {"content": {"video/mp4": {}, "video/h264": {}}},
    },
    summary="Download the reassembled bitstream of an evidence clip",
)
async def download_asset_video_clip(
    request: Request,
    asset_id: str,
    clip_id: str,
    identity: Annotated[IdentityContext, Depends(get_identity)],
    authorizer: Annotated[Authorizer, Depends(get_authorizer)],
    service: Annotated[NetworkAssuranceService, Depends(get_network_assurance_service)],
    object_store: Annotated[TenantObjectStore, Depends(get_object_store)],
) -> Response:
    """把分片按索引拼回完整码流交给运营端（MP4 或裸 Annex-B H.264）。

    为什么两种格式都要支持，而不是「统一转成 MP4 再给」：板端封装是**尽力**
    行为（缺 ffmpeg 或码流不合规时改传裸码流），而云端历史片段全是裸码流。
    强行在这里统一意味着云端要装 ffmpeg 并重封装——那是一台 API 服务器上
    不该有的 CPU 与依赖负担，而且对已存在的历史证据无效。所以云端只做
    一件事：**如实告诉浏览器这段字节是什么**。

    判据是字节本身（``ftyp`` box 落在 ``[4, 8)``），不是清单里的字段：
    容器类型是码流自身的事实，再在清单里声明一份就等于允许两者不一致，
    而不一致时以谁为准没有答案。

    为什么不是 ``StreamingResponse`` 逐块转出：对象存储的 ``get`` 是
    **整块读**，逐块 await 也只是一次性把 640 KiB 读进内存再写出，省不下
    什么。真正的流式需要存储层支持 range 读取，而那属于另一个改造。
    当前上限 256 MB，一次性拼装在可接受范围内，且实现简单到不会错。

    未收齐时返回 409 而不是返回半个文件：半段码流解不出画面，交给播放器
    只会得到\"文件损坏\"这种无法归因的错误。运营端应该看到\"还在传\"。
    """

    _require_read_scope(request, identity, authorizer, asset_id)
    context = identity.tenant_context
    clips = service.list_video_clips(context, asset_id=asset_id, limit=200)
    clip = next((c for c in clips if c.clip_id == clip_id), None)
    if clip is None:
        raise AppError(
            status_code=404,
            code="network_video_clip_not_found",
            category="not_found",
            message="Video clip not found for this asset",
            details={"clip_id": clip_id},
        )
    if clip.received_chunks < clip.expected_chunks:
        raise AppError(
            status_code=409,
            code="network_video_clip_incomplete",
            category="conflict",
            message="Video clip has not finished uploading",
            details={
                "received_chunks": clip.received_chunks,
                "expected_chunks": clip.expected_chunks,
            },
        )

    rows = service.list_video_clip_chunks(context, clip_id)
    if [r.chunk_index for r in rows] != list(range(clip.expected_chunks)):
        # 数量对得上但索引有洞：那是服务端的账算错了，不是设备的问题。
        # 明确报出来，而不是悄悄拼出顺序错乱的码流。
        raise AppError(
            status_code=500,
            code="network_video_clip_gap",
            category="dependency",
            message="Video clip chunk indices are not contiguous",
            retryable=True,
        )

    body = bytearray()
    for row in rows:
        key = tenant_object_key_from_value(context, row.object_key)
        content = await object_store.get(context, key)
        if content is None:
            # 行说已送达、对象却不在：这是真正的数据丢失，不能当成 404
            # 悄悄返回——运营端看到 404 会以为片段不存在，而实际上证据丢了。
            raise AppError(
                status_code=500,
                code="network_video_clip_object_missing",
                category="dependency",
                message="A stored chunk is missing from object storage",
                retryable=True,
                details={"chunk_index": row.chunk_index},
            )
        body.extend(content)

    # 容器类型按字节判，不看清单字段。判据与板端 `looksLikeMp4()` 同形：
    # ISO/IEC 14496-12 的 `ftyp` box 类型落在 [4, 8)，而 Annex-B 的起始码
    # 在最前 4 字节内，两者不可能混淆。
    #
    # 为什么这里必须判对：`media_type` 直接决定浏览器走哪条播放路径。
    # 把 MP4 标成 `video/h264` 时浏览器会下载而不是播放（`<video>` 不支持
    # 该 type）；把裸码流标成 `video/mp4` 时浏览器会尝试解析容器并失败，
    # 报「无法播放此文件」——两种错法都表现为"留证播不了"，而证据本身是好的。
    payload = bytes(body)
    is_mp4 = len(payload) >= 12 and payload[4:8] == b"ftyp"
    suffix = "mp4" if is_mp4 else "h264"

    return Response(
        content=payload,
        media_type="video/mp4" if is_mp4 else "video/h264",
        headers={
            "Cache-Control": "private, no-store",
            "Content-Disposition": f'inline; filename="{clip_id}.{suffix}"',
            "X-Content-Type-Options": "nosniff",
        },
    )


@router.get(
    "/assets/{asset_id}/snapshot-images",
    response_model=list[SnapshotImageItem],
    responses=STANDARD_ERROR_RESPONSES,
    summary="List incident snapshot images uploaded by this gateway asset",
)
async def list_asset_snapshot_images(
    request: Request,
    asset_id: str,
    identity: Annotated[IdentityContext, Depends(get_identity)],
    authorizer: Annotated[Authorizer, Depends(get_authorizer)],
    service: Annotated[NetworkAssuranceService, Depends(get_network_assurance_service)],
    incident_id: Annotated[str | None, Query(max_length=128)] = None,
    limit: Annotated[int, Query(ge=1, le=200)] = 50,
) -> list[SnapshotImageItem]:
    """运营端列出某网关的事故静帧（只读摘要，不含载荷）。

    这是 ``POST /edge/snapshot-images`` 的**消费者**。没有它，收上来的图在
    平台里没有任何出口——那才是真正的孤儿表面：收得进来、看不到。

    按 ``incident_id`` 过滤是事故详情页的主要用法（"这次事故的现场画面
    是什么"），所以它是一等参数而不是让调用方自己再筛一遍。
    """

    _require_read_scope(request, identity, authorizer, asset_id)
    return service.list_snapshot_images(
        identity.tenant_context,
        asset_id=asset_id,
        incident_id=incident_id,
        limit=limit,
    )


@router.get(
    "/assets/{asset_id}/snapshot-images/{image_id}/content",
    responses={
        **STANDARD_ERROR_RESPONSES,
        200: {"content": {"image/jpeg": {}}},
    },
    summary="Download the JPEG bytes of an incident snapshot image",
)
async def download_asset_snapshot_image(
    request: Request,
    asset_id: str,
    image_id: str,
    identity: Annotated[IdentityContext, Depends(get_identity)],
    authorizer: Annotated[Authorizer, Depends(get_authorizer)],
    service: Annotated[NetworkAssuranceService, Depends(get_network_assurance_service)],
    object_store: Annotated[TenantObjectStore, Depends(get_object_store)],
) -> Response:
    """把一张静帧的原始 JPEG 字节交给运营端。

    与片段下载三点不同，每一点都是「一张图」与「一段码流」的差别：

    1. 没有「未收齐」这个状态，所以没有 409。行存在 = 内容已落对象存储
       （写入顺序在路由层强制）。真取不到对象是**已登记的证据丢了**，报 500。
    2. 媒体类型固定 ``image/jpeg``，不需要按字节猜容器。契约层只接受
       ``codec == "JPEG"``，而字节在入库前已经解 base64 + 验长度 + 验摘要。
    3. **下载时重算摘要并与行里的 ``content_sha256`` 比对**。这是整条留证
       链路的最后一步，也是「证据可长期取回」唯一能拿出证据的地方。取回
       成功但摘要不符 != 文件不存在：前者要立刻查是谁改的，后者只需再等。
       混淆两者会让篡改看起来像延迟。

    为什么在下载路径里做校验而不是后台任务：证据被读取的那一刻是最需要
    确认它没变形的时刻。后台巡检能让问题更早暴露，但替代不了这一刻的判据。
    """

    _require_read_scope(request, identity, authorizer, asset_id)
    context = identity.tenant_context
    try:
        record = service.get_snapshot_image(context, image_id)
    except NetworkAssuranceNotFound as exc:
        # 与 ``download_asset_video_clip`` 不同，那里靠「列表里找不到」报 404，
        # 这里直接抛出再转。为什么不先在列表里找：静帧列表按 ``captured_at_ms``
        # 排序且 limit 封顶，用列表做存在性判断会让「图很多时找不到最老那张」
        # 变成一次假的 404。
        raise AppError(
            status_code=404,
            code="network_snapshot_image_not_found",
            category="not_found",
            message="Snapshot image not found for this asset",
            details={"image_id": image_id},
        ) from exc
    if record.asset_id != asset_id:
        # 图存在但属于别的网关：对这条 URL 而言就是不存在。用 404 而不是
        # 403——403 会确认「这张图存在」，让 URL 可枚举。
        raise AppError(
            status_code=404,
            code="network_snapshot_image_not_found",
            category="not_found",
            message="Snapshot image not found for this asset",
            details={"image_id": image_id},
        )

    key = tenant_object_key_from_value(context, record.object_key)
    payload = await object_store.get(context, key)

    if payload is None:
        # 行说已送达、对象却不在：这是真正的数据丢失，不能当成 404 悄悄
        # 返回——运营端看到 404 会以为图不存在，而实际上证据丢了。
        raise AppError(
            status_code=500,
            code="network_snapshot_image_object_missing",
            category="dependency",
            message="A stored snapshot image is missing from object storage",
            retryable=True,
            details={"image_id": image_id},
        )

    # 长度先判再算摘要：长度不符是比摘要更便宜、也更确定的一条判据。
    if len(payload) != record.size_bytes:
        raise AppError(
            status_code=500,
            code="network_snapshot_image_size_mismatch",
            category="dependency",
            message="Retrieved snapshot image length does not match the registered size",
            details={"expected_bytes": record.size_bytes, "actual_bytes": len(payload)},
        )

    try:
        verify_retrieved_digest(payload, record.content_sha256)
    except DigestMismatchError as exc:
        raise AppError(
            status_code=500,
            code="network_snapshot_image_digest_mismatch",
            category="dependency",
            message="Retrieved snapshot image does not match its registered digest",
            details={"image_id": image_id, "detail": str(exc)},
        ) from exc

    return Response(
        content=payload,
        media_type="image/jpeg",
        headers={
            # 证据图片不可变：摘要即身份，同一 URL 的字节永不变。
            # 仍标 private（租户隔离），但允许客户端缓存——重复查看同一
            # 事故现场不该反复下载。
            "Cache-Control": "private, max-age=3600",
            "Content-Disposition": f'inline; filename="{image_id}.jpg"',
            "X-Content-Type-Options": "nosniff",
        },
    )


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


class CouncilContributionResponse(BaseModel):
    """单个专家的原始贡献行（network_council_contributions 读视图）。"""

    contribution_id: str
    council_id: str
    attempt_number: int
    agent_role: str
    input_digest: str
    output_digest: str
    model_release_id: str | None = None
    prompt_bundle_hash: str | None = None
    output: dict[str, Any] = Field(default_factory=dict)
    completed_at: datetime


class NetworkCouncilResponse(BaseModel):
    council_id: str
    incident_id: str
    input_fingerprint: str
    status: str
    stage: str
    proposals: list[CouncilProposalResponse] = Field(default_factory=list)
    expert_opinions: list[CouncilExpertOpinionResponse] = Field(default_factory=list)
    contributions: list[CouncilContributionResponse] = Field(default_factory=list)
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
        contributions=[
            CouncilContributionResponse(
                contribution_id=c.contribution_id,
                council_id=c.council_id,
                attempt_number=c.attempt_number,
                agent_role=c.agent_role,
                input_digest=c.input_digest,
                output_digest=c.output_digest,
                model_release_id=c.model_release_id,
                prompt_bundle_hash=c.prompt_bundle_hash,
                output=dict(c.output),
                completed_at=c.completed_at,
            )
            for c in view.contributions
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

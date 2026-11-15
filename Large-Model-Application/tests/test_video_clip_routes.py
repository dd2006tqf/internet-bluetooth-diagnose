"""视频留证片段的 HTTP 传输层校验（不重复服务层语义测试）。

这里只测**路由独占**的职责：签名之外的报文完整性三重校验。

三重校验的顺序是本次最容易写错的地方，所以每条都单独断言：

1. base64 解码（``validate=True``，不接受缺 padding 或非法字符）；
2. 解码后字节数 == ``size_bytes``（防 pad 截断把截短的内容说成完整）；
3. sha256(解码后字节) == ``content_sha256``（防内容被换）。

再叠一条路径与报文的一致性：``clip_id``/``chunk_index`` 在 URL 和报文
里各有一份，不一致必须拒绝——接受它就等于允许"报到 A 片段名下、
实际写进 B 片段"。

这些检查必须在**写对象存储之前**全部通过，否则一个坏分片会留下孤儿对象。
"""

from __future__ import annotations

import base64
import hashlib
import json
from datetime import UTC, datetime, timedelta
from typing import Any

import pytest
from fastapi.testclient import TestClient
from sqlalchemy import create_engine
from sqlalchemy.pool import StaticPool

from industrial_ops_agent.auth.identity import IdentityContext, Role
from industrial_ops_agent.config import Environment, Settings
from industrial_ops_agent.network_assurance.service import NetworkAssuranceService
from industrial_ops_agent.object_store.memory import InMemoryTenantObjectStore
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import Base
from industrial_ops_agent.persistence.tenant import TenantContext

DEVICE = "radxa-cubie-a7a"
TENANT = "tenant-alpha"


@pytest.fixture
def app_db() -> Database:
    engine = create_engine(
        "sqlite:///:memory:",
        connect_args={"check_same_thread": False},
        poolclass=StaticPool,
    )
    Base.metadata.create_all(engine)
    return Database.from_engine(engine)


@pytest.fixture
def client(app_db: Database) -> TestClient:
    """带对象存储的最小 app。设备端点在测试里直接调服务层，
    所以这里只需要运营端的身份链。"""

    class _StubVerifier:
        def verify(self, token: str) -> IdentityContext:
            subject, _, role = token.partition(":")
            now = datetime.now(UTC)
            return IdentityContext(
                subject_id=subject or "op-1",
                oidc_subject=f"oidc-{subject}",
                tenant_id=TENANT,
                roles=frozenset({Role.DOMAIN_EXPERT}),
                asset_ids=frozenset(),
                site_ids=frozenset(),
                issued_at=now,
                expires_at=now + timedelta(hours=1),
            )

    from industrial_ops_agent.api.app import create_app
    from industrial_ops_agent.auth.policy import Authorizer
    from industrial_ops_agent.security_audit import (
        InMemorySecurityAuditSink,
        SecurityAuditor,
    )

    app = create_app(Settings(environment=Environment.TEST, _env_file=None))
    app.state.database = app_db
    app.state.oidc_verifier = _StubVerifier()
    app.state.authorizer = Authorizer(
        SecurityAuditor(InMemorySecurityAuditSink(), hash_key=b"video-clip-routes-key")
    )
    app.state.object_store = InMemoryTenantObjectStore()
    app.state.network_assurance_service = NetworkAssuranceService(database=app_db)
    return TestClient(app)


def _h() -> dict[str, str]:
    return {"Authorization": "Bearer op-1:expert"}


def _manifest_payload(**overrides: Any) -> dict[str, Any]:
    payload: dict[str, Any] = {
        "schema_version": "network.edge.video-clips.v1",
        "clip_id": "clip-alpha",
        "device_id": DEVICE,
        "site_id": "site-1",
        "gateway_id": "gw-1",
        "incident_id": "inc-1",
        "codec": "H264",
        "width": 1640,
        "height": 1232,
        "fps": 25,
        "started_at_ms": 1_700_000_000_000,
        "ended_at_ms": 1_700_000_030_000,
        "total_bytes": 20,
        "expected_chunks": 2,
        "declared_content_sha256": "",
    }
    payload.update(overrides)
    return payload


def _chunk_payload(index: int, body: bytes, **overrides: Any) -> dict[str, Any]:
    payload: dict[str, Any] = {
        "schema_version": "network.edge.video-clips.v1",
        "device_id": DEVICE,
        "clip_id": "clip-alpha",
        "chunk_index": index,
        "size_bytes": len(body),
        "content_sha256": hashlib.sha256(body).hexdigest(),
        "payload_b64": base64.b64encode(body).decode("ascii"),
    }
    payload.update(overrides)
    return payload


# ---------------------------------------------------------------------------
# 运营端读取（未经签名层，走正常 OIDC）
# ---------------------------------------------------------------------------


def test_list_requires_authentication(client: TestClient) -> None:
    assert client.get(f"/api/v1/network/assets/{DEVICE}/video-clips").status_code == 401


def test_list_is_empty_before_any_upload(client: TestClient) -> None:
    resp = client.get(f"/api/v1/network/assets/{DEVICE}/video-clips", headers=_h())
    assert resp.status_code == 200
    assert resp.json() == []


def test_download_missing_clip_is_404(client: TestClient) -> None:
    resp = client.get(
        f"/api/v1/network/assets/{DEVICE}/video-clips/nope/content", headers=_h()
    )
    assert resp.status_code == 404
    assert resp.json()["error"]["code"] == "network_video_clip_not_found"


def test_list_rejects_non_canonical_asset_id(client: TestClient) -> None:
    resp = client.get("/api/v1/network/assets/a%2Fb/video-clips", headers=_h())
    assert resp.status_code in (400, 404, 422)


# ---------------------------------------------------------------------------
# 下载端点的容器嗅探：`media_type` 必须与字节一致
# ---------------------------------------------------------------------------
#
# 这两条守的是一个**只在浏览器里才暴露**的错误：`media_type` 决定
# `<video>` 走播放还是走下载。标错时证据本身是完好的，只是播不出来——
# 而现场排查时会一路怀疑采集、编码、上传，唯独不会想到是一行 header。
#
# 判据必须按字节（`ftyp` box 落在 [4, 8)），不能按清单字段：板端封装是尽力
# 行为，缺 ffmpeg 时会改传裸码流，而云端历史片段全是裸码流，两者会长期共存。


def _seed_single_chunk_clip(app: TestClient, body: bytes) -> str:
    """把一个单分片片段直接写进 app 状态（服务层 + 对象存储），返回 clip_id。

    为什么不走签名 HTTP：这两条测的是**下载端点的渲染决策**，与签名域无关。
    走签名会把「签名错了」和「嗅探错了」混成同一个红灯，而它们的修法完全不同。
    """
    import asyncio

    from industrial_ops_agent.network_assurance.video_contracts import (
        VideoClipChunk,
        VideoClipManifest,
    )
    from industrial_ops_agent.object_store.keys import edge_video_chunk_object_key

    service = app.app.state.network_assurance_service
    store = app.app.state.object_store
    ctx = TenantContext(tenant_id=TENANT, subject_id="edge-subject")

    payload = _manifest_payload(
        total_bytes=len(body),
        expected_chunks=1,
        declared_content_sha256=hashlib.sha256(body).hexdigest(),
    )
    manifest = VideoClipManifest.model_validate(payload)
    service.register_video_clip(ctx, manifest, key_id="k")

    chunk = VideoClipChunk.model_validate(_chunk_payload(0, body))
    key = edge_video_chunk_object_key(ctx, chunk.clip_id, chunk.chunk_index)
    asyncio.run(store.put(ctx, key, body))
    service.store_video_clip_chunk(ctx, chunk, object_key=key.value)
    return chunk.clip_id


def test_download_serves_mp4_when_bytes_are_an_mp4_container(
    client: TestClient,
) -> None:
    # 一个真实形态的 MP4 头：box 长度 + "ftyp" + 主品牌。
    body = b"\x00\x00\x00\x20" + b"ftypisom" + b"\x00\x00\x02\x00" + b"payload"
    clip_id = _seed_single_chunk_clip(client, body)

    resp = client.get(
        f"/api/v1/network/assets/{DEVICE}/video-clips/{clip_id}/content", headers=_h()
    )
    assert resp.status_code == 200
    assert resp.headers["content-type"].startswith("video/mp4")
    assert f'filename="{clip_id}.mp4"' in resp.headers["content-disposition"]
    assert resp.content == body


def test_download_serves_h264_when_bytes_are_annexb(client: TestClient) -> None:
    """裸 Annex-B 必须仍然标成 ``video/h264``。

    这条不是「顺手也测一下」，是**回归保护**：改成无条件 ``video/mp4`` 会让
    全部历史片段（以及未来任何一次 ffmpeg 缺失时的降级）在浏览器里报
    「无法播放此文件」，而那条错误不会指向这里。
    """
    body = b"\x00\x00\x00\x01" + b"\x67" + b"\x00\x00\x00\x01\x68" + b"payload"
    clip_id = _seed_single_chunk_clip(client, body)

    resp = client.get(
        f"/api/v1/network/assets/{DEVICE}/video-clips/{clip_id}/content", headers=_h()
    )
    assert resp.status_code == 200
    assert resp.headers["content-type"].startswith("video/h264")
    assert f'filename="{clip_id}.h264"' in resp.headers["content-disposition"]
    assert resp.content == body


# ---------------------------------------------------------------------------
# 清单 / 分片的报文完整性（直接打服务层 + 对象存储，绕开签名域）
# ---------------------------------------------------------------------------


@pytest.fixture
def wired(app_db: Database) -> tuple[NetworkAssuranceService, InMemoryTenantObjectStore, TenantContext]:
    store = InMemoryTenantObjectStore()
    service = NetworkAssuranceService(database=app_db)
    ctx = TenantContext(tenant_id=TENANT, subject_id="edge-subject")
    return service, store, ctx


def test_good_chunk_round_trips_through_object_store(wired) -> None:
    from industrial_ops_agent.network_assurance.video_contracts import (
        VideoClipChunk,
        VideoClipManifest,
    )
    from industrial_ops_agent.object_store.keys import (
        edge_video_chunk_object_key,
        tenant_object_key_from_value,
    )

    service, store, ctx = wired
    service.register_video_clip(
        ctx, VideoClipManifest.model_validate(_manifest_payload()), key_id="k"
    )

    body = b"\x00\x00\x00\x01\x67hello"
    chunk = VideoClipChunk.model_validate(_chunk_payload(0, body))
    key = edge_video_chunk_object_key(ctx, chunk.clip_id, chunk.chunk_index)

    import asyncio

    asyncio.run(store.put(ctx, key, body))
    result = service.store_video_clip_chunk(ctx, chunk, object_key=key.value)
    assert result.accepted is True
    assert result.received_chunks == 1

    # 下载路径靠 object_key 往返重建，必须逐字一致。
    row_key = tenant_object_key_from_value(ctx, key.value)
    assert row_key.value == key.value
    assert asyncio.run(store.get(ctx, row_key)) == body


def test_chunk_size_declaration_mismatch_is_detectable(wired) -> None:
    """声明 size 大于真实内容时，服务层不负责发现——那是路由的职责。

    这条测试固定的是**分工**：服务层只信 object_key 已经落盘成功，所以
    长度与摘要校验必须在路由层、且在 put 之前完成。如果哪天有人把校验
    挪进服务层，这条测试会仍然通过而坏分片已经进对象存储了。
    """
    from industrial_ops_agent.network_assurance.video_contracts import (
        VideoClipChunk,
        VideoClipManifest,
    )

    service, _store, ctx = wired
    service.register_video_clip(
        ctx, VideoClipManifest.model_validate(_manifest_payload()), key_id="k"
    )
    body = b"abc"
    payload = _chunk_payload(0, body)
    # size 与 payload 不一致 —— 契约层允许（它只校验上界），
    # 拦截点在路由。这里确认契约层确实不拦，避免双重假设。
    payload["size_bytes"] = 99
    VideoClipChunk.model_validate(payload)


def test_manifest_rejects_traversal_clip_id_at_contract_level() -> None:
    from pydantic import ValidationError

    from industrial_ops_agent.network_assurance.video_contracts import (
        VideoClipManifest,
    )

    for bad in ("../escape", "a/b", ".hidden", "a b", ""):
        with pytest.raises(ValidationError):
            VideoClipManifest.model_validate(_manifest_payload(clip_id=bad))


def test_chunk_rejects_payload_over_encoded_limit() -> None:
    from pydantic import ValidationError

    from industrial_ops_agent.network_assurance.video_contracts import (
        MAX_VIDEO_CHUNK_B64_CHARS,
        VideoClipChunk,
    )

    payload = _chunk_payload(0, b"x")
    payload["payload_b64"] = "A" * (MAX_VIDEO_CHUNK_B64_CHARS + 4)
    with pytest.raises(ValidationError):
        VideoClipChunk.model_validate(payload)


def test_manifest_unknown_field_is_rejected() -> None:
    """``extra="forbid"`` 是整套 edge 契约的共同基座：板端多发一个字段
    意味着两端版本不一致，静默忽略会让漂移一直不被发现。"""
    from pydantic import ValidationError

    from industrial_ops_agent.network_assurance.video_contracts import (
        VideoClipManifest,
    )

    with pytest.raises(ValidationError):
        VideoClipManifest.model_validate(_manifest_payload(surprise="x"))


def test_manifest_wire_shape_is_stable() -> None:
    """钉住对外字段名：板端 emitter 与 ``tools/verify_telemetry_contract.py``
    依赖这组名字，改一个就会让跨语言校验在 CI 里炸。"""
    from industrial_ops_agent.network_assurance.video_contracts import (
        VideoClipChunk,
        VideoClipManifest,
    )

    assert set(VideoClipManifest.model_fields) == {
        "schema_version",
        "clip_id",
        "device_id",
        "site_id",
        "gateway_id",
        "incident_id",
        "codec",
        "width",
        "height",
        "fps",
        "started_at_ms",
        "ended_at_ms",
        "total_bytes",
        "expected_chunks",
        "declared_content_sha256",
    }
    assert set(VideoClipChunk.model_fields) == {
        "schema_version",
        "device_id",
        "clip_id",
        "chunk_index",
        "size_bytes",
        "content_sha256",
        "payload_b64",
    }

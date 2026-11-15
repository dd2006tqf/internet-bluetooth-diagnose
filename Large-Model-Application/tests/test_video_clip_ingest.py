"""视频留证片段上行的服务层与契约测试。

覆盖的都是**静默错误**的边界，而不是"能不能存进去"：

1. 清单重放计 duplicate、不重复插行；
2. 同 clip_id 异内容拒绝（证据不可变，不能让到达顺序决定内容）；
3. 分片必须先有清单（分片先到等于让服务端猜归属）；
4. 同索引同内容计 duplicate、同索引异内容拒绝；
5. 分片索引越界拒绝（不能收一个声明 3 片的片段的第 7 片）；
6. 跨租户同 clip_id 按既有规则判冲突；
7. 收齐判定：``complete`` 只在最后一片到达时为真；
8. 运营端查询的 ``state`` 与 ``received_chunks`` 自洽。

HTTP 层的验签域由 ``test_edge_tenant_binding.py`` 等既有测试覆盖，这里
不重复造签名；路由层的 base64/长度/摘要三重校验在
``test_video_clip_routes.py`` 里单独测。
"""

from __future__ import annotations

from datetime import UTC, datetime

import pytest
from sqlalchemy import create_engine, select
from sqlalchemy.pool import StaticPool

from industrial_ops_agent.network_assurance.service import (
    NetworkAssuranceConflict,
    NetworkAssuranceNotFound,
    NetworkAssuranceService,
)
from industrial_ops_agent.network_assurance.video_contracts import (
    MAX_VIDEO_CLIP_DURATION_MS,
    VideoClipChunk,
    VideoClipManifest,
)
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import (
    Base,
    NetworkVideoClipChunkRecord,
    NetworkVideoClipRecord,
)
from industrial_ops_agent.persistence.tenant import TenantContext

DEVICE = "radxa-cubie-a7a"
KEY_ID = "weaknet-edge-telemetry-v1"
TENANT = "tenant-alpha"
NOW = datetime(2026, 11, 14, 12, 0, tzinfo=UTC)


@pytest.fixture
def test_db() -> Database:
    engine = create_engine(
        "sqlite:///:memory:",
        connect_args={"check_same_thread": False},
        poolclass=StaticPool,
    )
    Base.metadata.create_all(engine)
    return Database.from_engine(engine)


@pytest.fixture
def service(test_db: Database) -> NetworkAssuranceService:
    return NetworkAssuranceService(database=test_db)


@pytest.fixture
def ctx() -> TenantContext:
    return TenantContext(tenant_id=TENANT, subject_id="edge-subject")


def _manifest(
    *,
    clip_id: str = "clip-001",
    device_id: str = DEVICE,
    expected_chunks: int = 2,
    total_bytes: int = 20,
    incident_id: str = "",
    started_at_ms: int = 1_700_000_000_000,
    ended_at_ms: int = 1_700_000_030_000,
    declared_sha: str = "",
) -> VideoClipManifest:
    return VideoClipManifest(
        clip_id=clip_id,
        device_id=device_id,
        site_id="site-1",
        gateway_id="gw-1",
        incident_id=incident_id,
        width=1640,
        height=1232,
        fps=25,
        started_at_ms=started_at_ms,
        ended_at_ms=ended_at_ms,
        total_bytes=total_bytes,
        expected_chunks=expected_chunks,
        declared_content_sha256=declared_sha,
    )


def _chunk(
    index: int,
    *,
    clip_id: str = "clip-001",
    sha: str,
    size: int = 10,
) -> VideoClipChunk:
    return VideoClipChunk(
        device_id=DEVICE,
        clip_id=clip_id,
        chunk_index=index,
        size_bytes=size,
        content_sha256=sha,
        payload_b64="AAAAAAAAAAAAAA==",
    )


SHA0 = "0" * 64
SHA1 = "1" * 64
SHA2 = "2" * 64


# ---------------------------------------------------------------------------
# 清单
# ---------------------------------------------------------------------------


def test_manifest_first_registration_accepts(
    service: NetworkAssuranceService, ctx: TenantContext
) -> None:
    result = service.register_video_clip(ctx, _manifest(), key_id=KEY_ID, received_at=NOW)
    assert result.accepted is True
    assert result.duplicate is False
    assert result.received_chunks == 0
    # 声明了 2 片但一片没到 → 还不完整
    assert result.complete is False


def test_manifest_replay_is_duplicate_and_does_not_duplicate_rows(
    service: NetworkAssuranceService, ctx: TenantContext, test_db: Database
) -> None:
    service.register_video_clip(ctx, _manifest(), key_id=KEY_ID, received_at=NOW)
    again = service.register_video_clip(ctx, _manifest(), key_id=KEY_ID, received_at=NOW)
    assert again.accepted is False
    assert again.duplicate is True
    with test_db.transaction(ctx) as session:
        rows = session.scalars(select(NetworkVideoClipRecord)).all()
    assert len(rows) == 1


def test_manifest_same_id_different_content_is_rejected(
    service: NetworkAssuranceService, ctx: TenantContext
) -> None:
    service.register_video_clip(ctx, _manifest(), key_id=KEY_ID, received_at=NOW)
    # 同一个 clip_id，但时长变了 —— 这是两段不同的视频，不是重放。
    with pytest.raises(NetworkAssuranceConflict):
        service.register_video_clip(
            ctx,
            _manifest(ended_at_ms=1_700_000_099_000),
            key_id=KEY_ID,
            received_at=NOW,
        )


def test_manifest_zero_chunks_is_complete_immediately(
    service: NetworkAssuranceService, ctx: TenantContext
) -> None:
    # 空片段（0 片 0 字节）是合法声明，且一登记就完整 —— 它表示"触发时
    # 编码器还来不及出帧"，是一个需要被看见的事实，不是要拒绝的错误。
    result = service.register_video_clip(
        ctx,
        _manifest(expected_chunks=0, total_bytes=0),
        key_id=KEY_ID,
        received_at=NOW,
    )
    assert result.accepted is True
    assert result.complete is True


# ---------------------------------------------------------------------------
# 分片
# ---------------------------------------------------------------------------


def test_chunk_before_manifest_is_not_found(
    service: NetworkAssuranceService, ctx: TenantContext
) -> None:
    with pytest.raises(NetworkAssuranceNotFound):
        service.store_video_clip_chunk(
            ctx, _chunk(0, sha=SHA0), object_key="k", received_at=NOW
        )


def test_chunk_beyond_declared_count_is_rejected(
    service: NetworkAssuranceService, ctx: TenantContext
) -> None:
    service.register_video_clip(
        ctx, _manifest(expected_chunks=2), key_id=KEY_ID, received_at=NOW
    )
    # 声明 2 片（索引 0/1），第 2 片越界。
    with pytest.raises(NetworkAssuranceConflict):
        service.store_video_clip_chunk(
            ctx, _chunk(2, sha=SHA2), object_key="k2", received_at=NOW
        )


def test_chunk_first_then_replay_is_duplicate(
    service: NetworkAssuranceService, ctx: TenantContext
) -> None:
    service.register_video_clip(
        ctx, _manifest(expected_chunks=2), key_id=KEY_ID, received_at=NOW
    )
    first = service.store_video_clip_chunk(
        ctx, _chunk(0, sha=SHA0), object_key="k0", received_at=NOW
    )
    assert first.accepted is True and first.duplicate is False
    assert first.received_chunks == 1
    assert first.complete is False

    replay = service.store_video_clip_chunk(
        ctx, _chunk(0, sha=SHA0), object_key="k0", received_at=NOW
    )
    assert replay.accepted is False and replay.duplicate is True
    assert replay.received_chunks == 1


def test_chunk_same_index_different_content_is_rejected(
    service: NetworkAssuranceService, ctx: TenantContext
) -> None:
    service.register_video_clip(
        ctx, _manifest(expected_chunks=2), key_id=KEY_ID, received_at=NOW
    )
    service.store_video_clip_chunk(
        ctx, _chunk(0, sha=SHA0), object_key="k0", received_at=NOW
    )
    # 同索引但摘要不同：板端编号错乱。接受它就等于让证据悄悄拼错。
    with pytest.raises(NetworkAssuranceConflict):
        service.store_video_clip_chunk(
            ctx, _chunk(0, sha=SHA1), object_key="k0b", received_at=NOW
        )


def test_complete_only_when_last_chunk_arrives(
    service: NetworkAssuranceService, ctx: TenantContext
) -> None:
    service.register_video_clip(
        ctx, _manifest(expected_chunks=2), key_id=KEY_ID, received_at=NOW
    )
    a = service.store_video_clip_chunk(
        ctx, _chunk(0, sha=SHA0), object_key="k0", received_at=NOW
    )
    b = service.store_video_clip_chunk(
        ctx, _chunk(1, sha=SHA1), object_key="k1", received_at=NOW
    )
    assert a.complete is False
    assert b.complete is True
    assert b.received_chunks == 2


def test_chunks_out_of_order_still_complete(
    service: NetworkAssuranceService, ctx: TenantContext
) -> None:
    # 分片不保证按序到达（重试、并发上传）。收齐判定必须是"数量到了"，
    # 不是"最后一片的索引等于 expected-1"。
    service.register_video_clip(
        ctx, _manifest(expected_chunks=2), key_id=KEY_ID, received_at=NOW
    )
    first = service.store_video_clip_chunk(
        ctx, _chunk(1, sha=SHA1), object_key="k1", received_at=NOW
    )
    assert first.complete is False
    second = service.store_video_clip_chunk(
        ctx, _chunk(0, sha=SHA0), object_key="k0", received_at=NOW
    )
    assert second.complete is True


# ---------------------------------------------------------------------------
# 跨租户
# ---------------------------------------------------------------------------


def test_cross_tenant_same_clip_id_conflicts(
    service: NetworkAssuranceService, ctx: TenantContext
) -> None:
    service.register_video_clip(ctx, _manifest(), key_id=KEY_ID, received_at=NOW)
    other = TenantContext(tenant_id="tenant-beta", subject_id="edge-other")
    # 板端 clip_id 按时间+序号生成，两个租户撞号完全可能。既有规则是
    # 判冲突（_require_same_tenant），不是悄悄覆盖或分租户各存一份。
    with pytest.raises(NetworkAssuranceConflict):
        service.register_video_clip(
            other, _manifest(), key_id="other-key", received_at=NOW
        )


# ---------------------------------------------------------------------------
# 运营端查询
# ---------------------------------------------------------------------------


def test_list_reports_pending_then_complete(
    service: NetworkAssuranceService, ctx: TenantContext
) -> None:
    service.register_video_clip(
        ctx,
        _manifest(expected_chunks=2, incident_id="inc-1"),
        key_id=KEY_ID,
        received_at=NOW,
    )
    pending = service.list_video_clips(ctx, asset_id=DEVICE)
    assert len(pending) == 1
    assert pending[0].state == "PENDING"
    assert pending[0].received_chunks == 0

    service.store_video_clip_chunk(
        ctx, _chunk(0, sha=SHA0), object_key="k0", received_at=NOW
    )
    service.store_video_clip_chunk(
        ctx, _chunk(1, sha=SHA1), object_key="k1", received_at=NOW
    )
    done = service.list_video_clips(ctx, asset_id=DEVICE)
    assert done[0].state == "COMPLETE"
    assert done[0].received_chunks == 2


def test_list_filters_by_incident(
    service: NetworkAssuranceService, ctx: TenantContext
) -> None:
    service.register_video_clip(
        ctx, _manifest(clip_id="c-a", incident_id="inc-1"), key_id=KEY_ID, received_at=NOW
    )
    service.register_video_clip(
        ctx, _manifest(clip_id="c-b", incident_id="inc-2"), key_id=KEY_ID, received_at=NOW
    )
    scoped = service.list_video_clips(ctx, asset_id=DEVICE, incident_id="inc-1")
    assert [c.clip_id for c in scoped] == ["c-a"]


def test_list_orders_newest_first(
    service: NetworkAssuranceService, ctx: TenantContext
) -> None:
    service.register_video_clip(
        ctx,
        _manifest(clip_id="old", started_at_ms=1_000, ended_at_ms=2_000),
        key_id=KEY_ID,
        received_at=NOW,
    )
    service.register_video_clip(
        ctx,
        _manifest(clip_id="new", started_at_ms=9_000, ended_at_ms=9_500),
        key_id=KEY_ID,
        received_at=NOW,
    )
    assert [c.clip_id for c in service.list_video_clips(ctx, asset_id=DEVICE)] == [
        "new",
        "old",
    ]


def test_list_chunks_returns_index_order(
    service: NetworkAssuranceService, ctx: TenantContext
) -> None:
    service.register_video_clip(
        ctx, _manifest(expected_chunks=3), key_id=KEY_ID, received_at=NOW
    )
    # 乱序写入
    for index, sha in ((2, SHA2), (0, SHA0), (1, SHA1)):
        service.store_video_clip_chunk(
            ctx, _chunk(index, sha=sha), object_key=f"k{index}", received_at=NOW
        )
    rows = service.list_video_clip_chunks(ctx, "clip-001")
    assert [r.chunk_index for r in rows] == [0, 1, 2]
    assert [r.object_key for r in rows] == ["k0", "k1", "k2"]


# ---------------------------------------------------------------------------
# 契约边界
# ---------------------------------------------------------------------------


def test_manifest_rejects_duration_beyond_window() -> None:
    with pytest.raises(Exception):
        VideoClipManifest(
            clip_id="c",
            device_id=DEVICE,
            width=1640,
            height=1232,
            started_at_ms=0,
            ended_at_ms=MAX_VIDEO_CLIP_DURATION_MS + 1,
            total_bytes=0,
            expected_chunks=0,
        )


def test_manifest_rejects_non_canonical_clip_id() -> None:
    # 这个值会成为对象键的一段；在契约层拦，板端拿到的是明确的 422。
    for bad in ("../escape", "a/b", "with space", ""):
        with pytest.raises(Exception):
            VideoClipManifest(
                clip_id=bad,
                device_id=DEVICE,
                width=16,
                height=16,
                started_at_ms=0,
                ended_at_ms=1,
                total_bytes=0,
                expected_chunks=0,
            )


def test_manifest_rejects_chunkless_clip_with_bytes() -> None:
    with pytest.raises(Exception):
        VideoClipManifest(
            clip_id="c",
            device_id=DEVICE,
            width=16,
            height=16,
            started_at_ms=0,
            ended_at_ms=1,
            total_bytes=100,
            expected_chunks=0,
        )


def test_chunk_record_allows_duplicate_index_only_in_one_tenant(
    service: NetworkAssuranceService, ctx: TenantContext, test_db: Database
) -> None:
    service.register_video_clip(
        ctx, _manifest(expected_chunks=1), key_id=KEY_ID, received_at=NOW
    )
    service.store_video_clip_chunk(
        ctx, _chunk(0, sha=SHA0), object_key="k0", received_at=NOW
    )
    with test_db.transaction(ctx) as session:
        rows = session.scalars(select(NetworkVideoClipChunkRecord)).all()
    assert len(rows) == 1
    assert rows[0].chunk_index == 0

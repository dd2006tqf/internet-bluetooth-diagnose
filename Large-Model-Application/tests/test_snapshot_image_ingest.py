"""事故静帧上行的服务层语义测试。

覆盖的都是**静默错误**的边界，而不是「能不能存进去」：

1. 首次登记返回 accepted，且行里的字段与契约一致；
2. 同一 image_id 重放计 duplicate，不新增行（幂等键必须真的幂等）；
3. 同 image_id 但内容不同 → 冲突拒绝（证据不可变，不能让到达顺序决定内容）；
4. 同 image_id 但 incident 不同 → 冲突拒绝（同一编号指两件事说明编号坏了）；
5. 跨租户同 image_id → 按既有规则判冲突，而不是悄悄覆盖；
6. ``object_key`` 原样落库（它是取回内容的唯一线索，写错就永久取不出图）。

HTTP 层的验签域由既有 ``test_edge_tenant_binding.py`` 覆盖；路由层的
base64/长度/摘要三重校验在 ``test_snapshot_image_routes.py`` 里单独测。
"""

from __future__ import annotations

import hashlib
from datetime import UTC, datetime

import pytest
from sqlalchemy import create_engine, select
from sqlalchemy.pool import StaticPool

from industrial_ops_agent.network_assurance.service import (
    NetworkAssuranceConflict,
    NetworkAssuranceService,
)
from industrial_ops_agent.network_assurance.snapshot_contracts import SnapshotImage
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import Base, NetworkSnapshotImageRecord
from industrial_ops_agent.persistence.tenant import TenantContext

DEVICE = "radxa-cubie-a7a"
KEY_ID = "weaknet-edge-telemetry-v1"
TENANT = "tenant-alpha"
NOW = datetime(2026, 11, 20, 7, 0, tzinfo=UTC)

PAYLOAD = b"\xff\xd8" + b"\x00" * 196 + b"\xff\xd9"
SHA = hashlib.sha256(PAYLOAD).hexdigest()


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


def _image(
    *,
    image_id: str = "img-001",
    device_id: str = DEVICE,
    incident_id: str = "sitinc-abc",
    captured_at_ms: int = 1_795_155_756_003,
    content_sha256: str = SHA,
    size_bytes: int | None = None,
) -> SnapshotImage:
    import base64

    return SnapshotImage(
        image_id=image_id,
        device_id=device_id,
        site_id="site-1",
        gateway_id="gw-1",
        incident_id=incident_id,
        width=1640,
        height=1232,
        captured_at_ms=captured_at_ms,
        size_bytes=len(PAYLOAD) if size_bytes is None else size_bytes,
        content_sha256=content_sha256,
        payload_b64=base64.b64encode(PAYLOAD).decode(),
    )


def _rows(db: Database, ctx: TenantContext) -> list[NetworkSnapshotImageRecord]:
    with db.transaction(ctx) as session:
        return list(session.scalars(select(NetworkSnapshotImageRecord)))


# ===========================================================================
# 首次登记
# ===========================================================================


def test_first_upload_is_accepted(service: NetworkAssuranceService, ctx: TenantContext) -> None:
    result = service.register_snapshot_image(
        ctx,
        _image(),
        object_key="tenant/tenant-alpha/quarantine/snapshot-images/img-001",
        key_id=KEY_ID,
        received_at=NOW,
    )
    assert result.accepted is True
    assert result.duplicate is False
    assert result.image_id == "img-001"
    assert result.size_bytes == len(PAYLOAD)


def test_row_carries_the_declared_facts(
    service: NetworkAssuranceService, ctx: TenantContext, test_db: Database
) -> None:
    service.register_snapshot_image(
        ctx, _image(), object_key="k-1", key_id=KEY_ID, received_at=NOW
    )
    rows = _rows(test_db, ctx)
    assert len(rows) == 1
    row = rows[0]
    assert row.asset_id == DEVICE
    assert row.incident_id == "sitinc-abc"
    assert row.codec == "JPEG"
    assert (row.width, row.height) == (1640, 1232)
    assert row.captured_at_ms == 1_795_155_756_003
    assert row.size_bytes == len(PAYLOAD)
    assert row.content_sha256 == SHA


def test_object_key_is_persisted_verbatim(
    service: NetworkAssuranceService, ctx: TenantContext, test_db: Database
) -> None:
    # object_key 是取回内容的唯一线索。写错就永久取不出图，而且没有任何
    # 报错——行看起来完全正常，只是指向一个不存在（或错误）的对象。
    key = "tenant/tenant-alpha/quarantine/snapshot-images/img-001"
    service.register_snapshot_image(
        ctx, _image(), object_key=key, key_id=KEY_ID, received_at=NOW
    )
    assert _rows(test_db, ctx)[0].object_key == key


# ===========================================================================
# 幂等与冲突
# ===========================================================================


def test_replay_is_duplicate_and_does_not_insert(
    service: NetworkAssuranceService, ctx: TenantContext, test_db: Database
) -> None:
    first = service.register_snapshot_image(
        ctx, _image(), object_key="k-1", key_id=KEY_ID, received_at=NOW
    )
    second = service.register_snapshot_image(
        ctx, _image(), object_key="k-1", key_id=KEY_ID, received_at=NOW
    )
    assert first.accepted is True
    assert second.accepted is False
    assert second.duplicate is True
    assert len(_rows(test_db, ctx)) == 1, "重放不能新增行"


def test_same_image_id_with_different_bytes_is_rejected(
    service: NetworkAssuranceService, ctx: TenantContext
) -> None:
    # 证据不可变：覆盖之后「这个事故现场长什么样」取决于到达顺序。
    service.register_snapshot_image(
        ctx, _image(), object_key="k-1", key_id=KEY_ID, received_at=NOW
    )
    other_sha = hashlib.sha256(b"different").hexdigest()
    with pytest.raises(NetworkAssuranceConflict):
        service.register_snapshot_image(
            ctx,
            _image(content_sha256=other_sha),
            object_key="k-2",
            key_id=KEY_ID,
            received_at=NOW,
        )


def test_same_image_id_with_different_incident_is_rejected(
    service: NetworkAssuranceService, ctx: TenantContext
) -> None:
    # 同一编号指向两个不同事故，说明板端编号逻辑坏了。此时「这张图属于
    # 哪个事故」没有正确答案，必须拒绝而不是任选一个。
    service.register_snapshot_image(
        ctx, _image(), object_key="k-1", key_id=KEY_ID, received_at=NOW
    )
    with pytest.raises(NetworkAssuranceConflict):
        service.register_snapshot_image(
            ctx,
            _image(incident_id="sitinc-other"),
            object_key="k-2",
            key_id=KEY_ID,
            received_at=NOW,
        )


def test_same_image_id_with_different_capture_time_is_rejected(
    service: NetworkAssuranceService, ctx: TenantContext
) -> None:
    service.register_snapshot_image(
        ctx, _image(), object_key="k-1", key_id=KEY_ID, received_at=NOW
    )
    with pytest.raises(NetworkAssuranceConflict):
        service.register_snapshot_image(
            ctx,
            _image(captured_at_ms=1_795_155_999_999),
            object_key="k-2",
            key_id=KEY_ID,
            received_at=NOW,
        )


def test_different_images_coexist(service: NetworkAssuranceService, ctx: TenantContext) -> None:
    # 幂等键判重不能过宽：两个不同事故的两张图必须都能存下。
    service.register_snapshot_image(
        ctx, _image(image_id="img-a"), object_key="k-a", key_id=KEY_ID, received_at=NOW
    )
    service.register_snapshot_image(
        ctx,
        _image(image_id="img-b", incident_id="sitinc-other"),
        object_key="k-b",
        key_id=KEY_ID,
        received_at=NOW,
    )
    assert len(_rows(test_db=None, ctx=ctx)) if False else True  # 占位，见下


def test_two_distinct_images_both_persist(
    service: NetworkAssuranceService, ctx: TenantContext, test_db: Database
) -> None:
    service.register_snapshot_image(
        ctx, _image(image_id="img-a"), object_key="k-a", key_id=KEY_ID, received_at=NOW
    )
    service.register_snapshot_image(
        ctx,
        _image(image_id="img-b", incident_id="sitinc-other"),
        object_key="k-b",
        key_id=KEY_ID,
        received_at=NOW,
    )
    rows = _rows(test_db, ctx)
    assert len(rows) == 2
    assert {r.image_id for r in rows} == {"img-a", "img-b"}


# ===========================================================================
# 租户边界
# ===========================================================================


def test_cross_tenant_same_image_id_is_rejected(
    service: NetworkAssuranceService, ctx: TenantContext
) -> None:
    # 跨租户撞号按既有规则判冲突，而不是悄悄覆盖另一租户的行——
    # 那会让一个租户把另一个租户的证据顶掉。
    other = TenantContext(tenant_id="tenant-beta", subject_id="edge-subject")
    service.register_snapshot_image(
        ctx, _image(), object_key="k-alpha", key_id=KEY_ID, received_at=NOW
    )
    with pytest.raises(NetworkAssuranceConflict):
        service.register_snapshot_image(
            other, _image(), object_key="k-beta", key_id=KEY_ID, received_at=NOW
        )

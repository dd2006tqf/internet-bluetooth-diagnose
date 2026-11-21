"""运营端静帧取回端点的行为测试。

补的是**留证链路的最后一段**：图上传进对象存储、行落库之后，运营端
能不能把它取回来，以及取回来的是不是当初那一份。

为什么这段必须单独测
====================

上传端点的测试（``test_snapshot_image_routes.py``）只证明「拒绝得对」，
服务层测试（``test_snapshot_image_ingest.py``）只证明「行记得对」。
两者都没有、也无法覆盖这件事：**把对象读回来，重算 sha256，与库里的
``content_sha256`` 比对**。

这条闭环是「静帧证据可长期取回」这句话唯一能拿出证据的地方。没有它，
整条链路只能证明「云端收到了一个摘要」，不能证明「取回来的还是那张图」。

这里刻意用**真落盘**对象存储（``LocalDiskTenantObjectStore``）而不是
内存实现：内存实现下对象永不写入失败、重启即丢，「取回 → 重算 → 比对」
虽然能跑通，但它证明的是字典取值的正确性，不是留证链路的正确性。
"""

from __future__ import annotations

import base64
import hashlib
from datetime import UTC, datetime, timedelta
from pathlib import Path

import pytest
from fastapi.testclient import TestClient
from sqlalchemy import create_engine
from sqlalchemy.pool import StaticPool

from industrial_ops_agent.auth.identity import IdentityContext, Role
from industrial_ops_agent.config import Environment, Settings
from industrial_ops_agent.network_assurance.service import NetworkAssuranceService
from industrial_ops_agent.network_assurance.snapshot_contracts import (
    SNAPSHOT_UPLINK_SCHEMA_VERSION,
    SnapshotImage,
)
from industrial_ops_agent.object_store.keys import edge_snapshot_image_object_key
from industrial_ops_agent.object_store.local_disk import LocalDiskTenantObjectStore
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import Base
from industrial_ops_agent.persistence.tenant import TenantContext

TENANT = "tenant-alpha"
DEVICE = "radxa-cubie-a7a"


def _jpeg(size: int = 400) -> bytes:
    """形状像 JPEG 的字节（首尾标记在位，便于断言逐字返回）。"""
    body = bytes(range(48)) * (max(size - 4, 0) // 48 + 1)
    body = body[: max(size - 4, 0)]
    return b"\xff\xd8" + body + b"\xff\xd9"


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
def store(tmp_path: Path) -> LocalDiskTenantObjectStore:
    return LocalDiskTenantObjectStore(tmp_path / "objects")


@pytest.fixture
def client(app_db: Database, store: LocalDiskTenantObjectStore) -> TestClient:
    class _StubVerifier:
        def verify(self, token: str) -> IdentityContext:
            subject, _, _role = token.partition(":")
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
        SecurityAuditor(InMemorySecurityAuditSink(), hash_key=b"snapshot-route-key")
    )
    app.state.object_store = store
    app.state.network_assurance_service = NetworkAssuranceService(database=app_db)
    return TestClient(app)


def _h() -> dict[str, str]:
    return {"Authorization": "Bearer op-1:expert"}


def _seed(
    client: TestClient,
    *,
    image_id: str = "snap-op-1",
    payload: bytes | None = None,
    incident_id: str = "sitinc-abc",
    captured_at_ms: int = 1_795_155_756_003,
    device_id: str = DEVICE,
) -> bytes:
    """把一张图真的写进对象存储、再登记到库里，返回图片字节。

    走的是与路由**相同的顺序**（对象存储 → 数据库）。这个顺序不是巧合：
    先写库会让本测试能通过、而生产上的「取不到对象」分支永远测不到。
    """

    import asyncio

    body = payload if payload is not None else _jpeg()
    ctx = TenantContext(tenant_id=TENANT, subject_id="edge-subject")
    image = SnapshotImage.model_validate(
        {
            "schema_version": SNAPSHOT_UPLINK_SCHEMA_VERSION,
            "image_id": image_id,
            "device_id": device_id,
            "site_id": "site-1",
            "gateway_id": "gw-1",
            "incident_id": incident_id,
            "codec": "JPEG",
            "width": 1640,
            "height": 1232,
            "captured_at_ms": captured_at_ms,
            "size_bytes": len(body),
            "content_sha256": hashlib.sha256(body).hexdigest(),
            "payload_b64": base64.b64encode(body).decode(),
        }
    )

    store = client.app.state.object_store
    key = edge_snapshot_image_object_key(ctx, image_id)
    asyncio.run(store.put(ctx, key, body))

    service = client.app.state.network_assurance_service
    service.register_snapshot_image(ctx, image, object_key=key.value, key_id="k")
    return body


# ===========================================================================
# 列表：必须存在，否则收上来的图在平台里没有出口
# ===========================================================================


def test_list_returns_registered_images(client: TestClient) -> None:
    _seed(client, image_id="snap-a")
    _seed(client, image_id="snap-b")

    resp = client.get(f"/api/v1/network/assets/{DEVICE}/snapshot-images", headers=_h())
    assert resp.status_code == 200
    rows = resp.json()
    assert {r["image_id"] for r in rows} == {"snap-a", "snap-b"}
    row = next(r for r in rows if r["image_id"] == "snap-a")
    assert row["codec"] == "JPEG"
    assert row["width"] == 1640 and row["height"] == 1232
    assert row["size_bytes"] == len(_jpeg())
    # 摘要必须在摘要里，否则运营端无法自己核验
    assert row["content_sha256"] == hashlib.sha256(_jpeg()).hexdigest()
    # object_key 不该流到响应里：它是内部寻址细节，暴露它等于把存储布局
    # 变成对外契约。
    assert "object_key" not in row


def test_list_filters_by_incident(client: TestClient) -> None:
    _seed(client, image_id="snap-inc-1", incident_id="sitinc-1")
    _seed(client, image_id="snap-inc-2", incident_id="sitinc-2")

    resp = client.get(
        f"/api/v1/network/assets/{DEVICE}/snapshot-images",
        params={"incident_id": "sitinc-1"},
        headers=_h(),
    )
    assert resp.status_code == 200
    assert [r["image_id"] for r in resp.json()] == ["snap-inc-1"]


def test_list_orders_newest_capture_first(client: TestClient) -> None:
    # 事故详情页默认看到的应该是现场**最近**的一刻。
    _seed(client, image_id="snap-old", captured_at_ms=1_000)
    _seed(client, image_id="snap-new", captured_at_ms=2_000)

    resp = client.get(f"/api/v1/network/assets/{DEVICE}/snapshot-images", headers=_h())
    assert [r["image_id"] for r in resp.json()] == ["snap-new", "snap-old"]


def test_list_empty_for_unknown_asset(client: TestClient) -> None:
    _seed(client)
    resp = client.get("/api/v1/network/assets/no-such-asset/snapshot-images", headers=_h())
    assert resp.status_code == 200
    assert resp.json() == []


def test_list_respects_limit(client: TestClient) -> None:
    for i in range(5):
        _seed(client, image_id=f"snap-{i}", captured_at_ms=1_000 + i)

    resp = client.get(
        f"/api/v1/network/assets/{DEVICE}/snapshot-images",
        params={"limit": 2},
        headers=_h(),
    )
    assert resp.status_code == 200
    assert len(resp.json()) == 2


# ===========================================================================
# 下载：整条留证链路的最后一步
# ===========================================================================


def test_download_returns_the_exact_bytes(client: TestClient) -> None:
    body = _seed(client, image_id="snap-dl")

    resp = client.get(
        f"/api/v1/network/assets/{DEVICE}/snapshot-images/snap-dl/content", headers=_h()
    )
    assert resp.status_code == 200
    # 逐字返回，不是「长度对」也不是「摘要对」——是字节本身。
    assert resp.content == body
    assert resp.headers["content-type"] == "image/jpeg"
    assert 'filename="snap-dl.jpg"' in resp.headers["content-disposition"]
    assert resp.headers["x-content-type-options"] == "nosniff"


def test_download_round_trips_through_disk_and_verifies_digest(
    client: TestClient, store: LocalDiskTenantObjectStore
) -> None:
    """这条就是「静帧证据可长期取回」那个缺口的补丁。

    写进去 → 从**磁盘**取回来 → 重算 sha256 → 与库里存的比对。
    对象真的在文件系统上，不在内存字典里，所以这条断言跨越了进程重启
    会保留的那部分状态。
    """

    body = _seed(client, image_id="snap-verify")
    declared = hashlib.sha256(body).hexdigest()

    # 直接读磁盘，确认对象真的落地了（而不是只有行）。
    on_disk = (
        Path(store._root) / "tenant" / TENANT / "quarantine" / "snapshot-images" / ("snap-verify")
    )
    assert on_disk.is_file()
    assert hashlib.sha256(on_disk.read_bytes()).hexdigest() == declared

    resp = client.get(
        f"/api/v1/network/assets/{DEVICE}/snapshot-images/snap-verify/content",
        headers=_h(),
    )
    assert resp.status_code == 200
    assert hashlib.sha256(resp.content).hexdigest() == declared


def test_download_404_for_unknown_image(client: TestClient) -> None:
    resp = client.get(
        f"/api/v1/network/assets/{DEVICE}/snapshot-images/snap-nope/content", headers=_h()
    )
    assert resp.status_code == 404
    assert resp.json()["error"]["code"] == "network_snapshot_image_not_found"


def test_download_404_when_image_belongs_to_another_asset(client: TestClient) -> None:
    """图存在但属于别的网关：对这条 URL 而言就是不存在。

    用 404 而不是 403，因为 403 会确认「这张图存在」——把 URL 变成可枚举的。
    但也不能用「按 asset_id 过滤后查不到」来实现：那会让一条本该报
    数据归属问题的路径退化成普通的 not found，掩盖真正的接线错误。
    """
    _seed(client, image_id="snap-other-asset", device_id="another-gateway")

    resp = client.get(
        f"/api/v1/network/assets/{DEVICE}/snapshot-images/snap-other-asset/content",
        headers=_h(),
    )
    assert resp.status_code == 404
    assert resp.json()["error"]["code"] == "network_snapshot_image_not_found"


def test_download_500_when_object_vanished_from_store(
    client: TestClient, store: LocalDiskTenantObjectStore
) -> None:
    """行说已送达、对象却不在 —— 这是数据丢失，不是 404。

    报 404 会让运营端以为「这张图从来没有过」，而实际上它存在过、丢了。
    两者的处置完全相反：前者什么都不用做，后者要立刻查存储。
    """

    import asyncio

    _seed(client, image_id="snap-lost")
    ctx = TenantContext(tenant_id=TENANT, subject_id="edge-subject")
    key = edge_snapshot_image_object_key(ctx, "snap-lost")
    asyncio.run(store.delete(ctx, key))

    resp = client.get(
        f"/api/v1/network/assets/{DEVICE}/snapshot-images/snap-lost/content", headers=_h()
    )
    assert resp.status_code == 500
    assert resp.json()["error"]["code"] == "network_snapshot_image_object_missing"


def test_download_500_when_stored_object_was_tampered(
    client: TestClient, store: LocalDiskTenantObjectStore
) -> None:
    """内容被换掉但行还是旧摘要 —— 必须报摘要不符，不能静默返回。

    静默返回比报错糟得多：运营端会拿着被篡改（或损坏）的图当证据。
    """

    _seed(client, image_id="snap-tampered", payload=b"\xff\xd8" + b"real" * 100 + b"\xff\xd9")

    target = (
        Path(store._root) / "tenant" / TENANT / "quarantine" / "snapshot-images" / "snap-tampered"
    )
    # 保持长度不变，只改内容：这样「长度校验」过不了关，必须靠摘要兜住。
    original = target.read_bytes()
    target.write_bytes(original[:2] + b"EVIL" + original[6:])

    resp = client.get(
        f"/api/v1/network/assets/{DEVICE}/snapshot-images/snap-tampered/content",
        headers=_h(),
    )
    assert resp.status_code == 500
    assert resp.json()["error"]["code"] == "network_snapshot_image_digest_mismatch"


def test_download_500_when_length_changed(
    client: TestClient, store: LocalDiskTenantObjectStore
) -> None:
    """长度先判：截断的图不该走到摘要比对就被拦下。

    长度不符比摘要不符更便宜、也更确定——它不需要读完整个对象。
    """

    _seed(client, image_id="snap-truncated")
    target = (
        Path(store._root) / "tenant" / TENANT / "quarantine" / "snapshot-images" / "snap-truncated"
    )
    target.write_bytes(target.read_bytes()[:-10])

    resp = client.get(
        f"/api/v1/network/assets/{DEVICE}/snapshot-images/snap-truncated/content",
        headers=_h(),
    )
    assert resp.status_code == 500
    assert resp.json()["error"]["code"] == "network_snapshot_image_size_mismatch"


def test_download_requires_authentication(client: TestClient) -> None:
    _seed(client, image_id="snap-auth")
    resp = client.get(f"/api/v1/network/assets/{DEVICE}/snapshot-images/snap-auth/content")
    assert resp.status_code == 401


def test_list_requires_authentication(client: TestClient) -> None:
    resp = client.get(f"/api/v1/network/assets/{DEVICE}/snapshot-images")
    assert resp.status_code == 401


def test_object_key_round_trips_through_tenant_object_key_from_value(
    client: TestClient,
) -> None:
    """下载路径靠 ``object_key`` 往返重建。这条钉的是那个往返必须逐字一致。

    如果哪天键的构造与解析漂移了（多一段、少一段、顺序变了），
    下载会静默 404 —— 因为重建出来的键指向一个不存在的对象。
    """

    from industrial_ops_agent.object_store.keys import tenant_object_key_from_value

    _seed(client, image_id="snap-key")
    ctx = TenantContext(tenant_id=TENANT, subject_id="edge-subject")
    key = edge_snapshot_image_object_key(ctx, "snap-key")

    rebuilt = tenant_object_key_from_value(ctx, key.value)
    assert rebuilt.value == key.value
    assert rebuilt.zone is key.zone

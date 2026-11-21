"""配额在 HTTP 层的接线：什么时候该动、什么时候**一次都不该动**。

服务层语义（删谁、删多少、行与对象是否一致）由 ``test_snapshot_quota.py``
覆盖。这里只测路由**独占**的职责，也就是只有接进上传端点才会出错的部分：

| 接线如果错了 | 表面现象 | 真实后果 |
| --- | --- | --- |
| 配额为 ``None`` 时仍然查库 | 上传正常 | 默认部署每次上传多一次全表 ``SUM``，没人会想到是这里 |
| 重放（duplicate）也触发淘汰 | 上传正常 | 一次网络重试删掉一批老证据，而没有任何新字节入库 |
| 写对象失败后仍跑淘汰 | 上传 500 | 没存下新图，却把旧的删了——净损失 |
| 淘汰抛异常时上传也失败 | 偶发 500 | 图已落盘、行已登记，板端重试撞 409：这张证据永远存不进 |

四条的共同点是**都不会报错**，只会让磁盘慢慢满或者让证据悄悄消失。
"""

from __future__ import annotations

import base64
import hashlib
import json
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any
from unittest import mock

import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from fastapi.testclient import TestClient
from sqlalchemy import create_engine, select
from sqlalchemy.pool import StaticPool

from industrial_ops_agent.auth.identity import IdentityContext, Role
from industrial_ops_agent.config import Environment, Settings
from industrial_ops_agent.network_assurance.service import NetworkAssuranceService
from industrial_ops_agent.network_assurance.signing import EdgeTelemetryVerifier
from industrial_ops_agent.network_assurance.snapshot_contracts import (
    SNAPSHOT_UPLINK_SCHEMA_VERSION,
)
from industrial_ops_agent.object_store.local_disk import LocalDiskTenantObjectStore
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import (
    Base,
    NetworkSnapshotImageRecord,
)
from industrial_ops_agent.persistence.tenant import TenantContext

TENANT = "tenant-alpha"
DEVICE = "radxa-cubie-a7a"
KEY_ID = "weaknet-edge-telemetry-v1"
DEVICE_TOKEN = "rehearsal-device-token"

TENANT_CTX = TenantContext(tenant_id=TENANT, subject_id="edge-subject")


def _jpeg(size: int = 400) -> bytes:
    body = bytes(range(48)) * (max(size - 4, 0) // 48 + 1)
    body = body[: max(size - 4, 0)]
    return b"\xff\xd8" + body + b"\xff\xd9"


@pytest.fixture
def signing_key() -> Ed25519PrivateKey:
    return Ed25519PrivateKey.generate()


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
def client(
    app_db: Database,
    store: LocalDiskTenantObjectStore,
    signing_key: Ed25519PrivateKey,
) -> TestClient:
    public_key = signing_key.public_key().public_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PublicFormat.SubjectPublicKeyInfo,
    )
    verifier = EdgeTelemetryVerifier.from_settings(
        public_key_pem=public_key.decode(),
        key_id=KEY_ID,
        device_token=DEVICE_TOKEN,
        tenant_id=TENANT,
    )

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
        SecurityAuditor(InMemorySecurityAuditSink(), hash_key=b"snapshot-quota-key")
    )
    app.state.object_store = store
    app.state.edge_telemetry_verifier = verifier
    app.state.network_assurance_service = NetworkAssuranceService(database=app_db)
    return TestClient(app, raise_server_exceptions=False)


def _set_quota(client: TestClient, quota: int | None) -> None:
    """改已装配好的 settings，而不是重建 app。

    ``Settings`` 是 pydantic 模型，``model_copy`` 保留其余全部字段——
    手工拼一个新的 ``Settings(...)`` 会让本用例在「新增一个必填设置项」时
    以与配额无关的理由变红。
    """

    client.app.state.settings = client.app.state.settings.model_copy(
        update={"network_snapshot_quota_bytes": quota}
    )


def _body(image_id: str, payload: bytes | None = None) -> bytes:
    data = payload if payload is not None else _jpeg()
    return json.dumps(
        {
            "schema_version": SNAPSHOT_UPLINK_SCHEMA_VERSION,
            "image_id": image_id,
            "device_id": DEVICE,
            "site_id": "site-1",
            "gateway_id": "gw-1",
            "incident_id": "sitinc-abc",
            "codec": "JPEG",
            "width": 1640,
            "height": 1232,
            "captured_at_ms": 1_795_155_756_003,
            "size_bytes": len(data),
            "content_sha256": hashlib.sha256(data).hexdigest(),
            "payload_b64": base64.b64encode(data).decode(),
        },
        separators=(",", ":"),
    ).encode()


def _post(client: TestClient, signing_key: Ed25519PrivateKey, body: bytes) -> Any:
    signature = signing_key.sign(body)
    return client.post(
        "/api/v1/network/edge/snapshot-images",
        content=body,
        headers={
            "Content-Type": "application/json",
            "X-Edge-Key-Id": KEY_ID,
            "X-Edge-Token": DEVICE_TOKEN,
            "X-Edge-Device-Id": DEVICE,
            "X-Edge-Signature": signature.hex(),
        },
    )


def _ids(db: Database) -> set[str]:
    with db.transaction(TENANT_CTX) as session:
        return set(session.scalars(select(NetworkSnapshotImageRecord.image_id)).all())


def _total_bytes(db: Database) -> int:
    with db.transaction(TENANT_CTX) as session:
        rows = session.scalars(select(NetworkSnapshotImageRecord)).all()
        return sum(row.size_bytes for row in rows)


# ===========================================================================
# 1. 配额没配 = 一次都不动
# ===========================================================================


def test_disabled_quota_does_not_even_construct_the_service(
    client: TestClient, signing_key: Ed25519PrivateKey
) -> None:
    """``network_snapshot_quota_bytes`` 默认 ``None``，此时连服务都不该构造。

    断言「没构造服务」而不是「没删数据」，是因为后者太弱：一个每次都跑
    全表 ``SUM`` 然后发现没超的实现，同样「没删数据」，但默认部署会白白
    多一次全表扫描。默认值必须是**零成本**的。
    """

    _set_quota(client, None)

    from industrial_ops_agent.api.routes import network_assurance as route_module

    constructed: list[Any] = []

    class _SpyQuotaService:
        def __init__(self, database: Any) -> None:
            constructed.append(database)

    with mock.patch.object(route_module, "SnapshotQuotaService", _SpyQuotaService):
        resp = _post(client, signing_key, _body("snap-disabled"))

    assert resp.status_code == 200, resp.text
    assert constructed == [], "配额未配置却构造了配额服务：默认部署多了一次查询"


# ===========================================================================
# 2. 配额生效：写成功之后淘汰最旧的，且不淘汰刚写的那张
# ===========================================================================


def test_quota_evicts_oldest_after_a_successful_write(
    client: TestClient, app_db: Database, signing_key: Ed25519PrivateKey
) -> None:
    """三张图、配额只装得下两张：最旧的那张必须被淘汰，新写的那张必须还在。

    「新写的那张必须还在」是这条用例真正的重点。它同时验证了两件事：
    一、淘汰确实发生在写之后（写之前的实现会先删到两张、再写第三张，
    结果一样是三张里少一张，看不出差别）；二、``keep_image_id`` 接线正确。
    """

    _set_quota(client, None)
    payload = _jpeg(400)
    for image_id in ("snap-old", "snap-mid"):
        assert _post(client, signing_key, _body(image_id, payload)).status_code == 200
    assert _ids(app_db) == {"snap-old", "snap-mid"}

    # 一张图的字节数就是上限：三张写进来必然要淘汰两张。
    _set_quota(client, len(payload))
    resp = _post(client, signing_key, _body("snap-new", payload))

    assert resp.status_code == 200, resp.text
    assert resp.json()["accepted"] is True
    assert _ids(app_db) == {"snap-new"}, "应当只留下刚写进来的那一张"
    assert _total_bytes(app_db) <= len(payload)


def test_quota_keeps_the_image_even_when_it_is_the_only_one(
    client: TestClient, app_db: Database, signing_key: Ed25519PrivateKey
) -> None:
    """配额比一张图还小、租户里只有这一张：**保留并告警，不删刚写的**。

    删掉它等于「接受上传然后当场销毁」。设备看到 200，再用同一个
    ``image_id`` 重试会撞冲突——这张证据永远存不进去。
    """

    _set_quota(client, 1)

    resp = _post(client, signing_key, _body("snap-lonely"))

    assert resp.status_code == 200, resp.text
    assert _ids(app_db) == {"snap-lonely"}
    assert _total_bytes(app_db) > 1, "超配额是预期结果，不是失败"


# ===========================================================================
# 3. 不该触发的路径一次都不触发
# ===========================================================================


def test_duplicate_upload_does_not_evict(
    client: TestClient, app_db: Database, signing_key: Ed25519PrivateKey
) -> None:
    """重放没有新字节入库，不该触发淘汰。

    若在这里淘汰，一次普通的网络重试就会删掉一批老证据——而这次重试
    **一张新图都没存进来**。
    """

    payload = _jpeg(400)
    _set_quota(client, None)
    for image_id in ("snap-a", "snap-b"):
        _post(client, signing_key, _body(image_id, payload))

    _set_quota(client, len(payload) * 2)
    first = _post(client, signing_key, _body("snap-c", payload))
    assert first.json()["accepted"] is True
    before = _ids(app_db)

    # 同一个 image_id、同样的内容：服务层判 duplicate。
    replay = _post(client, signing_key, _body("snap-c", payload))

    assert replay.status_code == 200, replay.text
    assert replay.json()["duplicate"] is True
    assert _ids(app_db) == before, "重放不该删除任何已有证据"


def test_object_write_failure_does_not_evict(
    client: TestClient, app_db: Database, signing_key: Ed25519PrivateKey
) -> None:
    """对象没写成功时不能淘汰：那是「新图没存下、旧图被删了」的净损失。"""

    payload = _jpeg(400)
    _set_quota(client, None)
    for image_id in ("snap-keep-a", "snap-keep-b"):
        _post(client, signing_key, _body(image_id, payload))
    before = _ids(app_db)

    class _FailingStore(LocalDiskTenantObjectStore):
        async def put(self, context: TenantContext, key: Any, content: bytes) -> None:
            raise OSError("simulated storage backend failure")

    client.app.state.object_store = _FailingStore(Path(client.app.state.object_store._root))
    _set_quota(client, len(payload))

    resp = _post(client, signing_key, _body("snap-will-fail", payload))

    assert resp.status_code >= 500, f"写对象失败却回了 {resp.status_code}"
    assert _ids(app_db) == before, "新图没存下，却不该因此删掉已有证据"


def test_quota_failure_never_fails_the_upload(
    client: TestClient, app_db: Database, signing_key: Ed25519PrivateKey
) -> None:
    """配额执行抛异常时，上传仍然成功。

    图已经落盘、行已经登记。这时候回 500，板端会重试，而重试会撞上
    ``image_id`` 冲突被拒——把「配额这一轮没算干净」放大成「这张证据
    永远存不进去」。
    """

    _set_quota(client, 1024)

    from industrial_ops_agent.api.routes import network_assurance as route_module

    class _ExplodingQuotaService:
        def __init__(self, database: Any) -> None:
            pass

        async def enforce(self, *args: Any, **kwargs: Any) -> Any:
            raise RuntimeError("simulated quota failure")

    with mock.patch.object(route_module, "SnapshotQuotaService", _ExplodingQuotaService):
        resp = _post(client, signing_key, _body("snap-quota-broken"))

    assert resp.status_code == 200, resp.text
    assert resp.json()["accepted"] is True
    assert _ids(app_db) == {"snap-quota-broken"}, "图已经存下了，不该因为配额失败而丢失"

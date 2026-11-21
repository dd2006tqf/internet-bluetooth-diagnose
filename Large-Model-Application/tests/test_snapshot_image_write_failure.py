"""对象写入失败时，数据库里**不能**留下行。

这是「写入顺序：对象存储 → 数据库」这条设计唯一能拿出证据的地方。
上一轮把它记成「503 分支从未被走到」，这一轮补上，但结论和当时的设想不同：

**它本来就不是 503。**

路由里写对象那一行是裸 ``await object_store.put(...)``，没有 try/except。
``put`` 抛异常会一路冒到全局异常处理器，变成 ``500 internal_error``。
真正的 503 只来自 ``get_object_store`` 取不到 store 时——那是「存储没装配」，
不是「存储写失败」。两者是不同的事：

| 情形 | 状态码 | 谁负责 |
| --- | --- | --- |
| store 没装配 | 503 `object_store_unavailable` | 部署/装配错误，板端应重试 |
| store 写入失败 | 500 `internal_error` | 存储故障，板端重试也不会立刻好 |

状态码是次要的。**真正的不变量是：库里不能有行。** 一条指向不存在对象的行
会声称证据已送达，而实际取不出内容——这比直接失败糟得多，因为它不可观测。

所以这个文件断言两件事：

1. ``put`` 失败时，响应是失败（不区分具体码），且 ``network_snapshot_images``
   里没有这张图的行；
2. ``put`` 失败时，对象存储里也没有残留（没有半张图）。

第 2 条看起来理所当然，但 ``LocalDiskTenantObjectStore`` 用的是
mkstemp + os.replace，``replace`` 之前失败会留下 ``.tmp-*``——它有专门的
清理路径，这里顺带从 HTTP 层再验一次，因为路由层才是最真实的调用方。
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
from sqlalchemy import create_engine, func, select
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


class _FailingStore(LocalDiskTenantObjectStore):
    """落盘存储在 ``put`` 上必失败，但**走完整条真实写入路径**。

    失败点选在 ``os.replace``：这是真实现里「临时文件已经写完、fsync 也做了，
    但没能转正」的那一步。选它而不是在 ``put`` 开头直接 ``raise``，是因为
    要验的正是**这条异常路径上的清理**——``_write_atomically`` 的 except
    分支必须把 ``.tmp-*`` 删掉。开头就 raise 的桩根本没走到那里，
    「没有残留」会变成一句谁也没证明过的话。

    也刻意不选「replace 成功之后再 raise」：那样磁盘上会留下一张完整对象
    而库里没有行。测出来的就不是残留问题，而是另一个场景（对象已送达、
    回执失败），结论指向相反的方向——那个场景下**残留是必然的**，
    而且是无害的（孤儿对象，可回收；不会出现「有行没内容」）。
    """

    def __init__(self, root: Path) -> None:
        super().__init__(root)
        self.put_attempts = 0

    async def put(self, context: TenantContext, key: Any, content: bytes) -> None:
        self.put_attempts += 1

        def _refuse_replace(src: Any, dst: Any) -> None:
            raise OSError("simulated storage backend failure")

        # ``local_disk`` 里写的就是 ``os.replace``，patch 的是同一个 os 模块
        # 对象，所以 ``asyncio.to_thread`` 里的写入线程也会看到这个替身。
        with mock.patch(
            "industrial_ops_agent.object_store.local_disk.os.replace",
            _refuse_replace,
        ):
            await super().put(context, key, content)


@pytest.fixture
def store(tmp_path: Path) -> _FailingStore:
    return _FailingStore(tmp_path / "objects")


@pytest.fixture
def client(
    app_db: Database,
    store: _FailingStore,
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
        SecurityAuditor(InMemorySecurityAuditSink(), hash_key=b"snapshot-fail-key")
    )
    app.state.object_store = store
    app.state.edge_telemetry_verifier = verifier
    app.state.network_assurance_service = NetworkAssuranceService(database=app_db)
    # ``raise_server_exceptions=False`` 是必须的，不是顺手加的选项。
    #
    # 默认 True 时 TestClient 会把服务端异常**原样抛出**（不经过
    # ``handle_unexpected_error``），于是永远看不到真实生产里的那个 500 响应。
    # 而这条用例要断言的正是「生产上会对这次失败回什么、以及库里剩下什么」——
    # 如果异常直接抛到测试函数里，那句「断言库里没有行」就变成了「断言异常前
    # 库里没有行」，而真实环境下请求早已返回、后台可能已经写了别的东西。
    #
    # 换句话说：默认设置会让这条用例测的是「异常长得对不对」，
    # 而不是「故障发生时系统状态对不对」。
    return TestClient(app, raise_server_exceptions=False)


def _body(image_id: str = "snap-write-fail", payload: bytes | None = None) -> bytes:
    """构造与板端同形的上传报文（原始字节，签名对它算）。"""

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


def _row_count(db: Database) -> int:
    with db.transaction(TENANT_CTX) as session:
        return session.scalar(select(func.count()).select_from(NetworkSnapshotImageRecord)) or 0


# ===========================================================================
# 核心不变量：put 失败 → 库里没有行
# ===========================================================================


def test_object_write_failure_leaves_no_row(
    client: TestClient, app_db: Database, store: _FailingStore, signing_key: Ed25519PrivateKey
) -> None:
    """这是本轮补的那条证据。

    断言的是**结果**而不是过程：对象写不进去时，数据库里绝不能出现一张
    声称「已送达」的行。那一行会让运营端以为证据在，而实际取不出任何字节。

    不断言具体错误码，因为那取决于 ``put`` 抛什么、以及全局处理器怎么映射。
    把断言钉在错误码上会让这条用例在「换个异常类型」时变红，而它要守的
    不变量根本没变。
    """

    resp = _post(client, signing_key, _body())

    assert store.put_attempts == 1, "对象存储根本没被调用，这条用例没测到东西"
    assert resp.status_code >= 500, f"写对象失败却回了 {resp.status_code}：{resp.text}"
    assert _row_count(app_db) == 0, "对象没写成功，数据库里却留下了行"


def test_object_write_failure_leaves_no_object_on_disk(
    client: TestClient, store: _FailingStore, signing_key: Ed25519PrivateKey
) -> None:
    """反过来也要干净：失败后磁盘上不能留下可被误认成证据的东西。

    真实现走的是 mkstemp + os.replace；上面的桩在 replace **之前**抛错，
    所以这里验的是「异常路径有没有清理临时文件」。残留的 ``.tmp-*`` 会
    慢慢把磁盘塞满，残留的半张 JPEG 更糟——它看起来像真图。
    """

    _post(client, signing_key, _body(image_id="snap-no-residue"))

    root = Path(store._root)
    stray = [p for p in root.rglob("*") if p.is_file() and p.name != ".keep"]
    assert stray == [], f"失败路径留下了残留文件: {stray}"


def test_successful_write_still_creates_exactly_one_row(
    client: TestClient, app_db: Database, tmp_path: Path, signing_key: Ed25519PrivateKey
) -> None:
    """对照组：同样的请求换成会成功的存储，必须恰好落一行。

    没有这条，上面两条「0 行」的断言可能因为**任何**原因通过（比如签名
    根本没过、路由 401 了而库里当然没有行）。这条负责排除那种假绿。
    """

    healthy = LocalDiskTenantObjectStore(tmp_path / "healthy-objects")
    client.app.state.object_store = healthy

    resp = _post(client, signing_key, _body(image_id="snap-happy"))
    assert resp.status_code == 200, resp.text
    assert resp.json()["accepted"] is True
    assert _row_count(app_db) == 1

    # 而且取回能通过摘要校验——把「写进去了」变成「真的能读回来」。
    got = client.get(
        f"/api/v1/network/assets/{DEVICE}/snapshot-images/snap-happy/content",
        headers={"Authorization": "Bearer op-1:expert"},
    )
    assert got.status_code == 200
    assert hashlib.sha256(got.content).hexdigest() == hashlib.sha256(_jpeg()).hexdigest()

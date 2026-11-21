"""单租户静帧总量上限：淘汰最旧的，永不淘汰刚写进来的那张。

## 这个文件在守什么

保留期与配额是两个正交的旋钮。保留期回答「多久以后删」，它的失效方式是
**短期写入风暴**：磁盘在证据过期之前就被写满。配额回答「最多占多少」。

这里每一条用例都对应一种**静默失效**——它们全部不会报错，只会让磁盘慢慢满
或者让证据悄悄消失：

| 如果这里错了 | 表面现象 | 真实后果 |
| --- | --- | --- |
| 淘汰最新而不是最旧 | 上传一直成功 | 运营端正在看的那张图被自己删掉 |
| 淘汰刚写入的那张 | 上传成功、立即 404 | 设备重试 → 撞 image_id 冲突 → 永远存不进 |
| 配额只算对象不算行 | `SUM` 比实际小 | 上限形同虚设，磁盘照样满 |
| 淘汰走「先删对象」 | 单测全绿 | 崩在中间留下谎报行（见 snapshot_deletion） |
| 淘汰失败让上传 500 | 图已落盘却回错误 | 板端重试被 409 拒：净损失 |
| 配额未配置时仍查库 | 每次上传多一次全表 SUM | 默认部署白白多一次 IO |

最后一条用「配额为 None 时连一次 SUM 都不做」来钉，测在路由层
（``test_snapshot_image_quota_route.py``），这里测的是服务层语义。
"""

from __future__ import annotations

import asyncio
from datetime import UTC, datetime, timedelta
from pathlib import Path

import pytest
from sqlalchemy import create_engine, func, select
from sqlalchemy.pool import StaticPool

from industrial_ops_agent.network_assurance.snapshot_quota import (
    MAX_EVICTION_ROWS,
    SnapshotQuotaService,
)
from industrial_ops_agent.object_store.keys import edge_snapshot_image_object_key
from industrial_ops_agent.object_store.local_disk import LocalDiskTenantObjectStore
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import Base, NetworkSnapshotImageRecord
from industrial_ops_agent.persistence.tenant import TenantContext

TENANT = "tenant-quota"
OTHER_TENANT = "tenant-other"
TENANT_CTX = TenantContext(tenant_id=TENANT, subject_id="quota-worker")
NOW = datetime(2026, 11, 21, 12, 0, tzinfo=UTC)
PAYLOAD = b"\xff\xd8quota-test-payload\xff\xd9"


@pytest.fixture
def db() -> Database:
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


def _seed(
    db: Database,
    store: LocalDiskTenantObjectStore,
    image_id: str,
    *,
    age_hours: float,
    size_bytes: int | None = None,
    tenant: str = TENANT,
    with_object: bool = True,
) -> str:
    """登记一张图并把对象真的写进存储。返回 object_key。

    ``age_hours`` 同时决定 ``received_at`` 与 ``captured_at_ms``，**但两者
    不总是同序**——见 ``test_evicts_by_received_at_not_captured_at``。
    """

    size = len(PAYLOAD) if size_bytes is None else size_bytes
    context = TenantContext(tenant_id=tenant, subject_id="quota-worker")
    key = edge_snapshot_image_object_key(context, image_id)
    if with_object:
        asyncio.run(store.put(context, key, PAYLOAD))

    received_at = NOW - timedelta(hours=age_hours)
    with db.transaction(context) as session:
        session.add(
            NetworkSnapshotImageRecord(
                image_id=image_id,
                tenant_id=tenant,
                asset_id="radxa-cubie-a7a",
                site_id="",
                gateway_id="radxa-cubie-a7a",
                incident_id="sitinc-test",
                codec="JPEG",
                width=1640,
                height=1232,
                captured_at_ms=int(received_at.timestamp() * 1000),
                size_bytes=size,
                content_sha256="0" * 64,
                object_key=key.value,
                received_at=received_at,
            )
        )
    return key.value


def _set_captured_at(db: Database, image_id: str, captured_at_ms: int) -> None:
    """只改板端自述的拍摄时刻，模拟一块时钟不准的板子。"""

    with db.transaction(TENANT_CTX) as session:
        row = session.get(NetworkSnapshotImageRecord, image_id)
        assert row is not None
        row.captured_at_ms = captured_at_ms


def _count(db: Database, tenant: str = TENANT) -> int:
    context = TenantContext(tenant_id=tenant, subject_id="quota-worker")
    with db.transaction(context) as session:
        return (
            session.scalar(
                select(func.count())
                .select_from(NetworkSnapshotImageRecord)
                .where(NetworkSnapshotImageRecord.tenant_id == tenant)
            )
            or 0
        )


def _total(db: Database, tenant: str = TENANT) -> int:
    context = TenantContext(tenant_id=tenant, subject_id="quota-worker")
    with db.transaction(context) as session:
        return int(
            session.scalar(
                select(func.coalesce(func.sum(NetworkSnapshotImageRecord.size_bytes), 0)).where(
                    NetworkSnapshotImageRecord.tenant_id == tenant
                )
            )
            or 0
        )


def _object_exists(
    store: LocalDiskTenantObjectStore, object_key: str, tenant: str = TENANT
) -> bool:
    context = TenantContext(tenant_id=tenant, subject_id="quota-worker")
    from industrial_ops_agent.object_store.keys import tenant_object_key_from_value

    key = tenant_object_key_from_value(context, object_key)
    return asyncio.run(store.get(context, key)) is not None


def _enforce(service: SnapshotQuotaService, store, **kwargs):
    return asyncio.run(service.enforce(TENANT_CTX, store, **kwargs))


# ===========================================================================
# 1. 不该动的时候一点都不动
# ===========================================================================


def test_under_quota_evicts_nothing(db: Database, store: LocalDiskTenantObjectStore) -> None:
    """没超上限就不该删任何东西。上限不是「总是删到某个数」。"""

    _seed(db, store, "snap-a", age_hours=3)
    _seed(db, store, "snap-b", age_hours=2)
    _seed(db, store, "snap-c", age_hours=1)

    plan = _enforce(SnapshotQuotaService(db), store, quota_bytes=10 * 1024 * 1024)

    assert plan.evicted == 0
    assert plan.over_quota is False
    assert _count(db) == 3


def test_exactly_at_quota_evicts_nothing(db: Database, store: LocalDiskTenantObjectStore) -> None:
    """**边界：等于上限不算超。** 写成 ``<`` 会让「刚好装下」的部署每天删一张。"""

    for index in range(3):
        _seed(db, store, f"snap-{index}", age_hours=3 - index)
    total = _total(db)

    plan = _enforce(SnapshotQuotaService(db), store, quota_bytes=total)

    assert plan.evicted == 0
    assert _count(db) == 3


def test_tenant_isolation_other_tenant_untouched(
    db: Database, store: LocalDiskTenantObjectStore
) -> None:
    """配额只算**本租户**。把另一个租户的字节算进来会让它替别人腾地方。"""

    _seed(db, store, "snap-mine", age_hours=2)
    _seed(db, store, "snap-theirs", age_hours=2, tenant=OTHER_TENANT)

    # 本租户只有 24 字节，配额 1 KB：不超。
    plan = _enforce(SnapshotQuotaService(db), store, quota_bytes=1024)

    assert plan.evicted == 0
    assert plan.total_bytes_before == len(PAYLOAD)
    assert _count(db, OTHER_TENANT) == 1


# ===========================================================================
# 2. 超了之后删谁：最旧的，且绝不动刚写进来的
# ===========================================================================


def test_evicts_oldest_first(db: Database, store: LocalDiskTenantObjectStore) -> None:
    """按 ``received_at`` 升序淘汰，直到装得下。"""

    _seed(db, store, "snap-oldest", age_hours=5)
    _seed(db, store, "snap-middle", age_hours=3)
    _seed(db, store, "snap-newest", age_hours=1)

    # 上限只装得下两张。
    plan = _enforce(SnapshotQuotaService(db), store, quota_bytes=len(PAYLOAD) * 2)

    assert plan.evicted == 1
    assert plan.over_quota is False
    remaining = _remaining_ids(db)
    assert "snap-oldest" not in remaining
    assert remaining == {"snap-middle", "snap-newest"}


def test_evicts_multiple_when_over_by_several(
    db: Database, store: LocalDiskTenantObjectStore
) -> None:
    for index in range(5):
        _seed(db, store, f"snap-{index}", age_hours=10 - index)

    plan = _enforce(SnapshotQuotaService(db), store, quota_bytes=len(PAYLOAD) * 2)

    assert plan.evicted == 3
    assert _count(db) == 2
    assert _remaining_ids(db) == {"snap-3", "snap-4"}


def test_never_evicts_the_image_just_written(
    db: Database, store: LocalDiskTenantObjectStore
) -> None:
    """``keep_image_id`` 是硬约束，即使它是「最旧的」。

    这是最关键的一条：如果它被淘汰，设备看到 200 而图立即消失，重试还会撞
    ``image_id`` 冲突——这张证据将**永远**存不进去。所以哪怕它的
    ``received_at`` 更早（时钟回拨、或测试里显式造出来），也不能删。
    """

    # keep 的那张反而更旧，确保「排除」不是因为排序恰好把它放最后。
    _seed(db, store, "snap-keep", age_hours=9)
    _seed(db, store, "snap-other", age_hours=1)

    plan = _enforce(
        SnapshotQuotaService(db),
        store,
        quota_bytes=1,
        keep_image_id="snap-keep",
    )

    assert _count(db) == 1
    assert _remaining_ids(db) == {"snap-keep"}
    assert plan.evicted == 1


def test_quota_smaller_than_one_image_warns_and_keeps_it(
    db: Database, store: LocalDiskTenantObjectStore
) -> None:
    """配额比一张图还小：**告警并保留，不删刚写的那张**。

    此时「淘汰到装得下」的唯一解法是删掉刚存进来的图——接受上传然后当场
    销毁。这里宁可超配额并让 ``over_quota`` 为真，让运维看见配置错了。
    """

    _seed(db, store, "snap-only", age_hours=1)

    plan = _enforce(
        SnapshotQuotaService(db),
        store,
        quota_bytes=1,
        keep_image_id="snap-only",
    )

    assert plan.evicted == 0
    assert plan.over_quota is True
    assert _count(db) == 1, "配额配错不该把唯一一张证据删掉"


def test_evicts_by_received_at_not_captured_at(
    db: Database, store: LocalDiskTenantObjectStore
) -> None:
    """判据是服务端的 ``received_at``，不是板端自述的 ``captured_at_ms``。

    一块没对过 NTP 的板子可以报出 1970 年。若按 ``captured_at_ms`` 排序，
    「最旧的」会变成「时钟最不准的」——那块板子的图会被优先删掉，而它与
    「哪张证据更该留」毫无关系。
    """

    _seed(db, store, "snap-received-later", age_hours=1)
    _seed(db, store, "snap-received-earlier", age_hours=5)
    # 把「晚收到的」那张的 captured_at_ms 改到 1970 年。
    _set_captured_at(db, "snap-received-later", 0)

    plan = _enforce(SnapshotQuotaService(db), store, quota_bytes=len(PAYLOAD))

    assert plan.evicted == 1
    assert _remaining_ids(db) == {"snap-received-later"}, (
        "按 captured_at_ms 排序会删掉这 1970 年的假时钟图，而它才是刚收到的证据"
    )


# ===========================================================================
# 3. 删的时候两个都对：行和对象
# ===========================================================================


def test_evicted_rows_take_their_objects_with_them(
    db: Database, store: LocalDiskTenantObjectStore
) -> None:
    """淘汰必须把对象一起删。只删行会留下一堆孤儿对象，磁盘一个字节都不会少——
    配额就成了「删记录」而不是「腾空间」。"""

    key = _seed(db, store, "snap-old", age_hours=5)
    _seed(db, store, "snap-new", age_hours=1)
    assert _object_exists(store, key)

    plan = _enforce(SnapshotQuotaService(db), store, quota_bytes=len(PAYLOAD))

    assert plan.evicted == 1
    assert not _object_exists(store, key), "行删了但对象还在：配额没真正腾出空间"
    assert plan.orphaned_objects == 0


def test_object_delete_failure_counts_as_orphan_but_rows_still_go(
    db: Database, store: LocalDiskTenantObjectStore
) -> None:
    """对象删不掉时行照删、记 orphaned，**不回滚行**。

    回滚行会留下「行在、对象没了」的谎报行——那正是整个删除顺序在避免的。
    孤儿对象浪费空间但可回收，谎报行会把「丢了」伪装成「还在」。
    """

    key = _seed(db, store, "snap-old", age_hours=5)
    _seed(db, store, "snap-new", age_hours=1)

    class _FailingDeleteStore(LocalDiskTenantObjectStore):
        async def delete(self, context, key):  # type: ignore[override]
            raise OSError("simulated storage failure")

    failing = _FailingDeleteStore(store._root)

    plan = _enforce(SnapshotQuotaService(db), failing, quota_bytes=len(PAYLOAD))

    assert plan.evicted == 1
    assert plan.orphaned_objects == 1
    assert _count(db) == 1, "行必须删掉，否则留下谎报行"
    assert _object_exists(store, key), "对象确实没删掉（数据还在，可被清扫器回收）"


def test_eviction_is_capped_by_max_eviction_rows(
    db: Database, store: LocalDiskTenantObjectStore
) -> None:
    """单次淘汰有上限，剩下的留给下一轮。

    没有上限时，一次「配额被调到 1 字节」的误操作会让一次上传锁住大半张表。
    测法：把上限临时调小——用 monkeypatch 改模块常量，断言只淘汰了那么多。
    """

    import industrial_ops_agent.network_assurance.snapshot_quota as quota_module

    for index in range(6):
        _seed(db, store, f"snap-{index}", age_hours=10 - index)

    original = quota_module.MAX_EVICTION_ROWS
    quota_module.MAX_EVICTION_ROWS = 2
    try:
        plan = _enforce(SnapshotQuotaService(db), store, quota_bytes=1)
    finally:
        quota_module.MAX_EVICTION_ROWS = original

    assert plan.evicted == 2, "单批上限必须生效"
    assert _count(db) == 4
    assert plan.over_quota is True, "没删完就还没有回到上限之内"


def test_eviction_cap_constant_is_sane() -> None:
    # 上限不能是 0（那会让配额永远不生效），也不能大到一次删空整张表。
    assert 1 <= MAX_EVICTION_ROWS <= 100_000


# ===========================================================================
# 4. 参数拒绝
# ===========================================================================


@pytest.mark.parametrize("quota", [0, -1])
def test_non_positive_quota_is_refused(
    db: Database, store: LocalDiskTenantObjectStore, quota: int
) -> None:
    """0 不是「不限制」，是「删光」（与保留期 0 天同一个陷阱）。"""

    _seed(db, store, "snap-a", age_hours=1)

    with pytest.raises(ValueError):
        _enforce(SnapshotQuotaService(db), store, quota_bytes=quota)

    assert _count(db) == 1, "拒绝必须发生在删除之前"


# ===========================================================================
# 5. 没有对象的行也能淘汰
# ===========================================================================


def test_row_without_object_still_evicts(db: Database, store: LocalDiskTenantObjectStore) -> None:
    """历史遗留的「有行、对象已丢」的行必须能被淘汰，否则它会永久占着配额额度。

    对象删除对不存在的键是幂等的（``_unlink_quietly``），所以不算失败。
    """

    _seed(db, store, "snap-ghost", age_hours=5, with_object=False)
    _seed(db, store, "snap-real", age_hours=1)

    plan = _enforce(SnapshotQuotaService(db), store, quota_bytes=len(PAYLOAD))

    assert plan.evicted == 1
    assert plan.orphaned_objects == 0
    assert _remaining_ids(db) == {"snap-real"}


# ===========================================================================
# helpers
# ===========================================================================


def _remaining_ids(db: Database, tenant: str = TENANT) -> set[str]:
    context = TenantContext(tenant_id=tenant, subject_id="quota-worker")
    with db.transaction(context) as session:
        return set(
            session.scalars(
                select(NetworkSnapshotImageRecord.image_id).where(
                    NetworkSnapshotImageRecord.tenant_id == tenant
                )
            ).all()
        )

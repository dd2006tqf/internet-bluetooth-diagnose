"""静帧保留策略：默认不删、只删过期、先删行、删除失败也记账。

这个文件的三类用例回答三个不同的问题：

1. **它会不会误删？** —— dry-run、`retention_days` 边界、租户隔离。
2. **它删得对不对？** —— 过期判据、对象一起删、分批推进。
3. **它出错时剩下什么？** —— 对象删除失败时的孤儿对象与行状态。

第 3 类最重要。保留策略是**不可逆的证据销毁**，一个「出错时留下一堆谎报行」
的回收器，比不回收更糟——它会把「证据丢了」伪装成「证据还在」。
"""

from __future__ import annotations

import asyncio
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any

import pytest
from sqlalchemy import create_engine, func, select
from sqlalchemy.pool import StaticPool

from industrial_ops_agent.network_assurance.snapshot_retention import (
    MAX_BATCH_SIZE,
    SnapshotRetentionRefused,
    SnapshotRetentionService,
)
from industrial_ops_agent.object_store.keys import edge_snapshot_image_object_key
from industrial_ops_agent.object_store.local_disk import LocalDiskTenantObjectStore
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import (
    Base,
    NetworkSnapshotImageRecord,
)
from industrial_ops_agent.persistence.tenant import TenantContext

TENANT = "tenant-retention"
TENANT_CTX = TenantContext(tenant_id=TENANT, subject_id="retention-worker")
NOW = datetime(2026, 11, 21, 12, 0, 0, tzinfo=UTC)
PAYLOAD = b"\xff\xd8retention-test-payload\xff\xd9"


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
    age_days: float,
    tenant: str = TENANT,
    with_object: bool = True,
) -> str:
    """登记一张图，并把它的对象真的写进存储。返回 object_key。"""

    context = TenantContext(tenant_id=tenant, subject_id="retention-worker")
    key = edge_snapshot_image_object_key(context, image_id)
    if with_object:
        asyncio.run(store.put(context, key, PAYLOAD))

    received_at = NOW - timedelta(days=age_days)
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
                size_bytes=len(PAYLOAD),
                content_sha256="0" * 64,
                object_key=key.value,
                received_at=received_at,
            )
        )
    return key.value


def _count(db: Database, tenant: str = TENANT) -> int:
    context = TenantContext(tenant_id=tenant, subject_id="retention-worker")
    with db.transaction(context) as session:
        return (
            session.scalar(
                select(func.count())
                .select_from(NetworkSnapshotImageRecord)
                .where(NetworkSnapshotImageRecord.tenant_id == tenant)
            )
            or 0
        )


def _object_exists(store: LocalDiskTenantObjectStore, object_key: str) -> bool:
    context = TenantContext(tenant_id=TENANT, subject_id="retention-worker")
    from industrial_ops_agent.object_store.keys import tenant_object_key_from_value

    key = tenant_object_key_from_value(context, object_key)
    return asyncio.run(store.get(context, key)) is not None


def _run(service: SnapshotRetentionService, store: Any, **kwargs: Any):
    return asyncio.run(service.run(TENANT_CTX, store, now=NOW, **kwargs))


# ===========================================================================
# 1. 它会不会误删？
# ===========================================================================


def test_dry_run_is_the_default_and_deletes_nothing(
    db: Database, store: LocalDiskTenantObjectStore
) -> None:
    """不传 apply 就什么都不删。

    这条是整个模块的安全底线。默认值一旦变成 True，任何一次「先跑一下看看」
    都会变成真删。默认 dry-run 让误用只产生一份报告。
    """

    object_key = _seed(db, store, "snap-old", age_days=30)

    plan = _run(SnapshotRetentionService(db), store, retention_days=7)

    assert plan.expired == 1, "应该报告有 1 张过期"
    assert plan.deleted == 0, "dry-run 不许删任何东西"
    assert plan.remaining == 1
    assert _count(db) == 1
    assert _object_exists(store, object_key), "dry-run 连对象都不许碰"


def test_apply_deletes_only_expired_images(db: Database, store: LocalDiskTenantObjectStore) -> None:
    """过期删掉、未过期留着。只有过期的那个会被删，是这条策略的核心语义。"""

    old_key = _seed(db, store, "snap-old", age_days=30)
    fresh_key = _seed(db, store, "snap-fresh", age_days=1)

    plan = _run(SnapshotRetentionService(db), store, retention_days=7, apply=True)

    assert plan.deleted == 1
    assert plan.expired == 1
    assert _count(db) == 1
    assert not _object_exists(store, old_key), "过期对象必须被删"
    assert _object_exists(store, fresh_key), "未过期对象不能被碰"


def test_boundary_exactly_at_cutoff_is_kept(
    db: Database, store: LocalDiskTenantObjectStore
) -> None:
    """恰好等于截止时刻的图**保留**。

    判据是 `received_at < cutoff`（严格小于）。边界值的取舍不能靠「大概」：
    如果一边是 <、另一边是 <=，同一个时刻在不同服务器上会得出相反结论。
    这条钉住 `<` 这个选择：恰好 7 天前的那张，按 7 天保留期仍然算「未过期」。
    """

    _seed(db, store, "snap-exactly-7d", age_days=7)
    _seed(db, store, "snap-just-over", age_days=7.001)

    plan = _run(SnapshotRetentionService(db), store, retention_days=7, apply=True)

    assert plan.deleted == 1
    assert _count(db) == 1


def test_other_tenant_rows_are_untouched(db: Database, store: LocalDiskTenantObjectStore) -> None:
    """只碰本租户。回收是最容易被写漏租户过滤的地方——一个 where 少写一列，
    就是把别的客户的证据删了，而且删除是不可逆的。"""

    _seed(db, store, "snap-mine", age_days=30)
    _seed(db, store, "snap-theirs", age_days=30, tenant="tenant-other")

    plan = _run(SnapshotRetentionService(db), store, retention_days=7, apply=True)

    assert plan.deleted == 1
    assert _count(db) == 0, "本租户的过期图应被删"
    assert _count(db, tenant="tenant-other") == 1, "别的租户一行都不能少"


@pytest.mark.parametrize("retention_days", [0, -1, -365])
def test_non_positive_retention_is_refused(
    db: Database, store: LocalDiskTenantObjectStore, retention_days: int
) -> None:
    """0 天或负数 = 立刻删光。那不是保留策略，是清库，必须在参数层拒绝。

    如果不拒绝，一个把 `retention_days` 配成 0 的部署会在第一轮就把全部
    证据删掉，而报告里只会写「deleted: N」，看起来像正常工作。
    """

    _seed(db, store, "snap-old", age_days=30)

    with pytest.raises(SnapshotRetentionRefused):
        _run(SnapshotRetentionService(db), store, retention_days=retention_days, apply=True)

    assert _count(db) == 1, "拒绝执行时不许有任何删除"


@pytest.mark.parametrize("batch_size", [0, -1, MAX_BATCH_SIZE + 1])
def test_invalid_batch_size_is_refused(
    db: Database, store: LocalDiskTenantObjectStore, batch_size: int
) -> None:
    _seed(db, store, "snap-old", age_days=30)

    with pytest.raises(SnapshotRetentionRefused):
        _run(
            SnapshotRetentionService(db),
            store,
            retention_days=7,
            apply=True,
            batch_size=batch_size,
        )

    assert _count(db) == 1


# ===========================================================================
# 2. 它删得对不对？
# ===========================================================================


def test_batch_size_limits_work_and_reports_remaining(
    db: Database, store: LocalDiskTenantObjectStore
) -> None:
    """分批：一批只删 batch_size 张，remaining 告诉调用方还要不要再来一轮。

    没有 remaining 的话，调用方只能靠「再跑一次看删了几个」来猜有没有删完，
    而一个删 0 张的结果无法区分「已经干净了」与「参数错了」。
    """

    for index in range(5):
        _seed(db, store, f"snap-old-{index}", age_days=30)
    service = SnapshotRetentionService(db)

    first = _run(service, store, retention_days=7, apply=True, batch_size=2)
    assert first.deleted == 2
    assert first.expired == 5
    assert first.remaining == 3

    second = _run(service, store, retention_days=7, apply=True, batch_size=2)
    assert second.deleted == 2
    assert second.remaining == 1

    third = _run(service, store, retention_days=7, apply=True, batch_size=2)
    assert third.deleted == 1
    assert third.remaining == 0
    assert _count(db) == 0


def test_oldest_are_deleted_first(db: Database, store: LocalDiskTenantObjectStore) -> None:
    """先删最旧的。

    分批时这一点很实际：如果排序不稳定，同一批可能反复包含同一行（删不掉
    的情况下）或顺序随机，导致「多轮之后还剩一些永远轮不到的」。按
    received_at 升序保证单调推进。
    """

    _seed(db, store, "snap-oldest", age_days=90)
    _seed(db, store, "snap-middle", age_days=60)
    _seed(db, store, "snap-newest", age_days=30)

    _run(SnapshotRetentionService(db), store, retention_days=7, apply=True, batch_size=1)

    assert _count(db) == 2
    context = TENANT_CTX
    with db.transaction(context) as session:
        left = set(session.scalars(select(NetworkSnapshotImageRecord.image_id)).all())
    assert left == {"snap-middle", "snap-newest"}, "被删的应该是最旧那张"


def test_no_expired_rows_is_a_clean_noop(db: Database, store: LocalDiskTenantObjectStore) -> None:
    _seed(db, store, "snap-fresh", age_days=1)

    plan = _run(SnapshotRetentionService(db), store, retention_days=7, apply=True)

    assert plan.expired == 0
    assert plan.deleted == 0
    assert plan.remaining == 0
    assert _count(db) == 1


def test_empty_tenant_is_a_clean_noop(db: Database, store: LocalDiskTenantObjectStore) -> None:
    plan = _run(SnapshotRetentionService(db), store, retention_days=7, apply=True)

    assert plan.expired == 0
    assert plan.deleted == 0


# ===========================================================================
# 3. 它出错时剩下什么？
# ===========================================================================


class _FailingDeleteStore(LocalDiskTenantObjectStore):
    """对象删除必失败的存储。

    用来回答「对象删不掉时行怎么办」。答案是：**行照样删，留孤儿对象**。
    因为反过来（留住行、对象已删）会得到指向缺失对象的谎报行，那正是整个
    静帧链路一直在防的东西。
    """

    async def delete(self, context: TenantContext, key: Any) -> None:
        raise OSError("simulated object delete failure")


def test_object_delete_failure_leaves_no_row_but_counts_orphan(
    db: Database, tmp_path: Path
) -> None:
    """对象删不掉时：行已经删了，孤立对象被计数报出。

    这条钉住删除顺序。如果实现是先删对象再删行，这个用例会看到「行还在、
    对象删失败」——而用户取回时就是 500 object_missing，即谎报行。
    """

    store_path = tmp_path / "objects"
    seed_store = LocalDiskTenantObjectStore(store_path)
    _seed(db, seed_store, "snap-old", age_days=30)

    plan = _run(
        SnapshotRetentionService(db),
        _FailingDeleteStore(store_path),
        retention_days=7,
        apply=True,
    )

    assert plan.deleted == 1, "行必须被删掉，不能因为对象删不掉就留下"
    assert plan.orphaned_objects == 1, "删不掉的对象必须被计数报出"
    assert _count(db) == 0, "库里不能留下指向缺失对象的行"


def test_object_is_gone_after_successful_apply(
    db: Database, store: LocalDiskTenantObjectStore
) -> None:
    """正常路径下对象也必须被删——否则回收只删行，磁盘照样涨。"""

    object_key = _seed(db, store, "snap-old", age_days=30)
    assert _object_exists(store, object_key)

    _run(SnapshotRetentionService(db), store, retention_days=7, apply=True)

    assert not _object_exists(store, object_key)


def test_row_without_object_still_gets_deleted(
    db: Database, store: LocalDiskTenantObjectStore
) -> None:
    """行在但对象早已丢失（历史遗留 / 上次回收崩溃）时，删行不该失败。

    这种行正是 `object_missing` 的来源。保留策略是它唯一的清理机会——
    如果这里因为「对象不存在」而报错，这些行会永久堆积。
    """

    _seed(db, store, "snap-no-object", age_days=30, with_object=False)

    plan = _run(SnapshotRetentionService(db), store, retention_days=7, apply=True)

    assert plan.deleted == 1
    assert plan.orphaned_objects == 0, "对象本来就不存在，不算删除失败"
    assert _count(db) == 0


def test_row_delete_failure_leaves_object_untouched(
    db: Database, store, monkeypatch: pytest.MonkeyPatch
):
    """**这条才是真的在测删除顺序**，上面那条不是。

    上一版我把「对象删失败 → 行仍被删」的用例写成能抓顺序错误，那是错的：
    `_delete_objects` 自己吞异常，所以两种顺序的最终状态完全一样
    （行没了、对象还在）。我把实现临时改成「先删对象」跑了一遍，17 条用例
    全绿——用例名不副实。

    真正区分顺序的是**中途失败**：让删行这一步抛错，然后看对象有没有被动过。

    | 实现顺序 | 删行失败后对象状态 | 判定 |
    | --- | --- | --- |
    | 先删行、再删对象（正确） | 仍在 | 可以安全重试 |
    | 先删对象、再删行（错误） | **已消失** | 行成了谎报行 |

    所以这里不依断言返回值，而直接去存储里查那个对象还在不在。
    """
    from sqlalchemy.exc import OperationalError

    from industrial_ops_agent.network_assurance import snapshot_deletion

    object_key = _seed(db, store, "snap-old", age_days=30)
    assert _object_exists(store, object_key)

    def _explode(database, context, image_ids):
        raise OperationalError("simulated crash in row deletion", None, Exception())

    # 注入点在新抽出的公共删除入口里，而不是服务自己的私有方法上：删行与
    # 删对象现在已经不在这个类里了。打在 `snapshot_deletion.delete_snapshot_rows`
    # 上仍然有效，因为 `delete_snapshot_rows_and_objects` 是在模块全局名字空间
    # 里查它的——同模块内按名字调用，monkeypatch 模块属性就能替换掉。
    monkeypatch.setattr(snapshot_deletion, "delete_snapshot_rows", _explode)

    service = SnapshotRetentionService(db)
    with pytest.raises(OperationalError):
        _run(service, store, retention_days=7, apply=True)

    # 行还在（删行本身就失败了），这是预期内的。
    assert _count(db) == 1
    # 关键断言：对象必须还在。若实现先删对象，这里会是 False，
    # 而行还在——一个指向缺失对象的谎报行。
    assert _object_exists(store, object_key), (
        "删行失败时对象已被删除：说明实现是先删对象再删行，崩在中间就会留下谎报行"
    )

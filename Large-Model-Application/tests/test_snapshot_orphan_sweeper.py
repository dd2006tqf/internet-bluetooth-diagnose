"""孤儿静帧对象清扫器：**不该删什么** 比该删什么重要得多。

## 这个文件在守什么

孤儿对象的定义是「库里没有行」。而「库里没有行」有两种完全不同的来源：

| 来源 | 该不该删 | 判据 |
| --- | --- | --- |
| 一次上传崩在 `put` 与 `insert` 之间 | **删** | 无行 **且** 对象足够老 |
| 一次上传**正在** `put` 与 `insert` 之间 | **绝不能删** | 无行 **但** 对象很新 |
| 行已删、对象删失败（`delete_snapshot_objects` 吞了异常） | **删** | 无行 且 足够老 |

第二行是本模块唯一真正危险的失效：删掉它会在上传方插入一行之后得到
「行在、对象没了」——正是整条留证链路一直在防的**谎报行**，而且取回时 500。
所以用例里绝大多数都在断言「什么情况下不删」。

每条都对应一种**不会报错**的失效：

| 如果这里错了 | 表面现象 | 真实后果 |
| --- | --- | --- |
| 不看时间窗，无行就删 | 清扫「成功」 | 删掉正在上传的图 → 上传方写入谎报行 |
| 时间窗按 `>=` 判（边界算旧） | 边界上偶尔删错 | 与上一行同因，概率低但更未难查 |
| 枚举前缀少了结尾 `/` | 扫到更多对象 | 误删 `snapshot-images-*` 兄弟命名空间 |
| 前缀写错租户 | 清扫「正常」 | 跨租户证据销毁，不可逆 |
| 把视频分片也枚举进来 | 扫到一堆孤儿 | 删掉整段视频留证（`video-clips/`） |
| 删失败不计入 remaining | 报告「已清空」 | 磁盘一直不降，且没人知道为什么 |
| 反向孤儿（有行无对象）也被「对齐」 | 数据看起来自洽 | 把「证据丢了」伪装成「从来没有过」 |
| dry-run 也真的删 | 一切正常 | 第一次上线就把证据删了 |

## 为什么用真 `LocalDiskTenantObjectStore`

`list_prefix` 是本模块依赖的新能力，而「本地盘实现按前缀枚举」这件事本身
（拼接键、用 mtime 而不是内容、只扫一层）也是被测对象。用假存储会把这一层
替换掉，于是「清扫器调对了、实现列错了」这种组合故障永远测不出来。

时间窗用注入的 `now` 而不是 `sleep`：用例要能在微秒内跑完，也要能精确断言
边界（`last_modified == cutoff` 时到底删不删）。
"""

from __future__ import annotations

import asyncio
import os
from datetime import UTC, datetime, timedelta
from pathlib import Path

import pytest
from sqlalchemy import create_engine
from sqlalchemy.pool import StaticPool

from industrial_ops_agent.network_assurance.snapshot_orphan_sweeper import (
    DEFAULT_MIN_AGE_SECONDS,
    MAX_SWEEP_OBJECTS,
    SnapshotOrphanSweeper,
    snapshot_images_prefix,
)
from industrial_ops_agent.object_store.local_disk import LocalDiskTenantObjectStore
from industrial_ops_agent.object_store.memory import InMemoryTenantObjectStore
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import Base, NetworkSnapshotImageRecord
from industrial_ops_agent.persistence.tenant import TenantContext

TENANT = "tenant-sweep"
OTHER_TENANT = "tenant-sweep-other"
TENANT_CTX = TenantContext(tenant_id=TENANT, subject_id="sweeper")
NOW = datetime(2026, 11, 21, 12, 0, tzinfo=UTC)
PAYLOAD = b"\xff\xd8sweep-test\xff\xd9"


def _run(coro):
    return asyncio.run(coro)


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


def _key_value(image_id: str, tenant: str = TENANT) -> str:
    return f"tenant/{tenant}/quarantine/snapshot-images/{image_id}"


def _write(
    store: LocalDiskTenantObjectStore,
    image_id: str,
    *,
    tenant: str = TENANT,
    mtime: datetime | None = None,
) -> Path:
    """直接按路径写对象，绕过 ``put``。

    为什么要绕过：``put`` 总是把 mtime 设成「现在」，而本模块被测的正是 mtime
    与时间窗的关系。用 ``os.utime`` 造出「一小时前写的对象」是这里唯一能表达
    「上次进程崩在 put 与 insert 之间」的手段。
    """

    path = store._root.joinpath(*_key_value(image_id, tenant).split("/"))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(PAYLOAD)
    if mtime is not None:
        stamp = mtime.timestamp()
        os.utime(path, (stamp, stamp))
    return path


def _seed_row(
    db: Database,
    image_id: str,
    *,
    tenant: str = TENANT,
    received_at: datetime | None = None,
) -> None:
    context = TenantContext(tenant_id=tenant, subject_id="seed")
    with db.transaction(context) as session:
        session.add(
            NetworkSnapshotImageRecord(
                image_id=image_id,
                tenant_id=tenant,
                asset_id="asset-1",
                site_id="site-1",
                gateway_id="gw-1",
                incident_id="inc-1",
                codec="JPEG",
                width=1640,
                height=1232,
                captured_at_ms=int(NOW.timestamp() * 1000),
                size_bytes=len(PAYLOAD),
                content_sha256="0" * 64,
                object_key=_key_value(image_id, tenant),
                received_at=received_at or NOW,
            )
        )


def _exists(store: LocalDiskTenantObjectStore, image_id: str, tenant: str = TENANT) -> bool:
    path = store._root.joinpath(*_key_value(image_id, tenant).split("/"))
    return path.is_file()


def _sweep(db, store, **kwargs):
    return _run(SnapshotOrphanSweeper(db).sweep(TENANT_CTX, store, now=NOW, **kwargs))


# ===========================================================================
# 1. 时间窗：这是唯一真正危险的失效
# ===========================================================================


def test_fresh_object_without_a_row_is_not_an_orphan(db, store):
    """正在上传的图：对象刚落地、行还没提交。**绝不能删。**

    上传是「put → insert」，两者之间的窗口里对象没有行。删掉它之后上传方会
    插入一行指向不存在的对象——谎报行：声称证据已送达，取回 500，且没有任何
    一处会报错。
    """

    _write(store, "snap-in-flight", mtime=NOW - timedelta(seconds=5))

    plan = _sweep(db, store, apply=True)

    assert plan.scanned == 1
    assert plan.orphans == 0, f"刚写入的对象被当成孤儿了: {plan}"
    assert plan.deleted == 0
    assert _exists(store, "snap-in-flight"), "正在上传的对象被删掉了"


def test_object_just_outside_the_window_is_an_orphan(db, store):
    """时间窗之外、且没有行 → 真孤儿（进程崩在 put 与 insert 之间）。"""

    _write(store, "snap-crashed", mtime=NOW - timedelta(seconds=DEFAULT_MIN_AGE_SECONDS + 60))

    plan = _sweep(db, store, apply=True)

    assert plan.orphans == 1
    assert plan.deleted == 1
    assert not _exists(store, "snap-crashed")


def test_window_boundary_is_inclusive_of_older_only(db, store):
    """``last_modified`` 恰好等于 cutoff 时**算孤儿**。

    判据写成 ``last_modified > cutoff`` 跳过（即「等于边界算旧」）。反过来写
    （``>=`` 跳过）会让窗口比声称的多一秒，但那不是安全性问题；而写成
    「全部跳过」会让清扫器永远删不掉任何东西——那个失效是静默的：每轮都报
    scanned>0、orphans=0，磁盘一直涨。
    """

    _write(store, "snap-exactly", mtime=NOW - timedelta(seconds=DEFAULT_MIN_AGE_SECONDS))

    plan = _sweep(db, store, apply=True, min_age_seconds=DEFAULT_MIN_AGE_SECONDS)

    assert plan.orphans == 1
    assert plan.deleted == 1


def test_zero_window_is_refused(db, store):
    """窗口 0 = 删掉所有正在上传的图。必须拒绝，且拒绝发生在删除之前。"""

    _write(store, "snap-recent", mtime=NOW - timedelta(seconds=1))

    with pytest.raises(ValueError, match="min_age_seconds"):
        _sweep(db, store, apply=True, min_age_seconds=0)

    assert _exists(store, "snap-recent"), "拒绝必须发生在任何删除之前"


def test_custom_window_is_honoured(db, store):
    """窗口可调：把 10 分钟前的对象放进 5 分钟窗口才算孤儿。"""

    _write(store, "snap-10min", mtime=NOW - timedelta(minutes=10))

    inside = _sweep(db, store, min_age_seconds=1800)
    assert inside.orphans == 0, "30 分钟窗口里，10 分钟前的对象不算孤儿"

    outside = _sweep(db, store, min_age_seconds=300)
    assert outside.orphans == 1


# ===========================================================================
# 2. 有行的对象永远不是孤儿（无论多老）
# ===========================================================================


def test_object_with_a_row_is_never_swept(db, store):
    """有行 = 库里承认了这张证据。它归保留期/配额管，不归清扫器管。"""

    _write(store, "snap-tracked", mtime=NOW - timedelta(days=400))
    _seed_row(db, "snap-tracked")

    plan = _sweep(db, store, apply=True)

    assert plan.scanned == 1
    assert plan.orphans == 0
    assert plan.deleted == 0
    assert _exists(store, "snap-tracked"), "有行的对象被清扫器删掉了"


def test_row_for_a_different_tenant_does_not_protect_our_object(db, store):
    """别的租户里的行不能保护本租户的孤儿。

    注意这条用例**测不出**「查询漏了 ``tenant_id`` 过滤」——``_seed_row`` 让
    ``object_key`` 跟着租户走，两个租户的键本来就不同，``IN`` 压根匹配不上。
    它证的是另一件同样必要的事：判据确实是按**键**匹配的，不是按
    ``image_id`` 之类的裸标识（若按后者匹配，别租户的同名图会让本租户的孤儿
    永远清不掉——静默的磁盘泄漏）。租户列本身由下一条用例单独守。
    """

    _write(store, "snap-shared-id", mtime=NOW - timedelta(days=400))
    _seed_row(db, "snap-shared-id", tenant=OTHER_TENANT)

    plan = _sweep(db, store, apply=True)

    assert plan.orphans == 1, "别租户的行把本租户的孤儿保护起来了"
    assert plan.deleted == 1


def test_a_row_is_attributed_by_its_tenant_column_not_by_its_key_string(db, store):
    """脏行：租户列是别人的，对象键却是本租户的。

    **这条才是「查询必须带 tenant_id 过滤」的可观测证据。** 只按 ``object_key``
    IN 匹配的话，脏行会把本租户一个**真孤儿**伪装成「有行」，于是永远清不掉
    ——静默的磁盘泄漏，任何地方都不报错。

    现实里这种行不该存在（``object_key`` 由租户推导），但「不该存在」不是
    「不会存在」：手工修库、迁移脚本写错列、跨租户撞号冲突处理出错都能造出来。
    清扫器的判据必须锚在租户列上，而不是靠「键里恰好带着租户」这个巧合。
    """

    _write(store, "snap-dirty", mtime=NOW - timedelta(days=400))
    with db.transaction(TenantContext(tenant_id=OTHER_TENANT, subject_id="seed")) as session:
        session.add(
            NetworkSnapshotImageRecord(
                image_id="snap-dirty",
                tenant_id=OTHER_TENANT,
                asset_id="asset-1",
                site_id="site-1",
                gateway_id="gw-1",
                incident_id="inc-1",
                codec="JPEG",
                width=1640,
                height=1232,
                captured_at_ms=int(NOW.timestamp() * 1000),
                size_bytes=len(PAYLOAD),
                content_sha256="0" * 64,
                # 键属于本租户，租户列却是别人的。
                object_key=_key_value("snap-dirty", TENANT),
                received_at=NOW,
            )
        )

    plan = _sweep(db, store, apply=True)

    assert plan.orphans == 1, "脏行（租户列是别租户）把本租户的真孤儿保护起来了"
    assert plan.deleted == 1
    assert not _exists(store, "snap-dirty")


# ===========================================================================
# 3. 枚举范围：只碰自己的命名空间
# ===========================================================================


def test_prefix_ends_with_a_slash():
    """结尾斜杠是唯一的命名空间边界。

    少了它，``snapshot-images-2/x`` 也以 ``snapshot-images`` 为前缀。
    """

    assert snapshot_images_prefix(TENANT_CTX) == (f"tenant/{TENANT}/quarantine/snapshot-images/")


def test_video_clip_chunks_are_never_touched(db, store):
    """视频分片在同层兄弟目录下。清扫器碰它一次就会删掉整段视频留证。"""

    old = NOW - timedelta(days=400)
    clip = store._root.joinpath(
        "tenant", TENANT, "quarantine", "video-clips", "clip-1", "chunk-00000"
    )
    clip.parent.mkdir(parents=True, exist_ok=True)
    clip.write_bytes(PAYLOAD)
    os.utime(clip, (old.timestamp(), old.timestamp()))

    plan = _sweep(db, store, apply=True)

    assert plan.scanned == 0, "视频分片被枚举进静帧前缀了"
    assert clip.is_file(), "视频留证被静帧清扫器删掉了"


def test_sibling_namespace_with_our_prefix_as_its_prefix_is_not_swept(db, store):
    """``snapshot-images-evil/`` 也以 ``snapshot-images`` 开头。

    这条用例是结尾斜杠的直接后果，单独列出来是因为它描述了攻击面：任何能写
    对象键的人在理论上都能造出这种目录（真实实现里键要过
    ``validate_boundary_identifier``，但运维手工误放也可能）。
    """

    old = NOW - timedelta(days=400)
    evil = store._root.joinpath("tenant", TENANT, "quarantine", "snapshot-images-evil", "x.jpg")
    evil.parent.mkdir(parents=True, exist_ok=True)
    evil.write_bytes(PAYLOAD)
    os.utime(evil, (old.timestamp(), old.timestamp()))

    plan = _sweep(db, store, apply=True)

    assert plan.scanned == 0
    assert evil.is_file()


def test_other_tenants_objects_are_not_enumerated(db, store):
    old = NOW - timedelta(days=400)
    _write(store, "snap-other", tenant=OTHER_TENANT, mtime=old)

    plan = _sweep(db, store, apply=True)

    assert plan.scanned == 0
    assert _exists(store, "snap-other", tenant=OTHER_TENANT)


def test_store_refuses_a_prefix_outside_the_active_tenant(db, store):
    """存储实现自己也要挡一次：清扫器拼错前缀时不能靠运气。"""

    with pytest.raises(ValueError, match="outside the active tenant"):
        _run(store.list_prefix(TENANT_CTX, f"tenant/{OTHER_TENANT}/quarantine/snapshot-images/"))


def test_store_refuses_a_prefix_without_a_trailing_slash(db, store):
    with pytest.raises(ValueError, match="end with"):
        _run(store.list_prefix(TENANT_CTX, f"tenant/{TENANT}/quarantine/snapshot-images"))


# ===========================================================================
# 4. dry-run 与 apply 的区别
# ===========================================================================


def test_dry_run_deletes_nothing_but_reports_the_count(db, store):
    """默认 dry-run。这是不可逆的删除，必须先能看清要删什么。"""

    _write(store, "snap-a", mtime=NOW - timedelta(days=400))
    _write(store, "snap-b", mtime=NOW - timedelta(days=400))

    plan = _sweep(db, store)  # apply 默认 False

    assert plan.orphans == 2
    assert plan.deleted == 0
    assert plan.remaining == 2, "dry-run 也确实还有 2 个待处理"
    assert _exists(store, "snap-a")
    assert _exists(store, "snap-b")


# ===========================================================================
# 5. 分批与剩余量
# ===========================================================================


def test_single_run_is_capped_and_reports_remaining(db, store):
    """单轮上限：不让一次清扫锁住存储太久。剩余量必须如实报。"""

    for i in range(5):
        _write(store, f"snap-{i}", mtime=NOW - timedelta(days=400))

    plan = _sweep(db, store, apply=True, max_objects=2)

    assert plan.orphans == 5
    assert plan.deleted == 2
    assert plan.remaining == 3, "剩余量算错的话下一轮不知道该继续"
    assert plan.still_orphaned


def test_fully_swept_reports_nothing_remaining(db, store):
    _write(store, "snap-only", mtime=NOW - timedelta(days=400))

    plan = _sweep(db, store, apply=True)

    assert plan.remaining == 0
    assert not plan.still_orphaned


def test_max_objects_out_of_range_is_refused(db, store):
    for bad in (0, MAX_SWEEP_OBJECTS + 1):
        with pytest.raises(ValueError, match="max_objects"):
            _sweep(db, store, apply=True, max_objects=bad)


# ===========================================================================
# 6. 删除失败
# ===========================================================================


def test_delete_failure_is_counted_and_kept_for_the_next_run(db, store, monkeypatch):
    """删不掉必须计数并计入 remaining。

    静默吞掉会让报告说「已清空」而磁盘一字节没少——运维据此认为问题解决了。
    """

    _write(store, "snap-stuck", mtime=NOW - timedelta(days=400))

    async def _boom(context, key):
        raise OSError("simulated unlink failure")

    monkeypatch.setattr(store, "delete", _boom)

    plan = _sweep(db, store, apply=True)

    assert plan.orphans == 1
    assert plan.deleted == 0
    assert plan.delete_failures == 1
    assert plan.remaining == 1, "删失败的对象下一轮还会被枚举到，必须算剩余"
    assert _exists(store, "snap-stuck")


def test_one_failure_does_not_stop_the_rest(db, store, monkeypatch):
    """一个删不掉不该中断整轮。否则一个坏文件会永久卡住整个清扫。"""

    for i in range(3):
        _write(store, f"snap-{i}", mtime=NOW - timedelta(days=400))

    real_delete = store.delete

    async def _selective(context, key):
        if key.value.endswith("snap-1"):
            raise OSError("simulated failure")
        await real_delete(context, key)

    monkeypatch.setattr(store, "delete", _selective)

    plan = _sweep(db, store, apply=True)

    assert plan.deleted == 2, "一个失败把整轮清扫都挡住了"
    assert plan.delete_failures == 1
    assert _exists(store, "snap-1")
    assert not _exists(store, "snap-0")
    assert not _exists(store, "snap-2")


# ===========================================================================
# 7. 空与非空
# ===========================================================================


def test_empty_namespace_is_a_normal_result(db, store):
    plan = _sweep(db, store, apply=True)

    assert plan.scanned == 0
    assert plan.orphans == 0
    assert plan.deleted == 0
    assert not plan.still_orphaned


def test_prefix_directory_that_does_not_exist_is_not_an_error(db, tmp_path):
    """租户还一张图都没传过时目录不存在。这不是错误。"""

    fresh = LocalDiskTenantObjectStore(tmp_path / "empty")

    assert _run(fresh.list_prefix(TENANT_CTX, snapshot_images_prefix(TENANT_CTX))) == []


# ===========================================================================
# 8. 与内存实现的一致性
# ===========================================================================


def test_memory_store_lists_by_prefix_and_keeps_first_seen(db):
    """内存实现用于测试与隔离，它的枚举语义必须与本地盘一致。

    尤其是「首次写入时刻」：静帧键是幂等键，重复上传会覆盖同一份字节，但那
    不是新对象。若年龄按最后一次覆盖算，一张被反复重传的孤儿会永远显得刚写完。
    """

    store = InMemoryTenantObjectStore()

    async def scenario():
        from industrial_ops_agent.object_store.keys import (
            edge_snapshot_image_object_key,
        )

        await store.put(TENANT_CTX, edge_snapshot_image_object_key(TENANT_CTX, "snap-1"), PAYLOAD)
        first = (await store.list_prefix(TENANT_CTX, snapshot_images_prefix(TENANT_CTX)))[0][1]
        await store.put(TENANT_CTX, edge_snapshot_image_object_key(TENANT_CTX, "snap-1"), PAYLOAD)
        second = (await store.list_prefix(TENANT_CTX, snapshot_images_prefix(TENANT_CTX)))[0][1]
        listed = await store.list_prefix(TENANT_CTX, snapshot_images_prefix(TENANT_CTX))
        return first, second, listed

    first, second, listed = _run(scenario())

    assert len(listed) == 1
    assert listed[0][0] == _key_value("snap-1")
    assert second == first, "重传覆盖了对象的首次写入时刻 → 年龄会永远归零"


def test_memory_store_rejects_a_foreign_prefix():
    store = InMemoryTenantObjectStore()

    with pytest.raises(ValueError, match="outside the active tenant"):
        _run(store.list_prefix(TENANT_CTX, f"tenant/{OTHER_TENANT}/quarantine/snapshot-images/"))

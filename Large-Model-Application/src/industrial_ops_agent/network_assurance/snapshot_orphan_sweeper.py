"""清扫「有对象、没行」的孤儿静帧对象。

## 为什么需要它

写入顺序是「先写对象，再写行」。这个顺序保证了**不会**出现谎报行，但代价是
崩溃（进程被杀、断电、库连接断）会留下**孤儿对象**：对象在存储里，库里没有任何
一行指向它。

孤儿对象不会说谎，但会做三件坏事：

1. **占空间**，而且是永久占——保留期与配额都只处理**有行**的数据，它们看不见
   没有行的对象。磁盘占用会单方向增长，而每一张都不是「过期了」，是「从没被
   承认过」。
2. **按前缀枚举时会出现**。运维用 ``ls`` 列某个租户的证据会看到它们，而库里
   查不到任何对应记录——这本身就是一次难解释的取证困惑。
3. **与「已经删了但删失败」的孤儿混在一起**。后者是 ``delete_snapshot_objects``
   吞掉失败留下的（行已删、对象还在）。两类孤儿在这里被同一个判据回收，因为
   对存储来说它们没有区别：**库里没有行**。

## 为什么判据必须带时间窗，而不是「没有行就删」

这是本模块**唯一**真正危险的地方。两次写入之间有一个时间窗口：

    写入方：  put(对象) ──────────────► insert(行)
                          ↑
                     清扫器在这里扫一遍

如果清扫器在这个窗口内枚举对象，那张**正在上传的图就是一个合法的孤儿**：
对象刚落地、行还没提交。此时删掉它，上传方随后插入一行指向一个不存在的对象
——正好造出整条链路一直在防的**谎报行**，而且取回时 500。

所以判据不能是「库里没有行」，必须是「库里没有行**且**对象足够老」。窗口长度
（``min_age_seconds``）必须显著大于任何一次上传的最长可能耗时：上传是「签名校验
→ put → insert」，实测量级是毫秒到秒；默认 1 小时，留了三个数量级余量。

这与保留期回收器不冲突：回收器处理的是**有行**的过期数据，它根本看不见孤儿；
两者删的是不相交的集合。

## 为什么按「对象键」而不是「对象内容」判断

孤儿的定义是「库里没有一行指向这个键」。按内容判断（比如重算 sha256 再找）会
把「内容相同但键不同」的两张图混为一谈，而键是唯一的幂等标识。键也不参与
签名，所以不需要重新验签——**清扫不是接收，它不做信任判断**，它只删已经被
自己确认不存在的引用。

## 不做的事

- **不删「有行但对象不存在」的反向孤儿**。那是谎报行，属于**数据完整性事故**，
  该报警、该人工介入，绝不该由清扫器「顺手删掉对齐」。删了就把「证据丢了」
  伪装成「从来没有过这张证据」——比留着那行更糟。
- **不删别的命名空间**。只枚举 ``tenant/<tid>/quarantine/snapshot-images/``。
  视频分片在同层的 ``video-clips/`` 下，按前缀枚举会把它整片当孤儿删掉；键的
  中间段不同正是为了防止这件事（见 ``keys.edge_snapshot_image_object_key``）。
- **不在请求路径上跑**。枚举前缀是 O(对象数)，和上传抢的是同一个存储。
"""

from __future__ import annotations

import logging
from dataclasses import dataclass
from datetime import UTC, datetime, timedelta
from typing import Protocol

from sqlalchemy import select

from industrial_ops_agent.object_store.keys import (
    ObjectZone,
    TenantObjectKey,
    tenant_object_key_from_value,
)
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import NetworkSnapshotImageRecord
from industrial_ops_agent.persistence.tenant import TenantContext

LOGGER = logging.getLogger("industrial_ops_agent.snapshot_orphan_sweeper")

#: 单轮最多删多少个孤儿对象。与回收器的分批同理：不让单次执行锁住存储太久。
#: 删不完的下一轮接着删，`still_orphaned` 会如实报出剩余量。
MAX_SWEEP_OBJECTS = 1000

#: 对象必须比这个时间更老才会被当作孤儿。默认 1 小时。
#:
#: **这不是可省略的保护。** 上传是「put → insert」，两者之间的窗口里对象没有行；
#: 清扫器若在这个窗口内扫到它，就会删掉一张正在上传的图，然后上传方插入一行
#: 指向不存在的对象——正是整条链路在防的谎报行。1 小时比任何一次上传的耗时
#: 大三个数量级，留足了余量。
DEFAULT_MIN_AGE_SECONDS = 3600


class SupportsEnumerate(Protocol):
    """清扫需要的两个能力：按前缀枚举 + 删除。

    不写成完整的存储协议：清扫器不 put、不 get。但它**确实**要删，所以
    ``delete`` 写在协议里而不是靠 ``getattr`` 兜底——一个「协议里没有却偷偷
    调用」的方法，会在换了实现的那天才炸，而那时炸的是删证据的路径。
    """

    async def list_prefix(
        self,
        context: TenantContext,
        prefix: str,
    ) -> list[tuple[str, datetime]]:
        """返回 ``[(对象键, 最后修改时刻)]``。

        签名带 ``context`` 只为了一致性（实现可据此再挡一次跨租户枚举）；
        租户边界的权威判定在本模块里用 ``tenant_object_key_from_value``
        再做一次——前缀拼错会让清扫器枚举到别的租户并删掉它的证据。
        """
        ...

    async def delete(self, context: TenantContext, key: TenantObjectKey) -> None: ...


@dataclass(frozen=True, slots=True)
class SnapshotOrphanSweepPlan:
    """一次孤儿清扫的结果。dry-run 与真删共用同一个形状。"""

    #: 枚举到的快照对象总数（在时间窗之外的才可能被删）
    scanned: int
    #: 判定为孤儿（无行 + 足够老）的数量；``apply=False`` 时即「将会删除」
    orphans: int
    #: 实际删除的对象数（dry-run 恒为 0）
    deleted: int
    #: 删除失败的数量（留在存储里，下一轮会再看到）
    delete_failures: int
    #: 本次调用结束后仍在时间窗之外、且仍无行的数量；> 0 表示需要再跑一轮
    remaining: int
    #: 时间窗边界：比它更老的对象才会被考虑
    cutoff: datetime

    @property
    def still_orphaned(self) -> bool:
        return self.remaining > 0


def snapshot_images_prefix(context: TenantContext) -> str:
    """本租户的静帧对象前缀。**必须以 ``/`` 结尾**。

    不结尾会匹到 ``snapshot-images-evil/...``——前缀匹配是字符串匹配，
    ``tenant/t/quarantine/snapshot-images`` 是 ``tenant/t/quarantine/snapshot-images-2/x``
    的前缀。结尾斜杠是这里唯一的边界。
    """

    return f"tenant/{context.tenant_id}/{ObjectZone.QUARANTINE.value}/snapshot-images/"


class SnapshotOrphanSweeper:
    """按前缀枚举快照对象，删掉「库里没有行且足够老」的那些。"""

    def __init__(self, database: Database) -> None:
        self._database = database

    async def sweep(
        self,
        context: TenantContext,
        object_store: SupportsEnumerate,
        *,
        apply: bool = False,
        min_age_seconds: int = DEFAULT_MIN_AGE_SECONDS,
        max_objects: int = MAX_SWEEP_OBJECTS,
        now: datetime | None = None,
    ) -> SnapshotOrphanSweepPlan:
        """清扫孤儿对象。

        ``apply`` 默认 False，与保留期回收器同一个理由：这是不可逆的删除，
        必须先能看清要删什么。
        """

        if min_age_seconds < 1:
            raise ValueError(
                "min_age_seconds must be at least 1; a zero window would delete "
                "objects that are still being uploaded (put done, row not yet committed)"
            )
        if not 1 <= max_objects <= MAX_SWEEP_OBJECTS:
            raise ValueError(f"max_objects must be between 1 and {MAX_SWEEP_OBJECTS}")

        reference = now or datetime.now(UTC)
        cutoff = reference - timedelta(seconds=min_age_seconds)
        prefix = snapshot_images_prefix(context)

        listed = await object_store.list_prefix(context, prefix)
        LOGGER.info(
            "snapshot orphan sweep begin: tenant=%s prefix=%s listed=%d cutoff=%s apply=%s",
            context.tenant_id,
            prefix,
            len(listed),
            cutoff.isoformat(),
            apply,
        )

        # 时间窗内的对象一律跳过，且**不查库**：它们可能正在被上传，此刻没有行
        # 是正常的。把它们算进 remaining 会让每一轮都报「还有剩余」，而那只是
        # 正在进行的上传。
        candidates: list[tuple[str, datetime]] = []
        in_flight = 0
        for raw_value, last_modified in listed:
            if last_modified > cutoff:
                in_flight += 1
                continue
            candidates.append((raw_value, last_modified))

        # 只对时间窗之外的对象查「有没有行」。一次 IN 查询，不逐条查。
        orphan_values = self._without_rows(context, candidates)
        orphans = len(orphan_values)
        LOGGER.info(
            "snapshot orphan sweep scanned: tenant=%s scanned=%d in_flight=%d "
            "candidates=%d orphans=%d",
            context.tenant_id,
            len(listed),
            in_flight,
            len(candidates),
            orphans,
        )

        if not apply or orphans == 0:
            return SnapshotOrphanSweepPlan(
                scanned=len(listed),
                orphans=orphans,
                deleted=0,
                delete_failures=0,
                remaining=orphans,
                cutoff=cutoff,
            )

        victims = orphan_values[:max_objects]
        deleted, failures = await self._delete_objects(context, object_store, victims)

        # 用「还没被删掉的」重算剩余：删失败的那些下一轮还会被枚举到，
        # 所以它们算剩余。
        remaining = orphans - deleted
        LOGGER.info(
            "snapshot orphan sweep done: tenant=%s deleted=%d failures=%d remaining=%d",
            context.tenant_id,
            deleted,
            failures,
            remaining,
        )
        return SnapshotOrphanSweepPlan(
            scanned=len(listed),
            orphans=orphans,
            deleted=deleted,
            delete_failures=failures,
            remaining=remaining,
            cutoff=cutoff,
        )

    # ------------------------------------------------------------------
    # 查询与删除
    # ------------------------------------------------------------------

    def _without_rows(
        self,
        context: TenantContext,
        candidates: list[tuple[str, datetime]],
    ) -> list[str]:
        """从候选键里挑出库里没有对应行的那些。

        分批查 IN，避免 ``max_objects`` 很大时撑爆参数上限。分片大小取 500：
        Postgres 的参数上限是 65535，SQLite 老版本是 999，500 在两者之下。
        """

        if not candidates:
            return []

        values = [raw_value for raw_value, _ in candidates]
        existing: set[str] = set()
        chunk = 500
        for start in range(0, len(values), chunk):
            batch = values[start : start + chunk]
            with self._database.transaction(context) as session:
                stmt = select(NetworkSnapshotImageRecord.object_key).where(
                    NetworkSnapshotImageRecord.tenant_id == context.tenant_id,
                    NetworkSnapshotImageRecord.object_key.in_(batch),
                )
                existing.update(session.scalars(stmt).all())

        return [value for value in values if value not in existing]

    async def _delete_objects(
        self,
        context: TenantContext,
        object_store: SupportsEnumerate,
        object_values: list[str],
    ) -> tuple[int, int]:
        """删除对象，返回 ``(deleted, failures)``。

        这里**不**走 ``delete_snapshot_rows_and_objects``：那个入口的语义是
        「先删行、再删对象」，而孤儿对象的定义就是**没有行**。硬套它只会删
        0 行再删对象，多一次无意义的事务；更重要的是，把「清扫」混进那条
        「顺序只写一次」的路径会让那段代码的注释失真。

        每个对象独立 try：一个删不掉不该中断整轮。
        """

        deleted = 0
        failures = 0
        for raw_value in object_values:
            try:
                # 重新构造键而不是直接拿字符串：这会强制检查前缀确实属于本
                # 租户（``tenant_object_key_from_value`` 的边界校验）。枚举
                # 前缀写错时，这一步会拦下别的租户的对象。
                key: TenantObjectKey = tenant_object_key_from_value(context, raw_value)
                await object_store.delete(context, key)
            except Exception:  # noqa: BLE001 - 单个对象失败不该中断整轮清扫
                failures += 1
                LOGGER.warning(
                    "snapshot orphan object delete failed: key=%s",
                    raw_value,
                    exc_info=True,
                )
                continue
            deleted += 1
        return deleted, failures

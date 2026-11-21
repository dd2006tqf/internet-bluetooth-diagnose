"""单租户静帧总量上限：写完之后淘汰最旧的，直到装得下。

## 为什么保留期不够

``snapshot_retention.py`` 约束的是「**多久以后删**」。它回答不了另一个问题：
「**写得多快**」。一次采集风暴（板端抖动、事件重复触发、回放工具误跑）可以在
一个保留期内写入远超磁盘容量的静帧，而回收只在过期之后才动手——也就是说，
磁盘会在证据还没到期的时候先满。``put`` 抛 ``OSError``，上传端看到 500，
运营端看到「这次事故没有图」，没有任何一处说「磁盘满了」。

保留期与配额是两个正交的旋钮：

| 旋钮 | 回答的问题 | 失效方式 |
| --- | --- | --- |
| 保留期 | 多久以后删 | 短期写入风暴 → 磁盘先满 |
| 配额 | 最多占多少 | 没有它 → 单调增长到满 |

## 为什么淘汰最旧的，而不是拒绝新的

拒绝新写入看起来更安全（不动已有证据），但它把失败写进了证据链：**这次**
事故没有图，而这次事故往往正是最该留下的那一次。删最旧的至少保留时间上
最近、也最可能正在被查看的那一批。

## 为什么在写成功**之后**才淘汰，而不是之前

先淘汰再写，如果写失败（存储故障），就是「旧的删了、新的没存下」——净损失，
而且没有任何收益。写成功再淘汰，最坏情况是短暂超出上限一张图（≤ 640 KB），
而配额本来就不是磁盘容量，是软上限。超一点远好过白删。

## 为什么永远不淘汰刚刚写进来的那一张

配额可能比一张图还小（配置失误，或者有人把配额调到了 1 KB）。此时「淘汰到
装得下」的唯一解法是立刻删掉刚存进来的图——那等于「接受上传然后当场销毁」，
设备侧看到 200 而图不见了，比直接报错更难排查。所以这里显式排除
``keep_image_id``，并在装不下时**告警而不是继续删**。

## 为什么失败只告警、不影响上传结果

图已经写进对象存储、行也已经登记了。这时候让请求失败，板端会重试，而重试
会撞上 ``image_id`` 冲突被拒——把一次「配额没算干净」放大成「这张证据永远
存不进去」。所以淘汰失败只记 ERROR 日志，下一张图、下一次保留期回收会接着
收拾。
"""

from __future__ import annotations

import logging
from dataclasses import dataclass

from sqlalchemy import func, select

from industrial_ops_agent.network_assurance.snapshot_deletion import (
    delete_snapshot_rows_and_objects,
)
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import NetworkSnapshotImageRecord
from industrial_ops_agent.persistence.tenant import TenantContext

LOGGER = logging.getLogger("industrial_ops_agent.snapshot_quota")

#: 单次淘汰最多处理多少行。配额通常只需要淘汰 1~2 张，这个上限只是防止
#: 「配额被调得很小」时一次上传就锁住大半个表。删不完的留给下一次上传与
#: 保留期回收，不需要一次删完。
MAX_EVICTION_ROWS = 1000


@dataclass(frozen=True, slots=True)
class SnapshotQuotaPlan:
    """一次配额执行的结果。"""

    #: 本次生效的上限（字节）
    quota_bytes: int
    #: 执行前的总量（含刚写入的那张）
    total_bytes_before: int
    #: 执行后的总量。> ``quota_bytes`` 表示仍超出（被 ``keep_image_id`` 卡住
    #: 或单批淘汰上限用尽）
    total_bytes_after: int
    #: 实际删除的行数
    evicted: int
    #: 这些行对应的字节数
    evicted_bytes: int
    #: 对象删除失败的数量（行已删，留下孤儿对象）
    orphaned_objects: int

    @property
    def over_quota(self) -> bool:
        return self.total_bytes_after > self.quota_bytes


class SnapshotQuotaService:
    """把单租户静帧总量压回上限之内。"""

    def __init__(self, database: Database) -> None:
        self._database = database

    def total_bytes(self, context: TenantContext) -> int:
        with self._database.transaction(context) as session:
            return int(
                session.scalar(
                    select(func.coalesce(func.sum(NetworkSnapshotImageRecord.size_bytes), 0)).where(
                        NetworkSnapshotImageRecord.tenant_id == context.tenant_id
                    )
                )
                or 0
            )

    async def enforce(
        self,
        context: TenantContext,
        object_store,
        *,
        quota_bytes: int,
        keep_image_id: str | None = None,
        reason: str = "quota",
    ) -> SnapshotQuotaPlan:
        """淘汰最旧的静帧，直到总量回到 ``quota_bytes`` 以内。

        ``keep_image_id`` 是刚写入的那一张，**任何情况下都不淘汰它**（理由见
        模块头）。它同时也是「谁是新的」的判据：其余行按 ``received_at``
        升序淘汰。

        注意这里用 ``received_at`` 而不是 ``captured_at_ms`` 排序，与保留期
        同一个理由：后者是板端自述的墙钟，一块没对过 NTP 的板子可以报出
        1970 年，于是「最旧的」会变成「时钟最不准的」。
        """

        if quota_bytes < 1:
            raise ValueError("quota_bytes must be at least 1")

        total = self.total_bytes(context)
        if total <= quota_bytes:
            return SnapshotQuotaPlan(
                quota_bytes=quota_bytes,
                total_bytes_before=total,
                total_bytes_after=total,
                evicted=0,
                evicted_bytes=0,
                orphaned_objects=0,
            )

        rows = self._oldest_rows(context, keep_image_id=keep_image_id)
        if not rows:
            # 一张都淘汰不了，而总量已经超了：只可能是 keep_image_id 自己
            # 就比配额大。这不是「配额暂时生效不了」，是配额配错了，必须
            # 让人看见——静默放行会让它每张图都超，而每一张都看起来正常。
            LOGGER.warning(
                "snapshot quota exceeded with nothing evictable: "
                "tenant=%s quota=%d total=%d keep=%s "
                "(quota is smaller than a single image, or this tenant has only that image)",
                context.tenant_id,
                quota_bytes,
                total,
                keep_image_id,
            )
            return SnapshotQuotaPlan(
                quota_bytes=quota_bytes,
                total_bytes_before=total,
                total_bytes_after=total,
                evicted=0,
                evicted_bytes=0,
                orphaned_objects=0,
            )

        # 从最旧的开始累加，直到「剩下的」装得下。
        victims: list[NetworkSnapshotImageRecord] = []
        reclaimed = 0
        for row in rows:
            if total - reclaimed <= quota_bytes:
                break
            victims.append(row)
            reclaimed += row.size_bytes

        if not victims:
            return SnapshotQuotaPlan(
                quota_bytes=quota_bytes,
                total_bytes_before=total,
                total_bytes_after=total,
                evicted=0,
                evicted_bytes=0,
                orphaned_objects=0,
            )

        deleted, orphaned = await delete_snapshot_rows_and_objects(
            self._database,
            context,
            object_store,
            victims,
            reason=reason,
        )

        after = self.total_bytes(context)
        plan = SnapshotQuotaPlan(
            quota_bytes=quota_bytes,
            total_bytes_before=total,
            total_bytes_after=after,
            evicted=deleted,
            evicted_bytes=reclaimed,
            orphaned_objects=orphaned,
        )
        if plan.over_quota:
            # 没压回去只有两种原因：单批上限用尽，或者配额比一张图还小。
            # 无论哪种都不该在请求路径上继续循环——下一次上传与保留期
            # 回收会接着处理，而这条日志是唯一能让人发现「配额一直不达标」
            # 的地方（配额不达标本身不会让任何一次上传失败）。
            LOGGER.warning(
                "snapshot quota still exceeded after eviction: "
                "tenant=%s quota=%d after=%d evicted=%d",
                context.tenant_id,
                quota_bytes,
                after,
                deleted,
            )
        return plan

    def _oldest_rows(
        self,
        context: TenantContext,
        *,
        keep_image_id: str | None,
    ) -> list[NetworkSnapshotImageRecord]:
        with self._database.transaction(context) as session:
            stmt = (
                select(NetworkSnapshotImageRecord)
                .where(NetworkSnapshotImageRecord.tenant_id == context.tenant_id)
                .order_by(NetworkSnapshotImageRecord.received_at.asc())
                .limit(MAX_EVICTION_ROWS)
            )
            if keep_image_id is not None:
                stmt = stmt.where(NetworkSnapshotImageRecord.image_id != keep_image_id)
            return list(session.scalars(stmt).all())

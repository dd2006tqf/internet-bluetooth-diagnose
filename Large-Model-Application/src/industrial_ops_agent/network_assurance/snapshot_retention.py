"""事故静帧的保留策略：按时间回收，默认只算不删。

## 为什么需要它

落盘对象存储会让磁盘占用单调增长，而静帧是低频但永续的流：一次事故一张，
一年就是几万张、几个 GB。没有回收，存储目录迟早写满——而写满的失败方式很
隐蔽：``put`` 抛 ``OSError``，上传端看到 500，运营端看到「这次事故没有图」，
没有任何一处说「磁盘满了」。

## 为什么默认 dry-run

这是**不可逆的证据销毁**。最糟的失败模式不是「删早了」，而是「删了但谁也
说不清删了什么」：时钟跳变、配置写错一位、误把开发库指向生产环境，事后都
无法复盘。所以 ``run()`` 默认 ``apply=False`` 只做计算，真删必须显式传
``apply=True``。这与仓库里 ``weaknet-ota-ctl reconcile --dry-run`` 是同一个
思路：先能看清要做什么，再允许它做。

## 删除顺序与写入顺序**相反**，这不是笔误

写入是「先写对象，再写行」，因为要避免「行存在但内容不存在」。
删除则必须「**先删行，再删对象**」，理由是对称的：

| 顺序 | 崩溃在中间时剩下什么 | 后果 |
| --- | --- | --- |
| 先删对象、再删行 | 行在，对象没了 | **谎报行**：声称证据已送达，取回 500。不可观测 |
| 先删行、再删对象 | 对象在，行没了 | 孤儿对象：浪费磁盘，可回收，不会说谎 |

两边的共同不变量只有一句：**行是对「证据存在」的声明，绝不让声明比证据活得久。**

孤儿对象不是零成本（占空间、按前缀枚举时会出现），但它可被发现、可被后续
按前缀清扫回收；而谎报行会把「丢了」伪装成「送达了」，那正是整个静帧链路
一直在防的事。所以这里宁可留下孤儿对象。

## 为什么判据是 ``received_at`` 而不是 ``captured_at_ms``

后者是板端自述的墙钟。一块没对过 NTP 的板子可以报出 1970 年或 2099 年。
拿它当判据意味着「板子时钟错了 → 证据被误删」。``received_at`` 由服务端写入，
板端改不了。

## 不做的事

- **不删事故**。事故是时间线的骨架，删了会让剩下的事实无处挂靠。静帧是事故
  的附件，附件可以过期，骨架不行。
- **不按「事故是否已关闭」筛选**。那会让回收依赖另一张表的状态，而那个状态
  可能永远不推进（没人手动关闭事故），于是磁盘照样写满。
- **不在库里记删除流水**。这一版只把删除判决与结果写进结构化日志，不新增
  持久化表（新增表意味着一次迁移与一个新的消费者，与「最小可验证闭环」不符）。
  代价是：日志是最佳努力，不构成可对账的账本。要真正做证据销毁审计时，
  应升级为独立的审计表——**那时再加，而不是现在顺手挖一个空表**。
- **不清扫「有对象、没行」的孤儿对象**。本服务只处理**有行**的过期数据；
  历史崩溃留下的孤儿对象没有行，它看不见。上面说孤儿对象「可被后续按前缀
  清扫回收」，但**目前并不存在那个清扫器**——不要把它当成已有的兜底。
  真要做时它必须按 `tenant/<tid>/.../snapshot-images/` 前缀枚举，再与库里的
  `object_key` 做差集；而「枚举 + 差集」本身有竞态（枚举到的对象可能刚被
  一次上传写入、行还没提交），所以它需要比本服务更严的时间窗。
- **不做配额上限**。保留期约束「多久以后删」，约束不了「写得多快」。一次
  采集风暴可以在一个保留期内把磁盘写满，而本服务只在过期之后才动手。
"""

from __future__ import annotations

import logging
from dataclasses import dataclass
from datetime import UTC, datetime, timedelta

from sqlalchemy import func, select

from industrial_ops_agent.network_assurance.snapshot_deletion import (
    delete_snapshot_rows_and_objects,
)
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import NetworkSnapshotImageRecord
from industrial_ops_agent.persistence.tenant import TenantContext

LOGGER = logging.getLogger("industrial_ops_agent.snapshot_retention")

#: 单批上限。再大就会让单个事务锁表过久；回收不抢时间。
MAX_BATCH_SIZE = 1000


class SnapshotRetentionRefused(RuntimeError):
    """保留策略拒绝执行，因为参数会让它做出不该做的事。

    与「没有可删的」刻意分开：后者是正常结果，前者是配置错误。混为一谈会让
    一个把 ``retention_days`` 填成 0 的部署看起来「运行正常、只是没有数据」。
    """


@dataclass(frozen=True, slots=True)
class SnapshotRetentionPlan:
    """一次保留策略执行的结果。dry-run 与真删共用同一个形状。"""

    #: 判定为过期的总数（``apply=False`` 时即「将会删除」的数量）
    expired: int
    #: 实际删除的行数（dry-run 恒为 0）
    deleted: int
    #: 对象删除失败的数量。这些行**已经被删**，留下的是孤儿对象
    orphaned_objects: int
    #: 本次调用结束后仍超过保留期的数量；> 0 表示需要再跑一轮
    remaining: int
    #: 过期判据的截止时刻（``received_at < cutoff`` 即过期）
    cutoff: datetime


class SnapshotRetentionService:
    """按时间回收事故静帧（行 + 对象）。"""

    def __init__(self, database: Database) -> None:
        self._database = database

    async def run(
        self,
        context: TenantContext,
        object_store,
        *,
        retention_days: int,
        apply: bool = False,
        batch_size: int = 200,
        now: datetime | None = None,
    ) -> SnapshotRetentionPlan:
        """回收超过保留期的静帧。

        ``object_store`` 是必填的，不是可选项：没有它就只该删行，而那会把
        **每一行**都变成孤儿对象。宁可拒绝运行，也不做半件事。
        """

        if retention_days < 1:
            raise SnapshotRetentionRefused(
                "retention_days must be at least 1; "
                "a non-positive window would delete every image immediately"
            )
        if not 1 <= batch_size <= MAX_BATCH_SIZE:
            raise SnapshotRetentionRefused(f"batch_size must be between 1 and {MAX_BATCH_SIZE}")

        cutoff = (now or datetime.now(UTC)) - timedelta(days=retention_days)
        expired_total = self._count_expired(context, cutoff)

        if not apply:
            LOGGER.info(
                "snapshot retention dry-run: tenant=%s cutoff=%s expired=%d",
                context.tenant_id,
                cutoff.isoformat(),
                expired_total,
            )
            return SnapshotRetentionPlan(
                expired=expired_total,
                deleted=0,
                orphaned_objects=0,
                remaining=expired_total,
                cutoff=cutoff,
            )

        rows = self._select_expired(context, cutoff, batch_size)
        if not rows:
            return SnapshotRetentionPlan(
                expired=expired_total,
                deleted=0,
                orphaned_objects=0,
                remaining=expired_total,
                cutoff=cutoff,
            )

        # 先把「准备删哪些」写进日志，再动手。中途崩溃时，日志里留下的是
        # 「打算删什么」而不是一个无法解释的缺口。
        LOGGER.info(
            "snapshot retention begin: tenant=%s cutoff=%s deleting=%d of %d expired",
            context.tenant_id,
            cutoff.isoformat(),
            len(rows),
            expired_total,
        )

        # 顺序只写一次：见 ``snapshot_deletion`` 模块头。任何需要删除静帧证据的
        # 路径（保留期、配额、将来的手工清理）都必须走这个入口，否则「先删行
        # 再删对象」这条不变量会出现第二份、可能写反的实现。
        deleted, orphaned = await delete_snapshot_rows_and_objects(
            self._database,
            context,
            object_store,
            rows,
            reason="retention",
        )

        remaining = self._count_expired(context, cutoff)
        LOGGER.info(
            "snapshot retention done: tenant=%s deleted=%d orphaned_objects=%d remaining=%d",
            context.tenant_id,
            deleted,
            orphaned,
            remaining,
        )
        return SnapshotRetentionPlan(
            expired=expired_total,
            deleted=deleted,
            orphaned_objects=orphaned,
            remaining=remaining,
            cutoff=cutoff,
        )

    # ------------------------------------------------------------------
    # 查询
    # ------------------------------------------------------------------

    def _expired_filter(self, context: TenantContext, cutoff: datetime):
        return (
            NetworkSnapshotImageRecord.tenant_id == context.tenant_id,
            NetworkSnapshotImageRecord.received_at < cutoff,
        )

    def _count_expired(self, context: TenantContext, cutoff: datetime) -> int:
        with self._database.transaction(context) as session:
            return (
                session.scalar(
                    select(func.count())
                    .select_from(NetworkSnapshotImageRecord)
                    .where(*self._expired_filter(context, cutoff))
                )
                or 0
            )

    def _select_expired(
        self, context: TenantContext, cutoff: datetime, batch_size: int
    ) -> list[NetworkSnapshotImageRecord]:
        with self._database.transaction(context) as session:
            stmt = (
                select(NetworkSnapshotImageRecord)
                .where(*self._expired_filter(context, cutoff))
                # 先过期的先删：多轮分批时保证单调推进，不会因为排序不稳定
                # 而在同一批上反复打转。
                .order_by(NetworkSnapshotImageRecord.received_at.asc())
                .limit(batch_size)
            )
            return list(session.scalars(stmt).all())

    # ------------------------------------------------------------------
    # 删除
    #
    # 这里**不实现**删除顺序。`_delete_rows` / `_delete_objects` 曾经在
    # 本文件中各有一份，而配额实现需要完全相同的一对。复制一份的代价不是
    # 代码量，是顺序可能被写反——而反着写不会报错（见模块头与
    # `snapshot_deletion` 模块说明）。现在唯一的入口是
    # `delete_snapshot_rows_and_objects()`。
    # ------------------------------------------------------------------

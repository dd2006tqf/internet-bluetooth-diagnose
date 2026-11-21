"""把「删哪些行 + 删哪些对象」这一步抽出来，供保留期与配额共用。

## 为什么必须抽出来，而不是各写一份

「**先删行、再删对象**」这条顺序是整条留证链路的对称不变量（写入是
对象→行，删除必须反过来）。它有一个非常讨厌的性质：**反着写也不会报错**，
两种顺序的最终状态完全一样（行没了、对象还在），因为对象删除失败是被吞掉
并计数的。

也就是说，如果配额实现时顺手复制了一段「先删对象」的代码，所有测试都会
是绿的，而崩在中间就会留下「行在、对象没了」的谎报行。详见
`openspec/pitfalls/delete-order-not-observable-from-final-state.md`。

所以这里不给第二份实现。任何需要删除静帧证据的路径都调用
`delete_snapshot_rows_and_objects()`，顺序只写一次。
"""

from __future__ import annotations

import logging
from typing import Protocol

from industrial_ops_agent.object_store.keys import tenant_object_key_from_value
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import NetworkSnapshotImageRecord
from industrial_ops_agent.persistence.tenant import TenantContext

LOGGER = logging.getLogger("industrial_ops_agent.snapshot_deletion")


class SupportsDelete(Protocol):
    """只需要删除能力的窄契约。

    不写成完整的存储协议：回收路径不 `put`、不 `get`，把它写成三方法协议
    只会让测试替身被迫实现用不到的方法。
    """

    async def delete(self, context: TenantContext, key) -> None: ...


def delete_snapshot_rows(
    database: Database,
    context: TenantContext,
    image_ids: list[str],
) -> int:
    """删行。**必须在删对象之前调用。**"""

    from sqlalchemy import delete

    with database.transaction(context) as session:
        result = session.execute(
            delete(NetworkSnapshotImageRecord).where(
                NetworkSnapshotImageRecord.tenant_id == context.tenant_id,
                NetworkSnapshotImageRecord.image_id.in_(image_ids),
            )
        )
        return int(result.rowcount or 0)


async def delete_snapshot_objects(
    context: TenantContext,
    object_store: SupportsDelete,
    object_keys: list[tuple[str, str]],
) -> int:
    """尽力删除对象，返回失败数。失败**不回滚行**。

    失败不回滚是有意的：行已经删了，回滚它只会得到一个指向缺失对象的行，
    也就是整个删除顺序设计在避免的谎报行。失败留成孤儿对象并计数报出。

    写成 ``async`` 而不是同步 + ``asyncio.run``：``asyncio.run`` 在已有事件
    循环的调用方里会直接报错——一个「在 FastAPI 上下文里调用就炸」的清理
    函数，早晚会被踩到。
    """

    failures = 0
    for image_id, raw_value in object_keys:
        try:
            key = tenant_object_key_from_value(context, raw_value)
            await object_store.delete(context, key)
        except Exception:  # noqa: BLE001 - 单个对象失败不该中断整轮回收
            failures += 1
            LOGGER.warning(
                "snapshot object delete failed: image_id=%s",
                image_id,
                exc_info=True,
            )
    return failures


async def delete_snapshot_rows_and_objects(
    database: Database,
    context: TenantContext,
    object_store: SupportsDelete,
    rows: list[NetworkSnapshotImageRecord],
    *,
    reason: str,
) -> tuple[int, int]:
    """唯一的删除入口：**先删行、后删对象**。返回 ``(deleted_rows, orphaned)``。

    ``reason`` 只进日志，用于事后区分「这批是保留期删的」还是「配额删的」。
    不落库：这一版刻意不新增删除流水表（新增表 = 一次迁移 + 一个新消费者）。
    """

    if not rows:
        return 0, 0

    image_ids = [row.image_id for row in rows]
    object_keys = [(row.image_id, row.object_key) for row in rows]

    # 先把「准备删哪些」写进日志，再动手。中途崩溃时，日志里留下的是
    # 「打算删什么」而不是一个无法解释的缺口。
    LOGGER.info(
        "snapshot deletion begin: reason=%s tenant=%s deleting=%d",
        reason,
        context.tenant_id,
        len(rows),
    )

    deleted = delete_snapshot_rows(database, context, image_ids)
    orphaned = await delete_snapshot_objects(context, object_store, object_keys)

    LOGGER.info(
        "snapshot deletion done: reason=%s tenant=%s deleted=%d orphaned_objects=%d",
        reason,
        context.tenant_id,
        deleted,
        orphaned,
    )
    return deleted, orphaned

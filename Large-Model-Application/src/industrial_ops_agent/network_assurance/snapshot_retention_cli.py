"""事故静帧保留策略的运维入口。

    industrial-ops-snapshot-retention --tenant-id <tid> --retention-days 90
    industrial-ops-snapshot-retention --tenant-id <tid> --retention-days 90 --apply

默认**只报告不删除**。要真删必须显式 `--apply`，这不是可省掉的仪式：
回收是不可逆的证据销毁，而最容易发生的误用不是「忘了删」，是「以为自己在
看报告，其实在删数据」。

对象存储用与 rehearsal 相同的环境变量 `REHEARSAL_OBJECT_STORE_DIR` 选择：

- 设了目录 → 落盘存储（与上传端同一种实现，能被真的删掉）；
- 没设 → 直接拒绝运行。

**不提供 InMemory 兜底**：那个实现里没有真实对象，用它跑回收会得到
「删了 0 个对象」的绿灯，而磁盘上的东西一个没少。一个会撒谎的回收器
比没有回收器危险得多，所以这里宁可报错退出。

## 为什么退出码要区分三种情况

| 退出码 | 含义 | 调度器的正确反应 |
| --- | --- | --- |
| 0 | 正常（含 dry-run 与「没有过期数据」） | 没事 |
| 1 | 参数被拒绝（保留期 ≤ 0 等，命令自己判的） | 别重试，改配置 |
| 2 | **用法错误**（`argparse` 自己抛的：缺必填项、参数不是整数） | 别重试，改配置 |
| 3 | **真删除后**仍有剩余（`--apply` 且 `remaining > 0`） | **可以再跑一轮** |

「还有剩余」用 3 而不是 2，是因为 **`argparse` 的用法错误本来就占用 2**。
少了 `--tenant-id` 时 `parse_args` 直接 `SystemExit(2)`，这个 2 根本走不到
`main()` 的返回值。若「还有剩余」也用 2，一个 `while rc == 2: 再跑一轮` 的
调度循环会在参数写错时**永远转下去**——而参数写错恰恰是第一次部署最常见的
问题。宁可多占一个数字，也不让两种完全相反的情况共用一个退出码。

**dry-run 恒返回 0，即使 `remaining > 0`。** 这不是漏判：dry-run 一张都不删，
它跑一百轮剩下的还是同样多，退出码 3 会让调度循环永远转下去。要表达
「还有多少」的字段是 stdout 里的 `remaining`，而它在两种模式下都照样输出。
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys

from industrial_ops_agent.config import get_settings
from industrial_ops_agent.network_assurance.snapshot_retention import (
    MAX_BATCH_SIZE,
    SnapshotRetentionRefused,
    SnapshotRetentionService,
)
from industrial_ops_agent.object_store.local_disk import LocalDiskTenantObjectStore
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.tenant import TenantContext
from industrial_ops_agent.secrets import SecretName, build_secret_provider

EXIT_OK = 0
EXIT_REFUSED = 1
# ``argparse`` 自己把用法错误固定成 2（缺必填项、参数不是整数）。它由
# ``parse_args`` 直接抛 ``SystemExit``，根本不经过 ``main()`` 的返回值，
# 所以这里连常量都不定义——定义了一个永不返回的数字只会让人以为能改。
# 正因如此，「还有剩余」必须避开 2，于是用 3（见模块头退出码表）。
EXIT_MORE_WORK = 3


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="industrial-ops-snapshot-retention",
        description="Reclaim expired incident snapshot images (rows + objects).",
    )
    parser.add_argument("--tenant-id", required=True)
    parser.add_argument(
        "--retention-days",
        required=True,
        type=int,
        help="images received longer ago than this are eligible for deletion",
    )
    parser.add_argument(
        "--apply",
        action="store_true",
        help="actually delete; without it this only reports what would go",
    )
    parser.add_argument(
        "--batch-size",
        type=int,
        default=200,
        help=f"max images per run (1..{MAX_BATCH_SIZE}); use remaining to loop",
    )
    parser.add_argument(
        "--subject-id",
        default="snapshot-retention",
        help="tenant subject recorded by the tenant guard",
    )
    return parser


def _object_store():
    directory = os.environ.get("REHEARSAL_OBJECT_STORE_DIR", "").strip()
    if not directory:
        print(
            "error: REHEARSAL_OBJECT_STORE_DIR is not set. Refusing to run: "
            "without a real object store this would delete rows while leaving "
            "every object behind as an orphan, and report success.",
            file=sys.stderr,
        )
        raise SystemExit(EXIT_REFUSED)
    return LocalDiskTenantObjectStore(directory)


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)

    # 先做最便宜的那个拒绝：没有对象存储时连数据库都不该碰。否则同一个
    # 「少配了一个环境变量」会先被报成数据库连不上，排查方向从一开始就错。
    store = _object_store()

    settings = get_settings()
    secrets = build_secret_provider(settings)
    database = Database(secrets.get(SecretName.DATABASE_URL).reveal())
    context = TenantContext(tenant_id=args.tenant_id, subject_id=args.subject_id)
    service = SnapshotRetentionService(database)

    try:
        plan = asyncio.run(
            service.run(
                context,
                store,
                retention_days=args.retention_days,
                apply=args.apply,
                batch_size=args.batch_size,
            )
        )
    except SnapshotRetentionRefused as exc:
        # 参数错误不是「暂时做不了」，重试一万次也一样。用不同退出码把它
        # 和「还有剩余」区分开，调度器才知道该改配置还是该再跑一轮。
        print(f"error: {exc}", file=sys.stderr)
        return EXIT_REFUSED

    print(
        json.dumps(
            {
                "tenant_id": args.tenant_id,
                "apply": args.apply,
                "cutoff": plan.cutoff.isoformat(),
                "expired": plan.expired,
                "deleted": plan.deleted,
                "orphaned_objects": plan.orphaned_objects,
                "remaining": plan.remaining,
            },
            ensure_ascii=False,
            sort_keys=True,
        )
    )
    # 只有真删过之后「还有剩余」才值得再跑一轮。dry-run 恒为 0：
    # 它一张都不删，`remaining > 0` 是它的常态而非待办信号，
    # 返回 3 会让「while rc == 3: 再跑一轮」的调度循环永远停不下来。
    if args.apply and plan.remaining > 0:
        return EXIT_MORE_WORK
    return EXIT_OK


def run() -> None:
    raise SystemExit(main())


if __name__ == "__main__":
    run()

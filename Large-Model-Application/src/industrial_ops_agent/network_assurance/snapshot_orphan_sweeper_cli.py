"""孤儿静帧对象清扫的运维入口。

    industrial-ops-snapshot-sweep --tenant-id <tid>
    industrial-ops-snapshot-sweep --tenant-id <tid> --apply

默认**只报告不删除**。与保留期回收器同一个理由：这是不可逆的证据销毁，而最
容易发生的误用不是「忘了删」，是「以为自己在看报告，其实在删数据」。

## 为什么这个命令必须有一个「额外」的参数：``--min-age-seconds``

保留期回收器的判据是「收到多久了」，孤儿清扫器的判据是「对象存在多久了」。
后者恰好是**危险的那一个**：上传是「put → insert」，两者之间的窗口里对象没有
行，此刻它长得和孤儿一模一样。窗口设短 = 删掉正在上传的图 = 上传方随后插入
一行指向不存在的对象。

所以这个参数故意**不设为**「随便填」：

- 下限 1 秒由 ``SnapshotOrphanSweeper.sweep`` 硬拒绝（0 会让窗口失效）；
- 默认 3600 秒，且帮助文本里写明真实上传耗时是毫秒到秒级——填小了的代价
  不是「删得激进一点」，是**谎报行**。

## 退出码与保留期回收器完全同形

| 退出码 | 含义 | 调度器的正确反应 |
| --- | --- | --- |
| 0 | 正常（含 dry-run、含「没有孤儿」） | 没事 |
| 1 | 参数被拒绝（窗口 < 1、上限越界、没有对象存储） | 别重试，改配置 |
| 2 | **用法错误**（``argparse`` 自己抛的） | 别重试，改配置 |
| 3 | **真删后**仍有剩余（``--apply`` 且 ``remaining > 0``） | 可以再跑一轮 |

「还有剩余」用 3 而不是 2 的理由与保留期回收器完全相同：``argparse`` 的用法
错误已经占用了 2，一个 ``while rc == 2:`` 的调度循环会在参数写错时永远转下去。
dry-run 恒返回 0 也一样——它一张都不删，``remaining > 0`` 是常态而非待办信号。

## 对象存储为什么必须是真的

不提供 InMemory 兜底。那个实现里没有真实对象，用它跑清扫会得到「删了 0 个
孤儿」的绿灯，而磁盘上的孤儿一个没少；更糟的是它**会**报 ``scanned == 0``，
让「没有孤儿」和「连错了存储」这两件事看起来一模一样。
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys

from industrial_ops_agent.config import get_settings
from industrial_ops_agent.network_assurance.snapshot_orphan_sweeper import (
    DEFAULT_MIN_AGE_SECONDS,
    MAX_SWEEP_OBJECTS,
    SnapshotOrphanSweeper,
)
from industrial_ops_agent.object_store.local_disk import LocalDiskTenantObjectStore
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.tenant import TenantContext
from industrial_ops_agent.secrets import SecretName, build_secret_provider

EXIT_OK = 0
EXIT_REFUSED = 1
# ``argparse`` 自己把用法错误固定成 2（缺必填项、参数不是整数），由
# ``parse_args`` 直接抛 ``SystemExit``，不经过 ``main()`` 的返回值。
# 正因如此「还有剩余」必须避开 2，于是用 3。
EXIT_MORE_WORK = 3


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="industrial-ops-snapshot-sweep",
        description=(
            "Delete snapshot objects that have no database row (upload crashed "
            "between put and insert, or the row was deleted but the object survived)."
        ),
    )
    parser.add_argument("--tenant-id", required=True)
    parser.add_argument(
        "--apply",
        action="store_true",
        help="actually delete; without it this only reports what would go",
    )
    parser.add_argument(
        "--min-age-seconds",
        type=int,
        default=DEFAULT_MIN_AGE_SECONDS,
        help=(
            "an object must be at least this old to be considered an orphan "
            f"(default {DEFAULT_MIN_AGE_SECONDS}). This is NOT a tuning knob: "
            "an upload is put-then-insert, so an object inside this window may be "
            "mid-upload and deleting it makes the uploader insert a row that "
            "points at nothing"
        ),
    )
    parser.add_argument(
        "--max-objects",
        type=int,
        default=MAX_SWEEP_OBJECTS,
        help=f"max objects to delete per run (1..{MAX_SWEEP_OBJECTS}); use remaining to loop",
    )
    parser.add_argument(
        "--subject-id",
        default="snapshot-orphan-sweep",
        help="tenant subject recorded by the tenant guard",
    )
    return parser


def _object_store() -> LocalDiskTenantObjectStore:
    directory = os.environ.get("REHEARSAL_OBJECT_STORE_DIR", "").strip()
    if not directory:
        print(
            "error: REHEARSAL_OBJECT_STORE_DIR is not set. Refusing to run: "
            "without a real object store this would report zero orphans while "
            "every orphan object is still on disk.",
            file=sys.stderr,
        )
        raise SystemExit(EXIT_REFUSED)
    return LocalDiskTenantObjectStore(directory)


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)

    # 先做最便宜的拒绝：没有对象存储时连数据库都不该碰。否则同一个「少配了
    # 一个环境变量」会先被报成数据库连不上，排查方向从一开始就错。
    store = _object_store()

    settings = get_settings()
    secrets = build_secret_provider(settings)
    database = Database(secrets.get(SecretName.DATABASE_URL).reveal())
    context = TenantContext(tenant_id=args.tenant_id, subject_id=args.subject_id)
    sweeper = SnapshotOrphanSweeper(database)

    try:
        plan = asyncio.run(
            sweeper.sweep(
                context,
                store,
                apply=args.apply,
                min_age_seconds=args.min_age_seconds,
                max_objects=args.max_objects,
            )
        )
    except ValueError as exc:
        # 参数错误不是「暂时做不了」，重试一万次也一样。用不同退出码把它和
        # 「还有剩余」区分开，调度器才知道该改配置还是该再跑一轮。
        print(f"error: {exc}", file=sys.stderr)
        return EXIT_REFUSED

    print(
        json.dumps(
            {
                "tenant_id": args.tenant_id,
                "apply": args.apply,
                "cutoff": plan.cutoff.isoformat(),
                "scanned": plan.scanned,
                "orphans": plan.orphans,
                "deleted": plan.deleted,
                "delete_failures": plan.delete_failures,
                "remaining": plan.remaining,
            },
            ensure_ascii=False,
            sort_keys=True,
        )
    )
    # 只有真删过之后「还有剩余」才值得再跑一轮。dry-run 恒为 0：它一张都不删，
    # `remaining > 0` 是它的常态而非待办信号。
    if args.apply and plan.remaining > 0:
        return EXIT_MORE_WORK
    return EXIT_OK


def run() -> None:
    raise SystemExit(main())


if __name__ == "__main__":
    run()

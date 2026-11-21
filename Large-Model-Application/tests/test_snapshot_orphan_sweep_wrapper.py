"""孤儿清扫包装脚本的语义测试：退出码翻译、参数守卫、多租户循环。

与 `test_snapshot_retention_wrapper.py` 同一套假设：被测对象是**脚本的控制
流**，不是清扫实现（那由 `test_snapshot_orphan_sweeper.py` 的 24 项覆盖）。
用假 `industrial-ops-snapshot-sweep` 让这些用例不依赖 Postgres。

这个脚本比回收器那个多一条硬约束，所以也多一组用例：

| 如果这里错了 | 表面现象 | 真实后果 |
| --- | --- | --- |
| 把退出码 3 当失败透出 | unit 每天变红 | 人被训练成忽略 timer，孤儿堆到盘满 |
| 给窗口一个默认值 | 一切正常 | 没人复核过的窗口开始删对象 |
| **窗口配得比 60 秒还短** | 「删得激进一点」 | **删掉正在上传的图 → 上传方写入谎报行** |
| 缺租户列表时自动枚举 | 「帮你省事」 | 一次指向开发库的误操作删光所有租户孤儿 |
| 多租户循环漏掉某个租户 | 该租户报告「无孤儿」 | 有些租户的磁盘永远在涨，且看不出来 |

第三行是这个脚本独有的一道闸：`SnapshotOrphanSweeper` 自己只拒绝 `< 1`，
而 1 秒的窗口在安全上是荒谬的（上传是 put→insert，两者之间对象没有行）。
包装层把下限抬到 60 秒，并在**开跑之前**就红。
"""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

import pytest

# 仓库是两层的：本文件在 `<repo>/Large-Model-Application/tests/`，脚本在
# `<repo>/tools/`。写死 `parents[1]` 会得到不存在的目录，全部用例以 rc=127 失败。
SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "snapshot_orphan_sweep.sh"
PROG_NAME = "industrial-ops-snapshot-sweep"


def _fake_cli(tmp_path: Path, body: str) -> Path:
    """在 PATH 上放一个假的清扫命令。"""

    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    path = bin_dir / PROG_NAME
    path.write_text("#!/usr/bin/env bash\n" + body, encoding="utf-8")
    path.chmod(0o755)
    return bin_dir


def _run(
    tmp_path: Path,
    *,
    bin_dir: Path,
    env: dict[str, str],
    store: Path | None = None,
) -> subprocess.CompletedProcess[str]:
    store_dir = store or (tmp_path / "objects")
    store_dir.mkdir(exist_ok=True)
    base = {
        "PATH": f"{bin_dir}:{os.environ['PATH']}",
        "IOAP_DATABASE_URL": "postgresql+psycopg://x/y",
        "REHEARSAL_OBJECT_STORE_DIR": str(store_dir),
        "SNAPSHOT_SWEEP_MIN_AGE_SECONDS": "86400",
        "SNAPSHOT_SWEEP_TENANTS": "tenant-a",
    }
    base.update(env)
    for key in ("SNAPSHOT_SWEEP_ALL", "SNAPSHOT_SWEEP_TENANT"):
        if key not in env:
            base.pop(key, None)
    return subprocess.run(
        ["bash", str(SCRIPT)],
        capture_output=True,
        text=True,
        env=base,
        timeout=60,
    )


# ===========================================================================
# 退出码 3 必须被吃掉：这是整个包装层的存在理由
# ===========================================================================


def test_exit_code_three_is_consumed_and_looped(tmp_path: Path) -> None:
    """「还有剩余」不是错误，是「继续」。交给 systemd 会把 unit 标红。"""

    counter = tmp_path / "count"
    bin_dir = _fake_cli(
        tmp_path,
        f"""
echo "run" >> {counter}
n=$(wc -l < {counter})
if (( n < 3 )); then
  echo '{{"deleted": 1, "remaining": 5}}'
  exit 3
fi
echo '{{"deleted": 1, "remaining": 0}}'
exit 0
""",
    )

    result = _run(tmp_path, bin_dir=bin_dir, env={})

    assert result.returncode == 0, result.stderr
    assert counter.read_text().count("run") == 3


def test_max_rounds_stops_an_endless_exit_three(tmp_path: Path) -> None:
    """永远返回 3 时不能无限转。达到上限必须正常退出并留 WARN。"""

    counter = tmp_path / "count"
    bin_dir = _fake_cli(
        tmp_path,
        f"""
echo "run" >> {counter}
echo '{{"deleted": 0, "remaining": 5}}'
exit 3
""",
    )

    result = _run(
        tmp_path,
        bin_dir=bin_dir,
        env={"SNAPSHOT_SWEEP_MAX_ROUNDS": "3"},
    )

    assert result.returncode == 0, result.stderr
    assert counter.read_text().count("run") == 3
    assert "MAX_ROUNDS" in result.stderr


def test_config_error_is_propagated_not_swallowed(tmp_path: Path) -> None:
    """退出码 1 是配置问题，重试一万次也一样。必须红着等人看。"""

    bin_dir = _fake_cli(tmp_path, "exit 1\n")

    result = _run(tmp_path, bin_dir=bin_dir, env={})

    assert result.returncode == 1
    assert "CONFIG ERROR" in result.stderr


def test_usage_error_is_propagated(tmp_path: Path) -> None:
    bin_dir = _fake_cli(tmp_path, "exit 2\n")

    result = _run(tmp_path, bin_dir=bin_dir, env={})

    assert result.returncode == 2


def test_unexpected_exit_code_is_propagated(tmp_path: Path) -> None:
    """没预料到的码也要透出。吞掉它会让一个真故障看起来像正常结束。"""

    bin_dir = _fake_cli(tmp_path, "exit 7\n")

    result = _run(tmp_path, bin_dir=bin_dir, env={})

    assert result.returncode == 7


# ===========================================================================
# 前置检查：缺任何一个都必须在开跑之前失败
# ===========================================================================


def test_missing_window_fails_closed(tmp_path: Path) -> None:
    """**不给默认窗口。** 默认值意味着没人复核过的数字开始删对象。"""

    bin_dir = _fake_cli(tmp_path, "echo should-not-run\n")
    env = {
        "IOAP_DATABASE_URL": "",
        "REHEARSAL_OBJECT_STORE_DIR": "",
        "SNAPSHOT_SWEEP_MIN_AGE_SECONDS": "",
    }
    base = {
        "PATH": f"{bin_dir}:{os.environ['PATH']}",
        **env,
    }
    store_dir = tmp_path / "objects"
    store_dir.mkdir()
    base["IOAP_DATABASE_URL"] = "postgresql+psycopg://x/y"
    base["REHEARSAL_OBJECT_STORE_DIR"] = str(store_dir)

    result = subprocess.run(
        ["bash", str(SCRIPT)], capture_output=True, text=True, env=base, timeout=60
    )

    assert result.returncode != 0
    assert "SNAPSHOT_SWEEP_MIN_AGE_SECONDS" in result.stderr
    assert "should-not-run" not in result.stdout


@pytest.mark.parametrize("window", ["59", "1", "0", "abc", "-5"])
def test_window_below_the_safety_floor_fails_closed(tmp_path: Path, window: str) -> None:
    """**这一组是本脚本独有的闸。**

    窗口 < 60 秒在安全上是荒谬的：上传是 put → insert，两者之间对象没有行，
    清扫器会把正在上传的图当成孤儿删掉，上传方随后插入一行指向不存在的对象。
    `SnapshotOrphanSweeper` 只拒绝 `< 1`，所以 30 秒会被它放行——危险值必须
    在这里拦住，而且在**开跑之前**。
    """

    bin_dir = _fake_cli(tmp_path, "echo should-not-run\n")

    result = _run(tmp_path, bin_dir=bin_dir, env={"SNAPSHOT_SWEEP_MIN_AGE_SECONDS": window})

    assert result.returncode != 0
    assert "SNAPSHOT_SWEEP_MIN_AGE_SECONDS" in result.stderr
    assert "should-not-run" not in result.stdout


def test_window_exactly_at_the_floor_is_accepted(tmp_path: Path) -> None:
    """60 秒是边界，必须放行（否则下限会被写成「大于」而无人察觉）。"""

    bin_dir = _fake_cli(tmp_path, 'echo \'{"deleted": 0, "remaining": 0}\'\nexit 0\n')

    result = _run(tmp_path, bin_dir=bin_dir, env={"SNAPSHOT_SWEEP_MIN_AGE_SECONDS": "60"})

    assert result.returncode == 0, result.stderr


def test_missing_tenant_list_fails_closed(tmp_path: Path) -> None:
    """自动枚举所有租户 = 「漏配」变成「默认全删」。"""

    bin_dir = _fake_cli(tmp_path, "echo should-not-run\n")

    result = _run(
        tmp_path,
        bin_dir=bin_dir,
        env={"SNAPSHOT_SWEEP_TENANTS": ""},
    )

    assert result.returncode != 0
    assert "SNAPSHOT_SWEEP_TENANTS" in result.stderr
    assert "should-not-run" not in result.stdout


def test_missing_store_dir_fails_closed(tmp_path: Path) -> None:
    bin_dir = _fake_cli(tmp_path, "echo should-not-run\n")
    base = {
        "PATH": f"{bin_dir}:{os.environ['PATH']}",
        "IOAP_DATABASE_URL": "postgresql+psycopg://x/y",
        "SNAPSHOT_SWEEP_MIN_AGE_SECONDS": "86400",
        "SNAPSHOT_SWEEP_TENANTS": "tenant-a",
    }

    result = subprocess.run(
        ["bash", str(SCRIPT)], capture_output=True, text=True, env=base, timeout=60
    )

    assert result.returncode != 0
    assert "REHEARSAL_OBJECT_STORE_DIR" in result.stderr


def test_nonexistent_store_dir_fails_closed(tmp_path: Path) -> None:
    bin_dir = _fake_cli(tmp_path, "echo should-not-run\n")

    result = _run(
        tmp_path,
        bin_dir=bin_dir,
        env={"REHEARSAL_OBJECT_STORE_DIR": str(tmp_path / "does-not-exist")},
    )

    assert result.returncode != 0
    assert "not a directory" in result.stderr


def test_missing_database_url_fails_closed(tmp_path: Path) -> None:
    bin_dir = _fake_cli(tmp_path, "echo should-not-run\n")
    base = {
        "PATH": f"{bin_dir}:{os.environ['PATH']}",
        "REHEARSAL_OBJECT_STORE_DIR": str(tmp_path / "objects"),
        "SNAPSHOT_SWEEP_MIN_AGE_SECONDS": "86400",
        "SNAPSHOT_SWEEP_TENANTS": "tenant-a",
    }
    (tmp_path / "objects").mkdir()

    result = subprocess.run(
        ["bash", str(SCRIPT)], capture_output=True, text=True, env=base, timeout=60
    )

    assert result.returncode != 0
    assert "IOAP_DATABASE_URL" in result.stderr


# ===========================================================================
# 多租户与参数透传
# ===========================================================================


def test_every_tenant_is_visited(tmp_path: Path) -> None:
    """漏掉一个租户看起来和「该租户没有孤儿」一模一样——不会报错。"""

    log = tmp_path / "tenants"
    bin_dir = _fake_cli(
        tmp_path,
        f"""
for arg in "$@"; do
  case "$prev" in
    --tenant-id) echo "$arg" >> {log} ;;
  esac
  prev="$arg"
done
echo '{{"deleted": 0, "remaining": 0}}'
exit 0
""",
    )

    result = _run(
        tmp_path,
        bin_dir=bin_dir,
        env={"SNAPSHOT_SWEEP_TENANTS": "tenant-a tenant-b tenant-c"},
    )

    assert result.returncode == 0, result.stderr
    assert log.read_text().split() == ["tenant-a", "tenant-b", "tenant-c"]


def test_single_tenant_escape_hatch_requires_explicit_id(tmp_path: Path) -> None:
    """SNAPSHOT_SWEEP_ALL=1 不是「自动发现」，它仍然要求写出租户。"""

    bin_dir = _fake_cli(tmp_path, 'echo \'{"deleted": 0, "remaining": 0}\'\nexit 0\n')

    without = _run(
        tmp_path,
        bin_dir=bin_dir,
        env={"SNAPSHOT_SWEEP_TENANTS": "", "SNAPSHOT_SWEEP_ALL": "1"},
    )
    assert without.returncode != 0, "没有显式租户也被放行了"

    with_id = _run(
        tmp_path,
        bin_dir=bin_dir,
        env={
            "SNAPSHOT_SWEEP_TENANTS": "",
            "SNAPSHOT_SWEEP_ALL": "1",
            "SNAPSHOT_SWEEP_TENANT": "tenant-solo",
        },
    )
    assert with_id.returncode == 0, with_id.stderr


def test_apply_and_window_are_always_passed(tmp_path: Path) -> None:
    """清扫器必须拿到 `--apply` 和配置的窗口。

    漏了 `--apply`，定时器每轮只打印报告、一个对象都不删，而 unit 天天是绿的
    ——那比没有定时器更糟。漏了 `--min-age-seconds`，命令会退回它自己的默认
    值，而运维以为配的是 unit 里那个数。
    """

    argv_log = tmp_path / "argv"
    bin_dir = _fake_cli(
        tmp_path,
        f"""
printf '%s\\n' "$@" > {argv_log}
echo '{{"deleted": 0, "remaining": 0}}'
exit 0
""",
    )

    result = _run(
        tmp_path,
        bin_dir=bin_dir,
        env={"SNAPSHOT_SWEEP_MIN_AGE_SECONDS": "172800"},
    )

    assert result.returncode == 0, result.stderr
    argv = argv_log.read_text().split()
    assert "--apply" in argv
    assert "--min-age-seconds" in argv
    assert argv[argv.index("--min-age-seconds") + 1] == "172800"

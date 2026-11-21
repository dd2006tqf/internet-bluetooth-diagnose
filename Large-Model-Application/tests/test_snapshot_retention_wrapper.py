"""保留策略包装脚本的语义测试：退出码翻译、参数守卫、多租户循环。

**为什么这条 shell 脚本值得专门测。** 它不产生任何业务价值，它只做一件事：
把 `industrial-ops-snapshot-retention` 的三档退出码翻译成 systemd 能理解的
结论。而这件事做错的后果是**静默的**：

| 如果这里错了 | 表面现象 | 真实后果 |
| --- | --- | --- |
| 把退出码 3 当失败透出 | unit 每天变红 | 人被训练成忽略这个 timer，磁盘写满 |
| 给 `SNAPSHOT_RETENTION_DAYS` 一个默认值 | 一切正常、日志好看 | 没人复核过的数字开始删证据 |
| 缺租户列表时自动枚举 | 「帮你省事」 | 一次指向开发库的误操作删光所有租户 |
| 多租户循环漏掉某个租户 | 该租户报告「无过期数据」 | 有些客户证据永不回收，且看不出来 |

这四件事没有一个会报错。所以每一条都配一个用例。

用假 `industrial-ops-snapshot-retention` 而不是真命令：被测对象是**脚本的
控制流**，不是回收实现（那由 `test_snapshot_retention.py` 的 18 项覆盖）。
用真命令会让这些用例依赖 Postgres，跑得慢且脆。
"""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

import pytest

# 仓库是两层的：本文件在 `<repo>/Large-Model-Application/tests/`，而部署用的
# 脚本在 `<repo>/tools/`（与 weaknet-server.service 等 systemd 单元同目录）。
# 写死成 `parents[1] / "tools"` 会得到 `Large-Model-Application/tools/`——那个
# 目录不存在，于是所有用例都以 rc=127 "No such file or directory" 失败。
SCRIPT = Path(__file__).resolve().parents[2] / "tools" / "snapshot_retention.sh"
PROG_NAME = "industrial-ops-snapshot-retention"


def _fake_cli(tmp_path: Path, body: str) -> Path:
    """在 PATH 上放一个假的回收命令。"""

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
        "SNAPSHOT_RETENTION_DAYS": "30",
        "SNAPSHOT_RETENTION_TENANTS": "tenant-a",
    }
    base.update(env)
    # 显式清掉可能从开发机继承下来的无关变量，避免用例互相污染。
    for key in ("SNAPSHOT_RETENTION_ALL", "SNAPSHOT_RETENTION_TENANT"):
        base.pop(key, None) if key not in env else None
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
    """先返回 3（还有剩余），再返回 0。脚本必须自己跑第二轮，最终 0。

    如果这条失败（脚本把 3 透出），systemd 会把 unit 标红。一个每天红的
    定时器最终会被忽略，而忽略定时器正是磁盘写满的标准前奏。
    """

    counter = tmp_path / "calls"
    bin_dir = _fake_cli(
        tmp_path,
        f"""\
n=$(cat {counter} 2>/dev/null || echo 0)
n=$((n+1)); echo $n > {counter}
if (( n < 3 )); then
  echo '{{"deleted": 2, "remaining": 3}}'
  exit 3
fi
echo '{{"deleted": 1, "remaining": 0}}'
exit 0
""",
    )

    result = _run(tmp_path, bin_dir=bin_dir, env={})

    assert result.returncode == 0, f"3 必须被消化成 0，实际 rc={result.returncode}\n{result.stderr}"
    assert counter.read_text().strip() == "3", "应当真的跑了三轮"
    assert result.stderr.count("more work remains") == 2


def test_max_rounds_stops_an_endless_exit_three(tmp_path: Path) -> None:
    """永远返回 3 也不能无限转——否则这台 timer 永不结束，下次触发被跳过。"""

    bin_dir = _fake_cli(
        tmp_path,
        """\
echo '{"deleted": 0, "remaining": 1}'
exit 3
""",
    )

    result = _run(
        tmp_path,
        bin_dir=bin_dir,
        env={"SNAPSHOT_RETENTION_MAX_ROUNDS": "3"},
    )

    assert result.returncode == 0, "达到轮数上限是正常收尾，不是失败"
    assert "MAX_ROUNDS=3" in result.stderr
    # `round` 从 1 开始、上限判断在调用**之前**，所以 MAX_ROUNDS=3 会真的
    # 跑满 3 轮（每轮一条 more work remains），第 4 轮开头才判上限退出。
    # 写成 == 2 是我按「第 3 轮先判上限」想当然了，实测是 3。
    assert result.stderr.count("more work remains") == 3


def test_config_error_is_propagated_not_swallowed(tmp_path: Path) -> None:
    """退出码 1 必须原样透出，让 unit 红着等人看。

    吞掉它的后果：保留期配成 0 的部署会「正常运行」，每一轮都在删光。
    """

    bin_dir = _fake_cli(
        tmp_path,
        """\
echo 'error: retention_days must be at least 1' >&2
exit 1
""",
    )

    result = _run(tmp_path, bin_dir=bin_dir, env={})

    assert result.returncode == 1
    assert "CONFIG ERROR" in result.stderr


def test_usage_error_is_propagated(tmp_path: Path) -> None:
    bin_dir = _fake_cli(tmp_path, "exit 2\n")

    result = _run(tmp_path, bin_dir=bin_dir, env={})

    assert result.returncode == 2, "argparse 的用法错误必须原样透出"


# ===========================================================================
# 参数守卫：宁可红着，也不替运维做决定
# ===========================================================================


def test_missing_retention_days_fails_closed(tmp_path: Path) -> None:
    """**故意没有默认值。** 一个「没配就用 90 天」的默认值意味着：某次部署
    忘了配，回收器会带着一个没人复核过的数字开始删证据，而且看起来一切正常。
    """

    bin_dir = _fake_cli(tmp_path, "exit 0\n")

    result = _run(tmp_path, bin_dir=bin_dir, env={"SNAPSHOT_RETENTION_DAYS": ""})

    assert result.returncode != 0
    assert "SNAPSHOT_RETENTION_DAYS" in result.stderr


@pytest.mark.parametrize("days", ["0", "-1", "abc", "3.5"])
def test_invalid_retention_days_fails_closed(tmp_path: Path, days: str) -> None:
    bin_dir = _fake_cli(tmp_path, "exit 0\n")

    result = _run(tmp_path, bin_dir=bin_dir, env={"SNAPSHOT_RETENTION_DAYS": days})

    assert result.returncode != 0
    assert "SNAPSHOT_RETENTION_DAYS" in result.stderr


def test_missing_tenant_list_fails_closed(tmp_path: Path) -> None:
    """不自动枚举租户。

    自动枚举把「漏配」变成「默认全删」：一次指向开发库的误操作，枚举出的
    每个租户都会被回收。显式列表让「删哪些租户」必须是一个写下来的决定。
    """

    bin_dir = _fake_cli(tmp_path, "exit 0\n")

    result = _run(tmp_path, bin_dir=bin_dir, env={"SNAPSHOT_RETENTION_TENANTS": ""})

    assert result.returncode != 0
    assert "SNAPSHOT_RETENTION_TENANTS" in result.stderr


def test_missing_store_dir_fails_closed(tmp_path: Path) -> None:
    bin_dir = _fake_cli(tmp_path, "exit 0\n")

    result = _run(
        tmp_path,
        bin_dir=bin_dir,
        env={"REHEARSAL_OBJECT_STORE_DIR": str(tmp_path / "does-not-exist")},
    )

    assert result.returncode != 0
    assert "REHEARSAL_OBJECT_STORE_DIR" in result.stderr


def test_missing_database_url_fails_closed(tmp_path: Path) -> None:
    bin_dir = _fake_cli(tmp_path, "exit 0\n")

    result = _run(tmp_path, bin_dir=bin_dir, env={"IOAP_DATABASE_URL": ""})

    assert result.returncode != 0
    assert "IOAP_DATABASE_URL" in result.stderr


# ===========================================================================
# 多租户：每一个都必须跑到
# ===========================================================================


def test_every_tenant_is_visited(tmp_path: Path) -> None:
    """漏掉一个租户不会报错——它只会安静地不回收。

    这就是为什么这里要断言「三个租户的 id 都出现在命令行里」，而不是只
    断言脚本退出码为 0。
    """

    seen = tmp_path / "seen"
    bin_dir = _fake_cli(
        tmp_path,
        f"""\
printf '%s\\n' "$2" >> {seen}
echo '{{"deleted": 0, "remaining": 0}}'
exit 0
""",
    )

    result = _run(
        tmp_path,
        bin_dir=bin_dir,
        env={"SNAPSHOT_RETENTION_TENANTS": "tenant-a tenant-b tenant-c"},
    )

    assert result.returncode == 0
    assert seen.read_text().split() == ["tenant-a", "tenant-b", "tenant-c"]


def test_single_tenant_escape_hatch_requires_explicit_id(tmp_path: Path) -> None:
    """`SNAPSHOT_RETENTION_ALL=1` 仍要求显式给出 TENANT，不是「自动发现」。"""

    seen = tmp_path / "seen"
    bin_dir = _fake_cli(
        tmp_path,
        f"""\
printf '%s\\n' "$2" >> {seen}
exit 0
""",
    )

    result = _run(
        tmp_path,
        bin_dir=bin_dir,
        env={
            "SNAPSHOT_RETENTION_TENANTS": "",
            "SNAPSHOT_RETENTION_ALL": "1",
            "SNAPSHOT_RETENTION_TENANT": "tenant-solo",
        },
    )

    assert result.returncode == 0
    assert seen.read_text().split() == ["tenant-solo"]


def test_apply_is_always_passed(tmp_path: Path) -> None:
    """定时器跑的是 `--apply`。

    dry-run 的正确用法是「上线前手工跑一次确认要删什么」。让定时器停在
    dry-run 比没有定时器更糟：`systemctl status` 一直是绿的，磁盘一个字节
    都不会少。
    """

    args_file = tmp_path / "args"
    bin_dir = _fake_cli(
        tmp_path,
        f"""\
printf '%s\\n' "$@" > {args_file}
exit 0
""",
    )

    result = _run(tmp_path, bin_dir=bin_dir, env={})

    assert result.returncode == 0
    assert "--apply" in args_file.read_text().split()

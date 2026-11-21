"""孤儿清扫命令行的退出码语义、拒绝运行、JSON 报告。

与 `test_snapshot_retention_cli.py` 同一套结构，但守的是**另一组**数字：

| 退出码 | 含义 | 这里为什么也要单独钉一遍 |
| --- | --- | --- |
| 0 | 正常（含 dry-run、含「没有孤儿」） | 静默失败最常见的样子就是「永远 0」 |
| 1 | 参数被拒绝 | 窗口配成 0 必须**在删除之前**停住 |
| 2 | `argparse` 用法错误 | 与 3 必须分得开，否则调度循环永转 |
| 3 | 真删后仍有剩余 | 漏了它，积压永远清不完 |

「删得对不对」由 `test_snapshot_orphan_sweeper.py` 的 24 项覆盖，这里不重复。
"""

from __future__ import annotations

import asyncio
import json
import os
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any

import pytest
from sqlalchemy import create_engine
from sqlalchemy.pool import StaticPool

from industrial_ops_agent.network_assurance import snapshot_orphan_sweeper_cli as cli
from industrial_ops_agent.object_store.keys import (
    edge_snapshot_image_object_key,
    tenant_object_key_from_value,
)
from industrial_ops_agent.object_store.local_disk import LocalDiskTenantObjectStore
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import Base, NetworkSnapshotImageRecord
from industrial_ops_agent.persistence.tenant import TenantContext
from industrial_ops_agent.secrets import SecretSource, SecretValue

TENANT = "tenant-sweep-cli"
PAYLOAD = b"\xff\xd8sweep-cli-payload\xff\xd9"


@pytest.fixture
def database() -> Database:
    engine = create_engine(
        "sqlite:///:memory:",
        connect_args={"check_same_thread": False},
        poolclass=StaticPool,
    )
    Base.metadata.create_all(engine)
    return Database.from_engine(engine)


@pytest.fixture
def store_dir(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    directory = tmp_path / "objects"
    directory.mkdir()
    monkeypatch.setenv("REHEARSAL_OBJECT_STORE_DIR", str(directory))
    return directory


class _StubSecrets:
    def get(self, name: Any) -> SecretValue:
        return SecretValue("sqlite://", source=SecretSource.ENVIRONMENT)

    def refresh(self) -> None:
        return None


@pytest.fixture(autouse=True)
def _isolate_process_dependencies(database: Database, monkeypatch: pytest.MonkeyPatch) -> None:
    """把命令里的进程级依赖换成测试替身。

    替的是**被构造出来的对象**，不是绕过装配顺序：先拒绝无存储、再连库、
    再构造清扫器，这个顺序仍然照原样跑。
    """

    monkeypatch.setattr(cli, "get_settings", lambda: object())
    monkeypatch.setattr(cli, "build_secret_provider", lambda _settings: _StubSecrets())
    monkeypatch.setattr(cli, "Database", lambda _url: database)


def _write_object(store_dir: Path, image_id: str, *, age: timedelta) -> None:
    """直接按路径写对象并用 utime 造出「很久以前落的盘」。"""

    context = TenantContext(tenant_id=TENANT, subject_id="cli-test")
    key = edge_snapshot_image_object_key(context, image_id)
    path = store_dir.joinpath(*key.value.split("/"))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(PAYLOAD)
    stamp = (datetime.now(UTC) - age).timestamp()
    os.utime(path, (stamp, stamp))


def _seed_row(database: Database, image_id: str) -> None:
    context = TenantContext(tenant_id=TENANT, subject_id="cli-test")
    key = edge_snapshot_image_object_key(context, image_id)
    with database.transaction(context) as session:
        session.add(
            NetworkSnapshotImageRecord(
                image_id=image_id,
                tenant_id=TENANT,
                asset_id="radxa-cubie-a7a",
                site_id="",
                gateway_id="radxa-cubie-a7a",
                incident_id="sitinc-cli",
                codec="JPEG",
                width=1640,
                height=1232,
                captured_at_ms=int(datetime.now(UTC).timestamp() * 1000),
                size_bytes=len(PAYLOAD),
                content_sha256="0" * 64,
                object_key=key.value,
                received_at=datetime.now(UTC),
            )
        )


def _exists(store_dir: Path, image_id: str) -> bool:
    context = TenantContext(tenant_id=TENANT, subject_id="cli-test")
    key = tenant_object_key_from_value(
        context, edge_snapshot_image_object_key(context, image_id).value
    )
    return asyncio.run(LocalDiskTenantObjectStore(store_dir).get(context, key)) is not None


def _argv(*extra: str) -> list[str]:
    return ["--tenant-id", TENANT, *extra]


# ===========================================================================
# 拒绝运行：没有真实对象存储就什么都不做
# ===========================================================================


def test_refuses_without_object_store_dir(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """没有对象存储时宁可报错退出。

    回落 InMemory 会得到「扫了 0 个对象、删了 0 个孤儿」的绿灯，而磁盘上的
    孤儿一个没少；更糟的是 `scanned == 0` 让「真的没有孤儿」和「连错了存储」
    这两件事看起来一模一样。
    """

    monkeypatch.delenv("REHEARSAL_OBJECT_STORE_DIR", raising=False)

    with pytest.raises(SystemExit) as caught:
        cli.main(_argv())

    assert caught.value.code == 1
    assert "REHEARSAL_OBJECT_STORE_DIR" in capsys.readouterr().err


def test_refusal_happens_before_touching_the_database(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """先拒绝无存储，再连库。否则「少配一个环境变量」会被报成数据库连不上。"""

    monkeypatch.delenv("REHEARSAL_OBJECT_STORE_DIR", raising=False)

    def _explode(_url: str) -> None:
        raise AssertionError("数据库被构造了：拒绝必须发生在它之前")

    monkeypatch.setattr(cli, "Database", _explode)

    with pytest.raises(SystemExit):
        cli.main(_argv())


# ===========================================================================
# dry-run 与 apply
# ===========================================================================


def test_dry_run_reports_orphans_and_exits_zero(
    store_dir: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    _write_object(store_dir, "snap-old", age=timedelta(days=30))

    rc = cli.main(_argv())

    assert rc == 0
    report = json.loads(capsys.readouterr().out)
    assert report["orphans"] == 1
    assert report["deleted"] == 0
    assert report["remaining"] == 1
    assert _exists(store_dir, "snap-old"), "dry-run 把对象删掉了"


def test_dry_run_exits_zero_even_when_remaining_is_positive(
    store_dir: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """dry-run 恒为 0。

    它一张都不删，`remaining > 0` 是它的常态而非待办信号。返回 3 会让
    `while rc == 3: 再跑一轮` 的调度循环永远停不下来。
    """

    _write_object(store_dir, "snap-old", age=timedelta(days=30))

    assert cli.main(_argv()) == 0
    capsys.readouterr()


def test_apply_deletes_orphans_and_exits_zero(
    store_dir: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    _write_object(store_dir, "snap-old", age=timedelta(days=30))

    rc = cli.main(_argv("--apply"))

    assert rc == 0
    report = json.loads(capsys.readouterr().out)
    assert report["deleted"] == 1
    assert report["remaining"] == 0
    assert not _exists(store_dir, "snap-old")


def test_apply_exits_three_when_more_work_remains(
    store_dir: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """还有剩余 → 3，让包装脚本再跑一轮。"""

    for i in range(3):
        _write_object(store_dir, f"snap-{i}", age=timedelta(days=30))

    rc = cli.main(_argv("--apply", "--max-objects", "2"))

    assert rc == 3
    report = json.loads(capsys.readouterr().out)
    assert report["deleted"] == 2
    assert report["remaining"] == 1


def test_fresh_object_is_never_deleted_by_apply(
    store_dir: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """正在上传的图：默认窗口内，`--apply` 也绝不能碰。

    这是整个命令唯一真正不可挽回的误操作。
    """

    _write_object(store_dir, "snap-inflight", age=timedelta(seconds=5))

    rc = cli.main(_argv("--apply"))

    assert rc == 0
    report = json.loads(capsys.readouterr().out)
    assert report["scanned"] == 1
    assert report["orphans"] == 0
    assert _exists(store_dir, "snap-inflight"), "正在上传的对象被真删了"


def test_object_with_a_row_is_never_swept(
    store_dir: Path, database: Database, capsys: pytest.CaptureFixture[str]
) -> None:
    """有行 = 库里承认了这张证据。它归保留期回收器管，不归清扫器管。"""

    _write_object(store_dir, "snap-tracked", age=timedelta(days=400))
    _seed_row(database, "snap-tracked")

    rc = cli.main(_argv("--apply"))

    assert rc == 0
    report = json.loads(capsys.readouterr().out)
    assert report["orphans"] == 0
    assert _exists(store_dir, "snap-tracked"), "有行的对象被清扫器删掉了"


# ===========================================================================
# JSON 报告字段
# ===========================================================================


def test_json_report_has_every_documented_field(
    store_dir: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """字段少一个，运维脚本就在某天静默地 KeyError 或读到 None。"""

    _write_object(store_dir, "snap-old", age=timedelta(days=30))

    cli.main(_argv())
    report = json.loads(capsys.readouterr().out)

    assert set(report) == {
        "tenant_id",
        "apply",
        "cutoff",
        "scanned",
        "orphans",
        "deleted",
        "delete_failures",
        "remaining",
    }
    assert report["tenant_id"] == TENANT
    assert report["apply"] is False
    # cutoff 必须是可解析的带时区时刻，不是一个裸数字或本地时间字符串。
    cutoff = datetime.fromisoformat(report["cutoff"])
    assert cutoff.tzinfo is not None


# ===========================================================================
# 参数被拒绝
# ===========================================================================


def test_zero_window_exits_one_and_deletes_nothing(
    store_dir: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """窗口 0 = 删掉所有正在上传的图。必须在删除之前拒绝。"""

    _write_object(store_dir, "snap-recent", age=timedelta(seconds=1))

    rc = cli.main(_argv("--apply", "--min-age-seconds", "0"))

    assert rc == 1
    assert "min_age_seconds" in capsys.readouterr().err
    assert _exists(store_dir, "snap-recent"), "拒绝必须发生在任何删除之前"


def test_invalid_max_objects_exits_one(store_dir: Path, capsys: pytest.CaptureFixture[str]) -> None:
    rc = cli.main(_argv("--apply", "--max-objects", "0"))

    assert rc == 1
    assert "max_objects" in capsys.readouterr().err


def test_missing_required_argument_still_uses_two() -> None:
    """`argparse` 自己占 2。3 必须留给「还有剩余」，两者绝不能撞。"""

    with pytest.raises(SystemExit) as caught:
        cli.main([])

    assert caught.value.code == 2

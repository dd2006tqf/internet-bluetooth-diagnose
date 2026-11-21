"""静帧保留策略的命令行入口：退出码语义、拒绝运行、JSON 报告。

这个文件不重复验「删得对不对」——那是 `test_snapshot_retention.py` 的事。
它只回答命令行特有的一类问题：**调度器看到的那个整数是什么意思**。

退出码是这块唯一真正的对外契约。一个「删除失败也返回 0」的回收命令会被
定时任务当成每天都在正常工作，直到磁盘写满为止；而一个「参数写错也返回
可重试码」的命令会让调度器无限重试一条永远不会成功的命令行。

所以下面每条用例都钉一个**具体数字**，不用常量反过来断言常量——那种写法
在常量被改错时会跟着一起错。
"""

from __future__ import annotations

import asyncio
import json
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any

import pytest
from sqlalchemy import create_engine, func, select
from sqlalchemy.pool import StaticPool

from industrial_ops_agent.network_assurance import snapshot_retention_cli as cli
from industrial_ops_agent.object_store.keys import edge_snapshot_image_object_key
from industrial_ops_agent.object_store.local_disk import LocalDiskTenantObjectStore
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import Base, NetworkSnapshotImageRecord
from industrial_ops_agent.persistence.tenant import TenantContext
from industrial_ops_agent.secrets import SecretSource, SecretValue

TENANT = "tenant-alpha"
PAYLOAD = b"\xff\xd8cli-test-payload\xff\xd9"


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
    # 命令读的是这个环境变量（与 rehearsal_server.py 同一个），不是 Settings。
    monkeypatch.setenv("REHEARSAL_OBJECT_STORE_DIR", str(directory))
    return directory


class _StubSecrets:
    """只认得 DATABASE_URL 的假 SecretProvider。"""

    def get(self, name: Any) -> SecretValue:
        return SecretValue("sqlite://", source=SecretSource.ENVIRONMENT)

    def refresh(self) -> None:
        return None


@pytest.fixture(autouse=True)
def _isolate_process_dependencies(database: Database, monkeypatch: pytest.MonkeyPatch) -> None:
    """把命令里的进程级依赖换成测试替身。

    `main()` 里 `Database(...)` 的入参是真实 Postgres DSN，测试里既连不上
    也不该连。这里替的是**被构造出来的对象**，不是绕过它——`main()` 自己的
    装配顺序（先拒绝无存储、再连库、再构造服务）仍然照原样跑，
    `test_refusal_happens_before_touching_the_database` 验的就是这个顺序。
    """

    monkeypatch.setattr(cli, "get_settings", lambda: object())
    monkeypatch.setattr(cli, "build_secret_provider", lambda _settings: _StubSecrets())
    monkeypatch.setattr(cli, "Database", lambda _url: database)


def _seed(database: Database, store_dir: Path, image_id: str, *, age_days: float) -> str:
    context = TenantContext(tenant_id=TENANT, subject_id="cli-test")
    key = edge_snapshot_image_object_key(context, image_id)
    asyncio.run(LocalDiskTenantObjectStore(store_dir).put(context, key, PAYLOAD))

    received_at = datetime.now(UTC) - timedelta(days=age_days)
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
                captured_at_ms=int(received_at.timestamp() * 1000),
                size_bytes=len(PAYLOAD),
                content_sha256="0" * 64,
                object_key=key.value,
                received_at=received_at,
            )
        )
    return key.value


def _row_count(database: Database) -> int:
    context = TenantContext(tenant_id=TENANT, subject_id="cli-test")
    with database.transaction(context) as session:
        return (
            session.scalar(
                select(func.count())
                .select_from(NetworkSnapshotImageRecord)
                .where(NetworkSnapshotImageRecord.tenant_id == TENANT)
            )
            or 0
        )


def _object_exists(store_dir: Path, object_key: str) -> bool:
    from industrial_ops_agent.object_store.keys import tenant_object_key_from_value

    context = TenantContext(tenant_id=TENANT, subject_id="cli-test")
    key = tenant_object_key_from_value(context, object_key)
    return asyncio.run(LocalDiskTenantObjectStore(store_dir).get(context, key)) is not None


def _argv(*extra: str) -> list[str]:
    return ["--tenant-id", TENANT, *extra]


# ===========================================================================
# 拒绝运行：没有真实对象存储就什么都不做
# ===========================================================================


def test_refuses_without_object_store_dir(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """没配 `REHEARSAL_OBJECT_STORE_DIR` 时**必须**拒绝，不能兜底成 InMemory。

    兜底的后果不是报错而是撒谎：InMemory 里没有任何真实对象，回收会删掉行、
    报「deleted: N」，而磁盘上的字节一个没少。那比直接失败糟得多——失败会被
    发现，假绿灯不会。
    """

    monkeypatch.delenv("REHEARSAL_OBJECT_STORE_DIR", raising=False)

    with pytest.raises(SystemExit) as excinfo:
        cli.main(_argv("--retention-days", "7"))

    assert excinfo.value.code == 1, "拒绝运行必须是 1，不能是 0 也不能是可重试码"
    assert "REHEARSAL_OBJECT_STORE_DIR" in capsys.readouterr().err


def test_refusal_happens_before_touching_the_database(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """拒绝要在连库之前发生。

    否则同一个「少配了个环境变量」会先被报成数据库连不上，排查方向从一开始
    就是错的。这里把 Database 换成计数器，看它有没有被构造过。
    """

    monkeypatch.delenv("REHEARSAL_OBJECT_STORE_DIR", raising=False)
    constructed: list[str] = []
    monkeypatch.setattr(cli, "Database", lambda url: constructed.append(url))

    with pytest.raises(SystemExit):
        cli.main(_argv("--retention-days", "7"))

    assert constructed == [], "没有对象存储时不该构造数据库连接"


# ===========================================================================
# 正常路径：dry-run 与真删
# ===========================================================================


def test_dry_run_reports_and_exits_zero(
    database: Database,
    store_dir: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    _seed(database, store_dir, "snap-old", age_days=30)

    code = cli.main(_argv("--retention-days", "7"))

    payload = json.loads(capsys.readouterr().out)
    assert code == 0
    assert payload["apply"] is False
    assert payload["expired"] == 1
    assert payload["deleted"] == 0
    assert payload["remaining"] == 1
    assert _row_count(database) == 1, "dry-run 不许删任何行"


def test_dry_run_exits_zero_even_when_remaining_is_positive(
    database: Database, store_dir: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """dry-run 天然「还有剩余」，但那不是待办信号。

    如果 dry-run 在有剩余时返回可重试码，`while rc == 3: 再跑一轮` 的调度
    循环会在一条只读命令上永远转下去——而它跑一万轮结果都一样。
    """

    for index in range(3):
        _seed(database, store_dir, f"snap-old-{index}", age_days=30)

    code = cli.main(_argv("--retention-days", "7"))

    assert json.loads(capsys.readouterr().out)["remaining"] == 3
    assert code == 0, "dry-run 无论剩多少都必须返回 0"


def test_apply_deletes_objects_and_exits_zero_when_done(
    database: Database, store_dir: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    object_key = _seed(database, store_dir, "snap-old", age_days=30)

    code = cli.main(_argv("--retention-days", "7", "--apply"))

    payload = json.loads(capsys.readouterr().out)
    assert code == 0
    assert payload["apply"] is True
    assert payload["deleted"] == 1
    assert payload["remaining"] == 0
    assert _row_count(database) == 0
    assert not _object_exists(store_dir, object_key), "对象也必须真的被删掉"


def test_apply_exits_three_when_more_work_remains(
    database: Database, store_dir: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """还有剩余时返回 3（不是 2）。

    2 是 `argparse` 的用法错误码，且它由 `parse_args` 直接抛 `SystemExit`，
    走不到 `main()` 的返回值。两者混用，一个「参数写错就无限重试」的调度
    循环会在第一次部署时就出现。
    """

    for index in range(3):
        _seed(database, store_dir, f"snap-old-{index}", age_days=30)

    code = cli.main(_argv("--retention-days", "7", "--apply", "--batch-size", "2"))

    payload = json.loads(capsys.readouterr().out)
    assert payload["deleted"] == 2
    assert payload["remaining"] == 1
    assert code == 3, "还有剩余必须返回 3，且绝不能是 argparse 的 2"


def test_json_report_has_every_documented_field(
    database: Database, store_dir: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """报告字段就是调度器与人对账的全部依据，少一个字段就只能靠猜。"""

    _seed(database, store_dir, "snap-old", age_days=30)

    cli.main(_argv("--retention-days", "7"))

    payload = json.loads(capsys.readouterr().out)
    assert set(payload) == {
        "tenant_id",
        "apply",
        "cutoff",
        "expired",
        "deleted",
        "orphaned_objects",
        "remaining",
    }
    assert payload["tenant_id"] == TENANT
    # cutoff 必须是能被调度器直接解析的 ISO-8601，并且带时区。
    assert datetime.fromisoformat(payload["cutoff"]).tzinfo is not None


# ===========================================================================
# 参数被拒绝：命令自己的判定，与用法错误分开
# ===========================================================================


@pytest.mark.parametrize("retention_days", ["0", "-1"])
def test_non_positive_retention_exits_one(
    database: Database,
    store_dir: Path,
    capsys: pytest.CaptureFixture[str],
    retention_days: str,
) -> None:
    """保留期 ≤ 0 是**配置错误**，用 1 表达，不是 3。

    调度器对 1 的反应是「别重试，改配置」，对 3 的反应是「再跑一轮」。
    如果这里返回 3，一个把保留期写成 0 的部署会被反复重试，
    而每一次重试都在执行「删光」。
    """

    _seed(database, store_dir, "snap-old", age_days=30)

    code = cli.main(_argv("--retention-days", retention_days, "--apply"))

    assert code == 1
    assert "retention_days" in capsys.readouterr().err
    assert _row_count(database) == 1, "被拒绝时不许有任何删除"


def test_invalid_batch_size_exits_one(
    database: Database, store_dir: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    _seed(database, store_dir, "snap-old", age_days=30)

    code = cli.main(_argv("--retention-days", "7", "--apply", "--batch-size", "0"))

    assert code == 1
    assert "batch_size" in capsys.readouterr().err
    assert _row_count(database) == 1


def test_missing_required_argument_still_uses_two(
    store_dir: Path,
) -> None:
    """缺 `--tenant-id` 时由 `argparse` 自己退出，码是 2。

    这条不是为了「验回 argparse 的行为」，而是钉住**它没被我们改写**：
    一旦有人给 `ArgumentParser` 加 `exit_on_error=False` 或自己接住
    `SystemExit` 再映射成 3，调度器就会把用法错误当成「再跑一轮」。
    """

    with pytest.raises(SystemExit) as excinfo:
        cli.main(["--retention-days", "7"])

    assert excinfo.value.code == 2, "argparse 用法错误必须是 2，且不能被映射成可重试码"

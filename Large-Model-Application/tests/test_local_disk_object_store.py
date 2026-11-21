"""落盘对象存储 + 取回校验：把两条从未被检验过的约束钉死。

这份测试针对的是**内存实现掩盖掉的那部分语义**，所以它全部用真目录、
真文件、真 fsync，不 mock 文件系统。

三类风险，每一类在内存实现下都不会暴露：

1. **路径穿越**：对象键来自请求（``{image_id}`` 是板端生成的），一个 ``..``
   就能让写入落到根目录之外。内存实现的键是字典 key，根本不存在这个面。
2. **写入不原子**：断电留下半张 JPEG 比留下 0 个文件危险得多，
   因为它看起来像真图。
3. **取回内容不是当初那份**：这是留证链路唯一的价值所在。
   只有真落盘才能验「存进去 → 取回来 → 重算摘要」这条闭环。

异步约定：本项目没有 ``pytest-asyncio``（``requirements-test.txt`` 里也没有），
既有异步用例一律在同步测试里用 ``asyncio.run`` 驱动（见
``tests/test_video_clip_routes.py``）。这里沿用同一套，不额外引入依赖。
"""

from __future__ import annotations

import asyncio
import hashlib
import os
from collections.abc import Coroutine
from pathlib import Path
from typing import Any, TypeVar

import pytest

from industrial_ops_agent.object_store.keys import (
    ObjectZone,
    TenantObjectKey,
    edge_snapshot_image_object_key,
)
from industrial_ops_agent.object_store.local_disk import (
    DigestMismatchError,
    LocalDiskTenantObjectStore,
    verify_retrieved_digest,
)
from industrial_ops_agent.persistence.errors import TenantBoundaryViolation
from industrial_ops_agent.persistence.tenant import TenantContext

TENANT = TenantContext(tenant_id="tenant-m1-demo", subject_id="device-radxa")

_T = TypeVar("_T")


def _run(coro: Coroutine[Any, Any, _T]) -> _T:
    return asyncio.run(coro)


def _key(image_id: str) -> TenantObjectKey:
    return edge_snapshot_image_object_key(TENANT, image_id)


@pytest.fixture()
def store(tmp_path: Path) -> LocalDiskTenantObjectStore:
    return LocalDiskTenantObjectStore(tmp_path / "objects")


# ===========================================================================
# 基本读写：与内存实现的协议一致
# ===========================================================================


def test_put_then_get_returns_the_same_bytes(store: LocalDiskTenantObjectStore) -> None:
    payload = b"\xff\xd8\xff\xe0" + b"jpeg-body" * 100
    _run(store.put(TENANT, _key("snap-1"), payload))
    assert _run(store.get(TENANT, _key("snap-1"))) == payload


def test_get_missing_key_returns_none_not_raise(store: LocalDiskTenantObjectStore) -> None:
    # 协议约定「不存在」返回 None。抛异常会让调用方把「还没到」当成故障，
    # 而重试一份永远不会到的对象是纯浪费。
    assert _run(store.get(TENANT, _key("snap-never-uploaded"))) is None


def test_delete_removes_the_object(store: LocalDiskTenantObjectStore) -> None:
    _run(store.put(TENANT, _key("snap-del"), b"payload"))
    _run(store.delete(TENANT, _key("snap-del")))
    assert _run(store.get(TENANT, _key("snap-del"))) is None


def test_delete_missing_key_is_a_noop(store: LocalDiskTenantObjectStore) -> None:
    # 幂等删除：清理任务重跑不该因为「已经被删过」而失败。
    _run(store.delete(TENANT, _key("snap-gone")))


def test_put_overwrites_previous_content(store: LocalDiskTenantObjectStore) -> None:
    _run(store.put(TENANT, _key("snap-overwrite"), b"first"))
    _run(store.put(TENANT, _key("snap-overwrite"), b"second"))
    assert _run(store.get(TENANT, _key("snap-overwrite"))) == b"second"


# ===========================================================================
# 租户边界：与内存 / MinIO 实现同一条纪律
# ===========================================================================


def test_foreign_tenant_key_is_rejected_before_any_io(
    store: LocalDiskTenantObjectStore,
) -> None:
    other = TenantContext(tenant_id="tenant-other", subject_id="device-x")
    key = _key("snap-cross")
    _run(store.put(TENANT, key, b"payload"))

    # 用别的上下文读同一个键：必须在**碰文件之前**就被拒。
    with pytest.raises(TenantBoundaryViolation):
        _run(store.get(other, key))

    # 而且原始对象完全没被动过。
    assert _run(store.get(TENANT, key)) == b"payload"


# ===========================================================================
# 路径安全：这是内存实现完全不具备的攻击面
# ===========================================================================


@pytest.mark.parametrize(
    "hostile_value",
    [
        "tenant/tenant-m1-demo/quarantine/../../../etc/passwd",
        "tenant/tenant-m1-demo/./quarantine/x",
        "tenant//tenant-m1-demo/quarantine/x",
        "tenant/tenant-m1-demo/quarantine/..",
    ],
)
def test_hostile_key_cannot_escape_the_root(
    store: LocalDiskTenantObjectStore, hostile_value: str, tmp_path: Path
) -> None:
    # 这些键**绕过**了键构造器（真实代码里构造器会校验），所以这里直接
    # 手工拼一个 TenantObjectKey —— 防的是「将来某个调用方拿来了未经
    # 构造器校验的键」。存储实现必须自己守这条线，不能依赖上游。
    hostile = TenantObjectKey(
        tenant_id=TENANT.tenant_id, zone=ObjectZone.QUARANTINE, value=hostile_value
    )
    with pytest.raises(ValueError):
        _run(store.put(TENANT, hostile, b"pwned"))

    # 关键断言：没有任何东西被写到根目录之外。
    escaped = tmp_path / "etc" / "passwd"
    assert not escaped.exists()


def test_backslash_is_rejected(store: LocalDiskTenantObjectStore) -> None:
    # 反斜杠在 Linux 上不是分隔符，但放行它会让同一份键在不同平台
    # 指向不同位置。宁可拒绝，也不要让跨平台行为分叉。
    hostile = TenantObjectKey(
        tenant_id=TENANT.tenant_id,
        zone=ObjectZone.QUARANTINE,
        value="tenant/tenant-m1-demo/quarantine/snapshots\\..\\evil",
    )
    with pytest.raises(ValueError):
        _run(store.put(TENANT, hostile, b"pwned"))


def test_symlinked_parent_cannot_redirect_a_write(
    store: LocalDiskTenantObjectStore, tmp_path: Path
) -> None:
    # 逐段校验挡不住「根目录内的某个目录其实是软链接」。
    # 这条用例证明 realpath 之后的二次确认确实在工作。
    outside = tmp_path / "outside"
    outside.mkdir()

    # 在根目录里造一条指向外部目录的软链接，然后把键指进它。
    root = Path(store._root)  # noqa: SLF001 - 测试需要构造攻击面
    linked = root / "tenant" / TENANT.tenant_id / "quarantine"
    linked.parent.mkdir(parents=True, exist_ok=True)
    if linked.exists() or linked.is_symlink():
        linked.unlink()
    os.symlink(outside, linked)

    key = _key("snap-via-symlink")
    with pytest.raises(ValueError):
        _run(store.put(TENANT, key, b"pwned"))

    assert not (outside / "snapshot-images" / "snap-via-symlink").exists()


# ===========================================================================
# 原子性：不留半截文件
# ===========================================================================


def test_no_partial_file_survives_a_failed_write(
    store: LocalDiskTenantObjectStore, monkeypatch: pytest.MonkeyPatch
) -> None:
    # 模拟「写到一半炸了」。判据是**目标路径不存在**而不是「没有 .part」：
    # 真正危险的是半个 JPEG，不是残留的临时文件。
    key = _key("snap-half")

    def boom(*args: object, **kwargs: object) -> None:
        raise OSError("disk full")

    monkeypatch.setattr(os, "replace", boom)
    with pytest.raises(OSError):
        _run(store.put(TENANT, key, b"\xff\xd8half-a-jpeg"))

    assert _run(store.get(TENANT, key)) is None


def test_no_temp_files_left_behind_after_success(
    store: LocalDiskTenantObjectStore,
) -> None:
    _run(store.put(TENANT, _key("snap-clean"), b"payload"))
    leftovers = [
        p
        for p in Path(store._root).rglob("*")  # noqa: SLF001
        if p.is_file() and p.name.startswith(".tmp-")
    ]
    assert leftovers == [], f"临时文件泄漏: {leftovers}"


def test_concurrent_writes_to_one_key_never_interleave(
    store: LocalDiskTenantObjectStore,
) -> None:
    # 两个写者同抢一个键时，`os.replace` 保证**要么全是 A、要么全是 B**。
    # 内存实现下这个性质毫无意义（最后一次赋值胜出）；落盘实现下它靠的
    # 恰恰是 tmp + rename，所以值得钉住：一旦有人改成直接 `open(path,"wb")`，
    # 这里就会拿到 A 的头接 B 的尾。
    first = b"A" * 100_000
    second = b"B" * 100_000

    async def scenario() -> bytes | None:
        await asyncio.gather(
            store.put(TENANT, _key("snap-race"), first),
            store.put(TENANT, _key("snap-race"), second),
        )
        return await store.get(TENANT, _key("snap-race"))

    result = _run(scenario())
    assert result in (first, second), "写者之间发生了内容混合"


def test_object_lands_on_disk_at_the_expected_path(
    store: LocalDiskTenantObjectStore,
) -> None:
    # 运维要能 `ls` 出「这个租户有哪些证据」，所以布局必须可读：
    # 不做哈希化、不做扁平化。
    _run(store.put(TENANT, _key("snap-layout"), b"payload"))
    expected = (
        Path(store._root)  # noqa: SLF001
        / "tenant"
        / TENANT.tenant_id
        / "quarantine"
        / "snapshot-images"
        / "snap-layout"
    )
    assert expected.is_file()


# ===========================================================================
# 取回校验：留证链路唯一的价值所在
# ===========================================================================


def test_round_trip_digest_matches(store: LocalDiskTenantObjectStore) -> None:
    # 这条就是「不得宣称静帧证据可长期取回」那个缺口的补丁：
    # 写进去 → 取回来 → 重算 sha256 → 与声明值比。全部通过。
    payload = b"\xff\xd8\xff\xe0real-jpeg-bytes" * 50
    declared = hashlib.sha256(payload).hexdigest()

    _run(store.put(TENANT, _key("snap-roundtrip"), payload))
    retrieved = _run(store.get(TENANT, _key("snap-roundtrip")))

    assert retrieved is not None
    verify_retrieved_digest(retrieved, declared)


def test_round_trip_survives_a_new_store_instance(
    tmp_path: Path,
) -> None:
    # 「长期取回」的真正含义：**换一个进程**也能取回。同一实例内读回来
    # 可能只是命中了内存缓存，那就等于什么都没证明。
    payload = b"\xff\xd8persisted-across-instances"
    declared = hashlib.sha256(payload).hexdigest()
    root = tmp_path / "objects"

    writer = LocalDiskTenantObjectStore(root)
    _run(writer.put(TENANT, _key("snap-restart"), payload))

    reader = LocalDiskTenantObjectStore(root)  # 模拟服务重启后的新实例
    retrieved = _run(reader.get(TENANT, _key("snap-restart")))
    assert retrieved is not None
    verify_retrieved_digest(retrieved, declared)


def test_verify_digest_rejects_altered_content() -> None:
    # 「取回成功」不等于「取回的是当初那份」。这个区别是整条链路的意义。
    original = b"\xff\xd8original"
    declared = hashlib.sha256(original).hexdigest()
    tampered = b"\xff\xd8tampered"

    with pytest.raises(DigestMismatchError) as excinfo:
        verify_retrieved_digest(tampered, declared)
    assert "digest" in str(excinfo.value)


def test_verify_digest_accepts_empty_payload_only_if_declared() -> None:
    # 空内容的摘要是一个合法值。这里钉的是**判据本身不特判空**：
    # 空对象要不要接受由契约层决定（板端契约明确拒绝 0 字节），
    # 存储层的校验只负责「算出来的和声明的一样吗」。
    empty = b""
    verify_retrieved_digest(empty, hashlib.sha256(empty).hexdigest())
    with pytest.raises(DigestMismatchError):
        verify_retrieved_digest(empty, hashlib.sha256(b"x").hexdigest())

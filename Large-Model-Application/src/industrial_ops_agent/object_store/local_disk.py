"""Disk-backed tenant object store for rehearsal and durability tests.

为什么需要它
============

``InMemoryTenantObjectStore`` 只存在进程内存里，**重启即丢**。这让两条最
需要被检验的约束无法验证：

1. **写入顺序「对象存储 → 数据库」**。先写库再写对象会得到「行存在但内容
   不存在」——那一行会声称证据已送达，实际取不出内容。内存实现下对象永不
   失败，所以这个分支从来没被走到过。
2. **取回校验**。「把对象取回来，重算 sha256，与库里的 ``content_sha256``
   比对」是留证链路的最后一步。没有持久化后端，这一步做不了，于是
   「静帧证据可长期取回」这句话没有证据支撑。

为什么不直接用 MinIO
====================

MinIO 是真实平台的落点（``runtime.py`` 装配 ``MinioTenantObjectStore``），
但它需要独立服务与凭据。排练环境的全部意义是**一条最小闭环**，为它拉起
一个 S3 兼容服务会让「想跑一下」的成本从 ``python rehearsal_server.py``
变成一个部署任务。

所以这里落盘，而不是落 S3。

安全约束（这是本实现最容易写错的地方）
======================================

对象键来自请求（``tenant/{tid}/quarantine/snapshot-images/{image_id}``），
**不可信**。任何一个 ``..`` 都会让写入落到根目录之外。三重防护：

1. ``key.assert_owned_by(context)`` —— 租户边界，与内存/MinIO 实现一致；
2. 逐段校验键的每个 path 片段，拒绝 ``.`` / ``..`` / 空段 / 含分隔符；
3. ``os.path.realpath`` 之后**再次**确认结果仍在根目录内。

第 3 步不是多余的：前两步防的是「键写得坏」，第 3 步防的是「根目录本身
是软链接」或「中间某个目录被换成了软链接」。只做前两步的实现，
在攻击者能创建软链接时会被绕过。

为什么写文件用 tmp + ``os.replace``
====================================

``os.replace`` 在同一文件系统内是原子的。断电或进程被杀留下的是一个
``.tmp`` 文件，而不是**半张 JPEG**。半张图更危险，因为它看起来像真图——
下游会把它当证据读。
"""

from __future__ import annotations

import asyncio
import contextlib
import os
import tempfile
from pathlib import Path

from industrial_ops_agent.object_store.integrity import (
    DigestMismatchError,
    verify_retrieved_digest,
)
from industrial_ops_agent.object_store.keys import TenantObjectKey
from industrial_ops_agent.persistence.tenant import TenantContext

# ``DigestMismatchError`` 与 ``verify_retrieved_digest`` 定义在 ``integrity``
# 模块里（判据与存储介质无关）。这里重新导出，好让既有 ``from
# ...object_store.local_disk import ...`` 的调用方不必改 import —— 但新代码
# 应该直接 import ``integrity``，那才是它们真正的家。
__all__ = [
    "DigestMismatchError",
    "LocalDiskTenantObjectStore",
    "verify_retrieved_digest",
]


class LocalDiskTenantObjectStore:
    """把对象按键的层级结构落到一个本地目录下。

    目录布局与键逐段对应::

        <root>/tenant/<tid>/quarantine/snapshot-images/<image_id>

    刻意**不做**任何哈希化/扁平化：运维要能 ``ls`` 出「这个租户有哪些
    证据」。代价是文件名直接来自请求，所以路径校验必须严格（见模块注释）。
    """

    def __init__(self, root: str | os.PathLike[str]) -> None:
        self._root = Path(root).resolve()
        self._root.mkdir(parents=True, exist_ok=True)

    # ------------------------------------------------------------------
    # 协议实现
    # ------------------------------------------------------------------

    async def put(self, context: TenantContext, key: TenantObjectKey, content: bytes) -> None:
        key.assert_owned_by(context)
        path = self._path_for(key)
        await asyncio.to_thread(self._write_atomically, path, content)

    async def get(self, context: TenantContext, key: TenantObjectKey) -> bytes | None:
        key.assert_owned_by(context)
        path = self._path_for(key)
        return await asyncio.to_thread(self._read, path)

    async def delete(self, context: TenantContext, key: TenantObjectKey) -> None:
        key.assert_owned_by(context)
        path = self._path_for(key)
        await asyncio.to_thread(self._unlink, path)

    # ------------------------------------------------------------------
    # 路径解析：安全边界全在这里
    # ------------------------------------------------------------------

    def _path_for(self, key: TenantObjectKey) -> Path:
        """把对象键翻译成本地路径，并确认它没有跑出根目录。"""

        segments = key.value.split("/")
        for segment in segments:
            # 空段会产生 `//`（等价于 `/`，但说明键本身写坏了）；
            # `.` 与 `..` 是目录引用；分隔符已经由 split 处理掉了，
            # 但反斜杠在 Linux 上不是分隔符，仍可出现在文件名里——
            # 放行它会让同一份键在不同平台上指向不同位置。
            if segment in {"", ".", ".."} or "\\" in segment:
                raise ValueError("object key contains an unsafe path segment")

        candidate = self._root.joinpath(*segments).resolve()

        # 二次确认：即使前一步逐段检查过，`resolve()` 可能因软链接
        # 把结果带到根目录之外。`is_relative_to` 是这里唯一可靠的判据，
        # 不能用字符串前缀比较（`/data/objects-evil` 以 `/data/objects`
        # 为前缀，但完全在根目录之外）。
        if not candidate.is_relative_to(self._root):
            raise ValueError("object key escapes the object-store root")
        return candidate

    # ------------------------------------------------------------------
    # 同步 IO（由 to_thread 调用）
    # ------------------------------------------------------------------

    def _write_atomically(self, path: Path, content: bytes) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)

        # 临时文件必须与目标**同一目录**：跨文件系统的 `os.replace`
        # 会退化成复制 + 删除，不再原子。
        fd, tmp_name = tempfile.mkstemp(dir=path.parent, prefix=".tmp-", suffix=".part")
        try:
            with os.fdopen(fd, "wb") as handle:
                handle.write(content)
                handle.flush()
                # fsync 是「断电后内容还在」的唯一保证。少了它，rename
                # 成功但内容为空是真实发生过的（ext4 delayed allocation）。
                os.fsync(handle.fileno())
            os.replace(tmp_name, path)
        except BaseException:
            # 任何失败都不能留下 .part：它们会慢慢把磁盘塞满，
            # 而且看起来像"证据"，会误导取证。
            _unlink_quietly(tmp_name)
            raise

    def _read(self, path: Path) -> bytes | None:
        try:
            return path.read_bytes()
        except FileNotFoundError:
            return None

    def _unlink(self, path: Path) -> None:
        _unlink_quietly(path)


def _unlink_quietly(path: str | os.PathLike[str]) -> None:
    # 临时文件可能在异常路径上已经被删掉了，重复删除不该再抛一次。
    with contextlib.suppress(FileNotFoundError):
        os.unlink(path)

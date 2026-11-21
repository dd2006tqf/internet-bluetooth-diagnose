"""Deterministic object-store adapter for isolation and media tests."""

from __future__ import annotations

from datetime import UTC, datetime

from industrial_ops_agent.object_store.keys import TenantObjectKey
from industrial_ops_agent.persistence.tenant import TenantContext


class InMemoryTenantObjectStore:
    def __init__(self) -> None:
        self._objects: dict[str, bytes] = {}
        #: 每个键**首次**写入的时刻。与 ``_objects`` 分开存，因为静帧的键是
        #: 幂等键：重复上传会覆盖同一份字节，但那不是新对象。年龄必须按首次
        #: 写入算，否则一张被反复重传的孤儿会永远显得「刚写完」而永不被清扫。
        self._first_seen: dict[str, datetime] = {}

    async def put(
        self, context: TenantContext, key: TenantObjectKey, content: bytes
    ) -> None:
        key.assert_owned_by(context)
        self._objects[key.value] = content
        self._first_seen.setdefault(key.value, datetime.now(UTC))

    async def get(self, context: TenantContext, key: TenantObjectKey) -> bytes | None:
        key.assert_owned_by(context)
        return self._objects.get(key.value)

    async def delete(self, context: TenantContext, key: TenantObjectKey) -> None:
        key.assert_owned_by(context)
        self._objects.pop(key.value, None)
        self._first_seen.pop(key.value, None)

    async def list_prefix(
        self, context: TenantContext, prefix: str
    ) -> list[tuple[str, datetime]]:
        """按字面前缀枚举，返回 ``[(对象键, 首次写入时刻)]``。

        只给孤儿清扫器用（不在 ``TenantObjectStore`` 协议里）：上传/取回/删除
        三条主路径都不需要枚举，把它们被迫实现一遍没有收益。

        前缀必须落在本租户下，且这里只做**字面**匹配——与真实实现一样，调用方
        给出的前缀已经由 ``snapshot_images_prefix()`` 带上结尾斜杠。少了那个
        斜杠会匹到 ``snapshot-images-evil/...``，那是跨命名空间的误删。
        """

        tenant_prefix = f"tenant/{context.tenant_id}/"
        if not prefix.startswith(tenant_prefix):
            raise ValueError("object prefix is outside the active tenant")

        out: list[tuple[str, datetime]] = []
        for value in sorted(self._objects):
            if value.startswith(prefix):
                out.append((value, self._first_seen[value]))
        return out

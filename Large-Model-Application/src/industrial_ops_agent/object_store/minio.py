"""MinIO implementation guarded by canonical tenant object keys."""

from __future__ import annotations

import logging
from asyncio import to_thread
from collections.abc import Callable
from datetime import UTC, datetime
from io import BytesIO

from minio import Minio
from minio.error import MinioException, S3Error

from industrial_ops_agent.object_store.keys import TenantObjectKey
from industrial_ops_agent.persistence.errors import InfrastructureUnavailable
from industrial_ops_agent.persistence.tenant import TenantContext

LOGGER = logging.getLogger("industrial_ops_agent.object_store.minio")


class MinioTenantObjectStore:
    def __init__(self, client: Minio, bucket: str, *, attempts: int = 2) -> None:
        self._client = client
        self._bucket = bucket
        self._attempts = attempts

    async def put(
        self, context: TenantContext, key: TenantObjectKey, content: bytes
    ) -> None:
        key.assert_owned_by(context)

        def operation() -> None:
            self._client.put_object(
                self._bucket,
                key.value,
                BytesIO(content),
                length=len(content),
                content_type="application/octet-stream",
            )

        await self._run(operation)

    async def get(self, context: TenantContext, key: TenantObjectKey) -> bytes | None:
        key.assert_owned_by(context)

        def operation() -> bytes | None:
            try:
                response = self._client.get_object(self._bucket, key.value)
            except S3Error as exc:
                if exc.code in {"NoSuchKey", "NoSuchObject"}:
                    return None
                raise
            try:
                return response.read()
            finally:
                response.close()
                response.release_conn()

        result = await self._run(operation)
        if result is not None and not isinstance(result, bytes):
            raise TypeError("object-store read returned an invalid payload")
        return result

    async def delete(self, context: TenantContext, key: TenantObjectKey) -> None:
        key.assert_owned_by(context)
        await self._run(lambda: self._client.remove_object(self._bucket, key.value))

    async def list_prefix(
        self, context: TenantContext, prefix: str
    ) -> list[tuple[str, datetime]]:
        """按前缀枚举对象，返回 ``[(对象键, last_modified)]``。

        只给孤儿清扫器用（不在 ``TenantObjectStore`` 协议里）：上传/取回/删除
        三条主路径都不需要枚举。

        ``recursive=True`` 且**不带** ``start_after``：静帧键是
        ``.../snapshot-images/<image_id>``，一层目录一个对象。MinIO 的
        ``list_objects`` 默认在 ``/`` 处折叠（返回的是「前缀」而非对象），那会让
        清扫器看到一堆以 ``/`` 结尾的伪键，而它们**不是对象**——按对象删会失败，
        按前缀删则会一次删掉整段。显式递归才是「列出对象」的语义。

        ``last_modified`` 缺失时（部分 S3 兼容实现不填）退回 ``datetime.min``：
        那会让该对象**立刻**满足时间窗，从而被当作候选——这是危险方向（宁可
        少删）。所以这里用 ``min`` 之外的做法：缺失即跳过并告警，让「枚举实现
        不完整」暴露出来，而不是让它悄悄把整批对象判成孤儿。
        """

        tenant_prefix = f"tenant/{context.tenant_id}/"
        if not prefix.startswith(tenant_prefix):
            raise ValueError("object prefix is outside the active tenant")

        def operation() -> list[tuple[str, datetime]]:
            out: list[tuple[str, datetime]] = []
            for item in self._client.list_objects(
                self._bucket, prefix=prefix, recursive=True
            ):
                name = getattr(item, "object_name", "") or ""
                if not name:
                    continue
                modified = getattr(item, "last_modified", None)
                if modified is None:
                    LOGGER.warning(
                        "object store listed an object without last_modified: key=%s "
                        "(skipped: cannot judge its age, and guessing would risk "
                        "treating a fresh upload as an orphan)",
                        name,
                    )
                    continue
                if modified.tzinfo is None:
                    modified = modified.replace(tzinfo=UTC)
                out.append((name, modified))
            return out

        listed = await self._run(operation)
        if not isinstance(listed, list):
            raise TypeError("object-store listing returned an invalid payload")
        return listed

    async def _run(self, operation: Callable[[], object]) -> object:
        for attempt in range(1, self._attempts + 1):
            try:
                return await to_thread(operation)
            except (MinioException, OSError) as exc:
                if attempt == self._attempts:
                    raise InfrastructureUnavailable("object_store") from exc
        raise AssertionError("bounded retry loop did not terminate")

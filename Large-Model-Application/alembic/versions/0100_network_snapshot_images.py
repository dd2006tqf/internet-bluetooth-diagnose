"""Add network_snapshot_images for edge evidence still images.

Revision ID: 0100_network_snapshot_images
Revises: 0099_network_video_clips
Create Date: 2026-11-20

一张图一行，而不是像 ``network_video_clips`` + ``network_video_clip_chunks``
那样的一对多。静帧没有分片，也没有「收齐了没有」这个状态；把它塞进片段那对
表会让 ``expected_chunks``/``fps`` 在图片行上变成无意义的占位值，而占位值
会被下游当事实读。

``image_id`` 是板端生成的幂等键，直接做主键。对象存储键存在行里（与分片表
同一个理由：内容在对象存储，这里只留索引与摘要）。
"""

from __future__ import annotations

from collections.abc import Sequence
from typing import Any

import sqlalchemy as sa
from alembic import op

revision: str = "0100_network_snapshot_images"
down_revision: str | None = "0099_network_video_clips"
branch_labels: str | Sequence[str] | None = None
depends_on: str | Sequence[str] | None = None


def _timestamps() -> list[sa.Column[Any]]:
    return [
        sa.Column(name, sa.DateTime(timezone=True), nullable=False, server_default=sa.func.now())
        for name in ("created_at", "updated_at")
    ]


def _tenant_isolation(table: str) -> None:
    """启用并强制行级安全，策略名按表名派生（与既有 migration 一致）。"""

    predicate = "tenant_id = current_setting('app.tenant_id', true)"
    policy_name = f"{table}_tenant_isolation"
    op.execute(f"ALTER TABLE {table} ENABLE ROW LEVEL SECURITY")
    op.execute(f"ALTER TABLE {table} FORCE ROW LEVEL SECURITY")
    op.execute(
        f"CREATE POLICY {policy_name} ON {table} "
        f"USING ({predicate}) WITH CHECK ({predicate})"
    )


def upgrade() -> None:
    op.create_table(
        "network_snapshot_images",
        sa.Column("image_id", sa.String(128), primary_key=True),
        sa.Column("tenant_id", sa.String(64), sa.ForeignKey("tenants.id"), nullable=False),
        sa.Column("asset_id", sa.String(128), nullable=False),
        sa.Column("site_id", sa.String(128), nullable=False, server_default=""),
        sa.Column("gateway_id", sa.String(128), nullable=False, server_default=""),
        sa.Column("incident_id", sa.String(128), nullable=False, server_default=""),
        sa.Column("codec", sa.String(16), nullable=False, server_default="JPEG"),
        sa.Column("width", sa.Integer(), nullable=False),
        sa.Column("height", sa.Integer(), nullable=False),
        sa.Column("captured_at_ms", sa.BigInteger(), nullable=False),
        sa.Column("size_bytes", sa.Integer(), nullable=False),
        sa.Column("content_sha256", sa.String(64), nullable=False),
        sa.Column("object_key", sa.String(512), nullable=False),
        sa.Column("received_at", sa.DateTime(timezone=True), nullable=False),
        *_timestamps(),
    )
    op.create_index(
        "ix_network_snapshot_images_asset",
        "network_snapshot_images",
        ["tenant_id", "asset_id", "captured_at_ms"],
    )
    op.create_index(
        "ix_network_snapshot_images_incident",
        "network_snapshot_images",
        ["tenant_id", "incident_id"],
    )

    _tenant_isolation("network_snapshot_images")


def downgrade() -> None:
    op.execute(
        "DROP POLICY IF EXISTS network_snapshot_images_tenant_isolation "
        "ON network_snapshot_images"
    )
    op.drop_index(
        "ix_network_snapshot_images_incident", table_name="network_snapshot_images"
    )
    op.drop_index("ix_network_snapshot_images_asset", table_name="network_snapshot_images")
    op.drop_table("network_snapshot_images")

"""Add network_video_clips / network_video_clip_chunks for edge evidence uploads.

Revision ID: 0099_network_video_clips
Revises: 0098_network_edge_hypotheses
Create Date: 2026-11-14

两张表是一对：清单登记「有这么一段留证视频」，分片登记「它的第 N 块已经
落盘」。刻意**不**把 ``received_chunks`` / ``state`` 做成清单表上的列——那
两个值都能由分片表推出，存成列就多一个必须与之同步的计数器，少写一次同步
就是一个永远卡在 PENDING 的片段。代价是一次带索引的 count。

分片的复合主键 ``(tenant_id, clip_id, chunk_index)`` 就是幂等保证：两个并发
的同片提交不能都插入。服务层那次预检只让 ``duplicate`` 计数准确，正确性
不依赖它。
"""

from __future__ import annotations

from collections.abc import Sequence
from typing import Any

import sqlalchemy as sa
from alembic import op

revision: str = "0099_network_video_clips"
down_revision: str | None = "0098_network_edge_hypotheses"
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
        "network_video_clips",
        sa.Column("clip_id", sa.String(128), primary_key=True),
        sa.Column("tenant_id", sa.String(64), sa.ForeignKey("tenants.id"), nullable=False),
        sa.Column("asset_id", sa.String(128), nullable=False),
        sa.Column("site_id", sa.String(128), nullable=False, server_default=""),
        sa.Column("gateway_id", sa.String(128), nullable=False, server_default=""),
        sa.Column("incident_id", sa.String(128), nullable=False, server_default=""),
        sa.Column("codec", sa.String(16), nullable=False, server_default="H264"),
        sa.Column("width", sa.Integer(), nullable=False),
        sa.Column("height", sa.Integer(), nullable=False),
        sa.Column("fps", sa.Integer(), nullable=False, server_default="25"),
        sa.Column("started_at_ms", sa.BigInteger(), nullable=False),
        sa.Column("ended_at_ms", sa.BigInteger(), nullable=False),
        sa.Column("total_bytes", sa.BigInteger(), nullable=False),
        sa.Column("expected_chunks", sa.Integer(), nullable=False),
        sa.Column(
            "declared_content_sha256", sa.String(64), nullable=False, server_default=""
        ),
        sa.Column("received_at", sa.DateTime(timezone=True), nullable=False),
        *_timestamps(),
    )
    op.create_index(
        "ix_network_video_clips_asset",
        "network_video_clips",
        ["tenant_id", "asset_id", "started_at_ms"],
    )
    op.create_index(
        "ix_network_video_clips_incident",
        "network_video_clips",
        ["tenant_id", "incident_id"],
    )

    op.create_table(
        "network_video_clip_chunks",
        sa.Column("tenant_id", sa.String(64), sa.ForeignKey("tenants.id"), nullable=False),
        sa.Column("clip_id", sa.String(128), nullable=False),
        sa.Column("chunk_index", sa.Integer(), nullable=False),
        sa.Column("size_bytes", sa.Integer(), nullable=False),
        sa.Column("content_sha256", sa.String(64), nullable=False),
        sa.Column("object_key", sa.String(512), nullable=False),
        sa.Column("received_at", sa.DateTime(timezone=True), nullable=False),
        *_timestamps(),
        sa.PrimaryKeyConstraint("tenant_id", "clip_id", "chunk_index"),
        sa.ForeignKeyConstraint(
            ["clip_id"],
            ["network_video_clips.clip_id"],
            name="fk_network_video_clip_chunks_clip",
        ),
    )

    _tenant_isolation("network_video_clips")
    _tenant_isolation("network_video_clip_chunks")


def downgrade() -> None:
    for table in ("network_video_clip_chunks", "network_video_clips"):
        op.execute(f"DROP POLICY IF EXISTS {table}_tenant_isolation ON {table}")
    op.drop_table("network_video_clip_chunks")
    op.drop_index("ix_network_video_clips_incident", table_name="network_video_clips")
    op.drop_index("ix_network_video_clips_asset", table_name="network_video_clips")
    op.drop_table("network_video_clips")

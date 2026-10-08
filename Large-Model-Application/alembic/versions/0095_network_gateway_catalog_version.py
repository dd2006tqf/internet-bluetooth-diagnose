"""Add network_gateway_catalog_versions —— 网关自述动作目录版本（漂移检测输入源）。

Revision ID: 0095_network_gateway_catalog_version
Revises: 0094_network_action_outcomes
Create Date: 2026-10-08

`action_policy` 早已实现「云端审阅目录 != 网关宣称目录 → allowed=False」的
fail-closed 语义，但真实运行中它**永不触发**：`build_request_from_tables()`
从不设置 `gateway_catalog_version`（恒取默认值），板端也从不上报。本表补上
这条缺失的输入源——网关自述其 L5 可执行目录的指纹，云端据实对比。

只存"设备声称了什么"这一事实，不做任何授权判断（授权仍在 Policy）。
"""

from __future__ import annotations

from collections.abc import Sequence
from typing import Any

import sqlalchemy as sa
from alembic import op

revision: str = "0095_network_gateway_catalog_version"
down_revision: str | None = "0094_network_action_outcomes"
branch_labels: str | Sequence[str] | None = None
depends_on: str | Sequence[str] | None = None


def _timestamps() -> list[sa.Column[Any]]:
    # TenantScopedMixin 继承 TimestampMixin → 模型有 created_at + updated_at 两列。
    # 迁移必须与之逐列对齐（0089 的教训：少建 updated_at 会让每条查询在 PG 上 500）。
    return [
        sa.Column(name, sa.DateTime(timezone=True), nullable=False, server_default=sa.func.now())
        for name in ("created_at", "updated_at")
    ]


def upgrade() -> None:
    op.create_table(
        "network_gateway_catalog_versions",
        sa.Column("tenant_id", sa.String(64), sa.ForeignKey("tenants.id"), nullable=False),
        sa.Column("device_id", sa.String(128), nullable=False),
        sa.Column("catalog_version", sa.String(64), nullable=False),
        #: 云端最近一次收到该设备自述版本的时刻（覆盖式更新，保留最新事实）
        sa.Column("reported_at", sa.DateTime(timezone=True), nullable=False),
        *_timestamps(),
        sa.PrimaryKeyConstraint("tenant_id", "device_id", name="pk_network_gateway_catalog"),
    )

    predicate = "tenant_id = current_setting('app.tenant_id', true)"
    policy_name = "network_gateway_catalog_versions_tenant_isolation"
    op.execute("ALTER TABLE network_gateway_catalog_versions ENABLE ROW LEVEL SECURITY")
    op.execute("ALTER TABLE network_gateway_catalog_versions FORCE ROW LEVEL SECURITY")
    op.execute(
        f"CREATE POLICY {policy_name} ON network_gateway_catalog_versions "
        f"USING ({predicate}) WITH CHECK ({predicate})"
    )


def downgrade() -> None:
    op.execute(
        "DROP POLICY IF EXISTS network_gateway_catalog_versions_tenant_isolation "
        "ON network_gateway_catalog_versions"
    )
    op.drop_table("network_gateway_catalog_versions")

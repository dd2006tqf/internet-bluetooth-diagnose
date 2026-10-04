"""Add network_action_outcomes —— L1 权威事实投影：终态执行回执（路线图 ⑥ T0b）。

Revision ID: 0094_network_action_outcomes
Revises: 0093_network_action_approvals
Create Date: 2026-10-04

`network_pending_actions` 是 L5 执行域**队列状态**（QUEUED→DELIVERED→
APPLIED/REJECTED/ROLLBACK）；本表把同一事件投影为 **L1 事实**："某次动作
在网关上被报告执行，结果是什么"。同事务写入（`record_action_results`），
append-only，`UNIQUE(tenant_id, pending_action_id)` 保证一个执行实例
只有一条终态事实。

同时给 `network_pending_actions` 补 `proposal_id`/`approval_id` 两列
（L4 决策链 provenance；手工路径为 NULL——事实，不是"不安全"的判断）。
"""

from __future__ import annotations

from collections.abc import Sequence
from typing import Any

import sqlalchemy as sa
from alembic import op

revision: str = "0094_network_action_outcomes"
down_revision: str | None = "0093_network_action_approvals"
branch_labels: str | Sequence[str] | None = None
depends_on: str | Sequence[str] | None = None


def _timestamps() -> list[sa.Column[Any]]:
    return [
        sa.Column(name, sa.DateTime(timezone=True), nullable=False, server_default=sa.func.now())
        for name in ("created_at", "updated_at")
    ]


def upgrade() -> None:
    # 1) pending_actions 补 provenance（可空：历史行与手工路径都没有）
    op.add_column(
        "network_pending_actions",
        sa.Column("proposal_id", sa.String(128), nullable=True),
    )
    op.add_column(
        "network_pending_actions",
        sa.Column("approval_id", sa.String(128), nullable=True),
    )

    # 2) L1 事实表
    op.create_table(
        "network_action_outcomes",
        sa.Column("outcome_id", sa.String(128), primary_key=True),
        sa.Column("tenant_id", sa.String(64), sa.ForeignKey("tenants.id"), nullable=False),
        sa.Column("pending_action_id", sa.String(128), nullable=False),
        sa.Column("asset_id", sa.String(128), nullable=False),
        sa.Column("proposal_id", sa.String(128), nullable=True),
        sa.Column("approval_id", sa.String(128), nullable=True),
        sa.Column("action_snapshot", sa.JSON(), nullable=False),
        sa.Column("action_payload_digest", sa.String(128), nullable=False),
        sa.Column("status", sa.String(16), nullable=False),
        sa.Column("result_detail", sa.String(1024), nullable=True),
        sa.Column("completed_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("recorded_at", sa.DateTime(timezone=True), nullable=False),
        *_timestamps(),
        sa.UniqueConstraint(
            "tenant_id", "pending_action_id",
            name="uq_network_action_outcome_pending_action",
        ),
        sa.CheckConstraint(
            "status IN ('APPLIED', 'REJECTED', 'ROLLBACK')",
            name="ck_network_action_outcome_status",
        ),
    )
    op.create_index(
        "ix_network_action_outcomes_asset",
        "network_action_outcomes",
        ["tenant_id", "asset_id", "completed_at"],
    )

    # 3) RLS（与既有 tenant 表同一模式）
    predicate = "tenant_id = current_setting('app.tenant_id', true)"
    policy_name = "network_action_outcomes_tenant_isolation"
    op.execute("ALTER TABLE network_action_outcomes ENABLE ROW LEVEL SECURITY")
    op.execute("ALTER TABLE network_action_outcomes FORCE ROW LEVEL SECURITY")
    op.execute(
        f"CREATE POLICY {policy_name} ON network_action_outcomes "
        f"USING ({predicate}) WITH CHECK ({predicate})"
    )


def downgrade() -> None:
    op.execute(
        "DROP POLICY IF EXISTS network_action_outcomes_tenant_isolation "
        "ON network_action_outcomes"
    )
    op.drop_index(
        "ix_network_action_outcomes_asset", table_name="network_action_outcomes"
    )
    op.drop_table("network_action_outcomes")
    op.drop_column("network_pending_actions", "approval_id")
    op.drop_column("network_pending_actions", "proposal_id")

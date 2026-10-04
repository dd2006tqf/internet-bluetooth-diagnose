"""Add Network Council proposal / decision / approval tables (路线图 ⑤).

Revision ID: 0093_network_action_approvals
Revises: 0092_network_council
Create Date: 2026-10-04

Four tables, mirroring the E1 → E2 state machine:

- ``network_council_proposals``          —— 所有 E1 提案（含 BLOCKED）的稳定身份
- ``network_action_decisions``            —— Policy 裁决（append-only，Policy 升版可重裁）
- ``network_action_approvals``            —— 人工审批（批准对象 = decision，不是 LLM 原文）
- ``network_action_approval_decisions``   —— 人的批准留痕（append-only）

TimestampMixin（经 TenantScopedMixin）同时声明 created_at 与 updated_at ——
0089 的教训：迁移漏建任一列会让 PostgreSQL 上每条 ORM SELECT 失败。
"""

from __future__ import annotations

from collections.abc import Sequence
from typing import Any

import sqlalchemy as sa
from alembic import op

revision: str = "0093_network_action_approvals"
down_revision: str | None = "0092_network_council"
branch_labels: str | Sequence[str] | None = None
depends_on: str | Sequence[str] | None = None

_TENANT_TABLES = (
    "network_council_proposals",
    "network_action_decisions",
    "network_action_approvals",
    "network_action_approval_decisions",
)


def _timestamps() -> list[sa.Column[Any]]:
    return [
        sa.Column(name, sa.DateTime(timezone=True), nullable=False, server_default=sa.func.now())
        for name in ("created_at", "updated_at")
    ]


def upgrade() -> None:
    op.create_table(
        "network_council_proposals",
        sa.Column("proposal_id", sa.String(128), primary_key=True),
        sa.Column("tenant_id", sa.String(64), sa.ForeignKey("tenants.id"), nullable=False),
        sa.Column("council_id", sa.String(128), nullable=False),
        sa.Column("attempt", sa.Integer(), nullable=False),
        sa.Column("proposal_index", sa.Integer(), nullable=False),
        sa.Column("proposal_snapshot", sa.JSON(), nullable=False),
        sa.Column("proposal_digest", sa.String(128), nullable=False),
        *_timestamps(),
        sa.UniqueConstraint(
            "tenant_id", "council_id", "attempt", "proposal_index",
            name="uq_network_council_proposal_position",
        ),
    )
    op.create_index(
        "ix_network_council_proposals_council",
        "network_council_proposals",
        ["tenant_id", "council_id", "attempt"],
    )

    op.create_table(
        "network_action_decisions",
        sa.Column("decision_id", sa.String(128), primary_key=True),
        sa.Column("tenant_id", sa.String(64), sa.ForeignKey("tenants.id"), nullable=False),
        sa.Column("proposal_id", sa.String(128), nullable=False),
        sa.Column("council_id", sa.String(128), nullable=False),
        sa.Column("attempt", sa.Integer(), nullable=False),
        sa.Column("proposal_digest", sa.String(128), nullable=False),
        sa.Column("normalized_action", sa.JSON(), nullable=False),
        sa.Column("normalized_action_digest", sa.String(128), nullable=False),
        sa.Column("allowed", sa.Boolean(), nullable=False),
        sa.Column("risk", sa.String(8), nullable=False),
        sa.Column("required_preconditions", sa.JSON(), nullable=False, server_default="[]"),
        sa.Column("approval_required", sa.Boolean(), nullable=False),
        sa.Column("block_reason", sa.Text(), nullable=True),
        sa.Column("execution_mode", sa.String(32), nullable=False),
        sa.Column("catalog_version", sa.String(64), nullable=False),
        sa.Column("risk_policy_version", sa.String(64), nullable=False),
        sa.Column("decision_digest", sa.String(128), nullable=False),
        *_timestamps(),
    )
    op.create_index(
        "ix_network_action_decisions_proposal",
        "network_action_decisions",
        ["tenant_id", "proposal_id"],
    )
    op.create_index(
        "ix_network_action_decisions_council",
        "network_action_decisions",
        ["tenant_id", "council_id"],
    )

    op.create_table(
        "network_action_approvals",
        sa.Column("approval_id", sa.String(128), primary_key=True),
        sa.Column("tenant_id", sa.String(64), sa.ForeignKey("tenants.id"), nullable=False),
        sa.Column("proposal_id", sa.String(128), nullable=False),
        sa.Column("council_id", sa.String(128), nullable=False),
        sa.Column("attempt", sa.Integer(), nullable=False),
        sa.Column("decision_id", sa.String(128), nullable=False),
        sa.Column("decision_digest", sa.String(128), nullable=False),
        sa.Column("normalized_action", sa.JSON(), nullable=False),
        sa.Column("approval_payload_digest", sa.String(128), nullable=False),
        sa.Column("runbook_renderer_version", sa.String(64), nullable=False),
        sa.Column("status", sa.String(16), nullable=False, server_default="PENDING"),
        sa.Column("initiated_by", sa.String(128), nullable=False),
        sa.Column("expires_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("version", sa.Integer(), nullable=False, server_default="1"),
        sa.Column("execution_status", sa.String(24), nullable=False, server_default="NOT_EXECUTED"),
        sa.Column("manual_execution_required", sa.Boolean(), nullable=False),
        sa.Column("queued_action_id", sa.String(128), nullable=True),
        sa.Column("executed_idempotency_key", sa.String(64), nullable=True),
        sa.Column("executed_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column("approved_by_subject_id", sa.String(128), nullable=True),
        sa.Column("approved_at", sa.DateTime(timezone=True), nullable=True),
        *_timestamps(),
        sa.UniqueConstraint(
            "tenant_id", "proposal_id", name="uq_network_action_approval_proposal"
        ),
        sa.UniqueConstraint(
            "tenant_id", "executed_idempotency_key",
            name="uq_network_action_approval_idempotency",
        ),
        sa.CheckConstraint(
            "status IN ('PENDING', 'APPROVED', 'REJECTED', 'EXPIRED', 'SUPERSEDED')",
            name="ck_network_action_approval_status",
        ),
        sa.CheckConstraint(
            "execution_status IN ('NOT_EXECUTED', 'QUEUED')",
            name="ck_network_action_approval_execution_status",
        ),
        sa.CheckConstraint("version >= 1", name="ck_network_action_approval_version"),
    )
    op.create_index(
        "ix_network_action_approvals_council",
        "network_action_approvals",
        ["tenant_id", "council_id", "status"],
    )

    op.create_table(
        "network_action_approval_decisions",
        sa.Column("approval_decision_id", sa.String(128), primary_key=True),
        sa.Column("tenant_id", sa.String(64), sa.ForeignKey("tenants.id"), nullable=False),
        sa.Column("approval_id", sa.String(128), nullable=False),
        sa.Column("approval_decision", sa.String(16), nullable=False),
        sa.Column("reason", sa.Text(), nullable=False),
        sa.Column("decider_subject_id", sa.String(128), nullable=False),
        sa.Column("approval_payload_digest", sa.String(128), nullable=False),
        sa.Column("decided_at", sa.DateTime(timezone=True), nullable=False),
        *_timestamps(),
    )
    op.create_index(
        "ix_network_action_approval_decisions",
        "network_action_approval_decisions",
        ["tenant_id", "approval_id", "decided_at"],
    )
    op.create_index(
        "ix_network_action_approval_decisions_approval",
        "network_action_approval_decisions",
        ["approval_id"],
    )

    predicate = "tenant_id = current_setting('app.tenant_id', true)"
    for table_name in _TENANT_TABLES:
        policy_name = f"{table_name}_tenant_isolation"[:63]
        op.execute(f"ALTER TABLE {table_name} ENABLE ROW LEVEL SECURITY")
        op.execute(f"ALTER TABLE {table_name} FORCE ROW LEVEL SECURITY")
        op.execute(
            f"CREATE POLICY {policy_name} ON {table_name} "
            f"USING ({predicate}) WITH CHECK ({predicate})"
        )


def downgrade() -> None:
    for table_name in reversed(_TENANT_TABLES):
        policy_name = f"{table_name}_tenant_isolation"[:63]
        op.execute(f"DROP POLICY IF EXISTS {policy_name} ON {table_name}")
    op.drop_index(
        "ix_network_action_approval_decisions_approval",
        table_name="network_action_approval_decisions",
    )
    op.drop_index(
        "ix_network_action_approval_decisions",
        table_name="network_action_approval_decisions",
    )
    op.drop_table("network_action_approval_decisions")
    op.drop_index(
        "ix_network_action_approvals_council", table_name="network_action_approvals"
    )
    op.drop_table("network_action_approvals")
    op.drop_index(
        "ix_network_action_decisions_council", table_name="network_action_decisions"
    )
    op.drop_index(
        "ix_network_action_decisions_proposal", table_name="network_action_decisions"
    )
    op.drop_table("network_action_decisions")
    op.drop_index(
        "ix_network_council_proposals_council", table_name="network_council_proposals"
    )
    op.drop_table("network_council_proposals")

"""Add the Network Operations Council tables (路线图 ④).

Revision ID: 0092_network_council
Revises: 0091_network_device_baseline_history
Create Date: 2026-10-04

Two tables:

- ``network_councils``            —— 会商生命周期 + E1 建议清单（ActionProposal[]）
- ``network_council_contributions`` —— 每个角色每轮的贡献（append-only 审计）

**刻意没有** approved_by / approved_at / execution 列：批准与执行属于路线图 ⑤，
Council 记录在结构上就不表达它们。

TimestampMixin（经 TenantScopedMixin）同时声明 created_at 与 updated_at ——
0089 的教训：迁移漏建任一列会让 PostgreSQL 上每条 ORM SELECT 失败，而
SQLite 单测（用 Base.metadata.create_all 建表）永远发现不了。
"""

from __future__ import annotations

from collections.abc import Sequence
from typing import Any

import sqlalchemy as sa
from alembic import op

revision: str = "0092_network_council"
down_revision: str | None = "0091_network_device_baseline_history"
branch_labels: str | Sequence[str] | None = None
depends_on: str | Sequence[str] | None = None

_COUNCILS = "network_councils"
_CONTRIBUTIONS = "network_council_contributions"
_TENANT_TABLES = (_COUNCILS, _CONTRIBUTIONS)


def _timestamps() -> list[sa.Column[Any]]:
    return [
        sa.Column(name, sa.DateTime(timezone=True), nullable=False, server_default=sa.func.now())
        for name in ("created_at", "updated_at")
    ]


def upgrade() -> None:
    op.create_table(
        _COUNCILS,
        sa.Column("council_id", sa.String(128), primary_key=True),
        sa.Column("tenant_id", sa.String(64), sa.ForeignKey("tenants.id"), nullable=False),
        sa.Column("incident_id", sa.String(128), nullable=False),
        sa.Column("input_fingerprint", sa.String(128), nullable=False),
        sa.Column("status", sa.String(32), nullable=False, server_default="QUEUED"),
        sa.Column("stage", sa.String(64), nullable=False, server_default="QUEUED"),
        sa.Column("attempt_count", sa.Integer(), nullable=False, server_default="0"),
        sa.Column("proposals_json", sa.JSON(), nullable=True),
        sa.Column("expert_opinions_json", sa.JSON(), nullable=True),
        sa.Column("failure_code", sa.String(128), nullable=True),
        sa.Column("requested_by_subject_id", sa.String(128), nullable=False),
        sa.Column("completed_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column("version", sa.Integer(), nullable=False, server_default="1"),
        *_timestamps(),
        sa.UniqueConstraint(
            "tenant_id", "incident_id", "input_fingerprint", name="uq_network_council_input"
        ),
        sa.CheckConstraint(
            "status IN ('QUEUED', 'RUNNING', 'REVIEW_PENDING', 'FAILED')",
            name="ck_network_council_status",
        ),
        sa.CheckConstraint(
            "attempt_count >= 0 AND version >= 1", name="ck_network_council_versions"
        ),
    )
    op.create_index(
        "ix_network_councils_incident", _COUNCILS, ["tenant_id", "incident_id", "updated_at"]
    )

    op.create_table(
        _CONTRIBUTIONS,
        sa.Column("contribution_id", sa.String(128), primary_key=True),
        sa.Column("tenant_id", sa.String(64), sa.ForeignKey("tenants.id"), nullable=False),
        sa.Column("council_id", sa.String(128), nullable=False),
        sa.Column("attempt_number", sa.Integer(), nullable=False),
        sa.Column("agent_role", sa.String(32), nullable=False),
        sa.Column("input_digest", sa.String(128), nullable=False),
        sa.Column("output_json", sa.JSON(), nullable=False),
        sa.Column("output_digest", sa.String(128), nullable=False),
        sa.Column("model_release_id", sa.String(128), nullable=True),
        sa.Column("prompt_bundle_hash", sa.String(128), nullable=True),
        sa.Column("completed_at", sa.DateTime(timezone=True), nullable=False),
        *_timestamps(),
        sa.UniqueConstraint(
            "tenant_id",
            "council_id",
            "attempt_number",
            "agent_role",
            name="uq_network_council_contribution_role_attempt",
        ),
        sa.CheckConstraint(
            "agent_role IN ('RF_SPECTRUM', 'KERNEL_STACK', 'OPS_SAFETY', 'COORDINATOR')",
            name="ck_network_council_contribution_role",
        ),
        sa.CheckConstraint(
            "attempt_number >= 1", name="ck_network_council_contribution_attempt"
        ),
    )
    op.create_index(
        "ix_network_council_contribution_council",
        _CONTRIBUTIONS,
        ["tenant_id", "council_id", "attempt_number"],
    )

    # 与 0090/0091 同款租户隔离
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
    op.drop_index("ix_network_council_contribution_council", table_name=_CONTRIBUTIONS)
    op.drop_table(_CONTRIBUTIONS)
    op.drop_index("ix_network_councils_incident", table_name=_COUNCILS)
    op.drop_table(_COUNCILS)

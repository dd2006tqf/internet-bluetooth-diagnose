"""Add network_edge_hypotheses table for T4 L2 edge AI hypotheses.

Revision ID: 0098_network_edge_hypotheses
Revises: 0097_network_action_outcome_local_origin
Create Date: 2026-10-11
"""

from __future__ import annotations

from collections.abc import Sequence
from typing import Any

import sqlalchemy as sa
from alembic import op

revision: str = "0098_network_edge_hypotheses"
down_revision: str | None = "0097_network_action_outcome_local_origin"
branch_labels: str | Sequence[str] | None = None
depends_on: str | Sequence[str] | None = None


def _timestamps() -> list[sa.Column[Any]]:
    return [
        sa.Column(name, sa.DateTime(timezone=True), nullable=False, server_default=sa.func.now())
        for name in ("created_at", "updated_at")
    ]


def upgrade() -> None:
    op.create_table(
        "network_edge_hypotheses",
        sa.Column("record_id", sa.String(128), primary_key=True),
        sa.Column("tenant_id", sa.String(64), sa.ForeignKey("tenants.id"), nullable=False),
        sa.Column("hypothesis_id", sa.String(64), nullable=False),
        sa.Column("asset_id", sa.String(128), nullable=False),
        sa.Column("network_epoch", sa.BigInteger(), nullable=False),
        sa.Column("sequence_id", sa.BigInteger(), nullable=False),
        sa.Column("interface", sa.String(64), nullable=False),
        sa.Column("source", sa.String(64), nullable=False, server_default="EDGE_LOCAL_LLM"),
        sa.Column("model_id", sa.String(128), nullable=False),
        sa.Column("summary", sa.Text(), nullable=False),
        sa.Column("evidence_refs_json", sa.JSON(), nullable=False),
        sa.Column("recommended_actions_json", sa.JSON(), nullable=False),
        sa.Column("status", sa.String(32), nullable=False, server_default="UNVERIFIED"),
        sa.Column("ttl_seconds", sa.Integer(), nullable=False, server_default="300"),
        sa.Column("hypothesis_created_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("received_at", sa.DateTime(timezone=True), nullable=False),
        *_timestamps(),
        sa.UniqueConstraint(
            "tenant_id",
            "asset_id",
            "hypothesis_id",
            name="uq_network_edge_hypothesis_id",
        ),
    )
    op.create_index(
        "ix_network_edge_hypotheses_snapshot",
        "network_edge_hypotheses",
        ["tenant_id", "asset_id", "network_epoch", "sequence_id"],
    )

    predicate = "tenant_id = current_setting('app.tenant_id', true)"
    policy_name = "network_edge_hypotheses_tenant_isolation"
    op.execute("ALTER TABLE network_edge_hypotheses ENABLE ROW LEVEL SECURITY")
    op.execute("ALTER TABLE network_edge_hypotheses FORCE ROW LEVEL SECURITY")
    op.execute(
        f"CREATE POLICY {policy_name} ON network_edge_hypotheses "
        f"USING ({predicate}) WITH CHECK ({predicate})"
    )


def downgrade() -> None:
    op.execute(
        "DROP POLICY IF EXISTS network_edge_hypotheses_tenant_isolation "
        "ON network_edge_hypotheses"
    )
    op.drop_index("ix_network_edge_hypotheses_snapshot", table_name="network_edge_hypotheses")
    op.drop_table("network_edge_hypotheses")

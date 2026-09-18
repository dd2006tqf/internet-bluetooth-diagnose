"""Add generation, nonce, and claim metadata to network_pending_actions.

Revision ID: 0088_network_action_transaction_metadata
Revises: 0087_network_assurance_telemetry
Create Date: 2026-09-18
"""

from __future__ import annotations

from collections.abc import Sequence

import sqlalchemy as sa
from alembic import op

revision: str = "0088_network_action_transaction_metadata"
down_revision: str | None = "0087_network_assurance_telemetry"
branch_labels: str | Sequence[str] | None = None
depends_on: str | Sequence[str] | None = None


def upgrade() -> None:
    # 1. 扩充列
    op.add_column(
        "network_pending_actions",
        sa.Column("generation", sa.BigInteger(), nullable=False, server_default="0"),
    )
    op.add_column(
        "network_pending_actions",
        sa.Column("nonce", sa.String(64), nullable=False, server_default=""),
    )
    op.add_column(
        "network_pending_actions",
        sa.Column("claimed_by_device_id", sa.String(128), nullable=True),
    )
    op.add_column(
        "network_pending_actions",
        sa.Column("claim_token", sa.String(128), nullable=True),
    )

    # 2. 防重放索引
    op.create_index(
        "ix_network_pending_actions_nonce",
        "network_pending_actions",
        ["tenant_id", "nonce"],
    )


def downgrade() -> None:
    op.drop_index("ix_network_pending_actions_nonce", table_name="network_pending_actions")
    op.drop_column("network_pending_actions", "claim_token")
    op.drop_column("network_pending_actions", "claimed_by_device_id")
    op.drop_column("network_pending_actions", "nonce")
    op.drop_column("network_pending_actions", "generation")

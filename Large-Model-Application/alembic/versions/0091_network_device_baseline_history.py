"""Add append-only network_device_baseline_history (L3 trajectory source).

Revision ID: 0091_network_device_baseline_history
Revises: 0090_weaknet_wireless_ingestion
Create Date: 2026-10-04

``network_device_baselines`` is an UPSERT current-state table: a device whose
baseline drifts -60 → -61 → -62 leaves only the last row, erasing the slow
drift that is precisely the most important long-term prediction signal (契约 C
纪律 6: L3 observes baseline_trajectory, not current-minus-current).

This table is append-only (historical rows are never updated), which is a
legal L1 authoritative-fact lifecycle evolution; L2~L5 never write it.

TimestampMixin (via TenantScopedMixin) declares BOTH created_at and updated_at —
the 0089 lesson: a migration omitting either makes every ORM SELECT fail on
PostgreSQL, and hermetic SQLite tests never catch it.
"""

from __future__ import annotations

from collections.abc import Sequence
from typing import Any

import sqlalchemy as sa
from alembic import op

revision: str = "0091_network_device_baseline_history"
down_revision: str | None = "0090_weaknet_wireless_ingestion"
branch_labels: str | Sequence[str] | None = None
depends_on: str | Sequence[str] | None = None

_TABLE = "network_device_baseline_history"


def _timestamps() -> list[sa.Column[Any]]:
    return [
        sa.Column(name, sa.DateTime(timezone=True), nullable=False, server_default=sa.func.now())
        for name in ("created_at", "updated_at")
    ]


def upgrade() -> None:
    op.create_table(
        _TABLE,
        sa.Column("history_id", sa.String(128), primary_key=True),
        sa.Column("tenant_id", sa.String(64), sa.ForeignKey("tenants.id"), nullable=False),
        sa.Column("baseline_id", sa.String(128), nullable=False),
        sa.Column("asset_id", sa.String(128), nullable=False),
        sa.Column("device_address", sa.String(32), nullable=False),
        sa.Column("observed_at_ms", sa.BigInteger(), nullable=False),
        sa.Column("baseline_rssi_dbm", sa.Integer(), nullable=True),
        sa.Column("baseline_sample_count", sa.Integer(), nullable=False, server_default="0"),
        sa.Column("state", sa.String(16), nullable=False, server_default="LEARNING"),
        *_timestamps(),
    )
    op.create_index(
        "ix_network_device_baseline_history_baseline_idx", _TABLE, ["baseline_id"]
    )
    op.create_index(
        "ix_network_device_baseline_history_series_idx",
        _TABLE,
        ["tenant_id", "baseline_id", "observed_at_ms"],
    )
    op.create_index(
        "ix_network_device_baseline_history_device_idx",
        _TABLE,
        ["tenant_id", "device_address", "observed_at_ms"],
    )

    # 与 0090 同款租户隔离：事实表一律进 RLS
    predicate = "tenant_id = current_setting('app.tenant_id', true)"
    policy_name = f"{_TABLE}_tenant_isolation"[:63]
    op.execute(f"ALTER TABLE {_TABLE} ENABLE ROW LEVEL SECURITY")
    op.execute(f"ALTER TABLE {_TABLE} FORCE ROW LEVEL SECURITY")
    op.execute(
        f"CREATE POLICY {policy_name} ON {_TABLE} "
        f"USING ({predicate}) WITH CHECK ({predicate})"
    )


def downgrade() -> None:
    policy_name = f"{_TABLE}_tenant_isolation"[:63]
    op.execute(f"DROP POLICY IF EXISTS {policy_name} ON {_TABLE}")
    op.drop_index("ix_network_device_baseline_history_device_idx", table_name=_TABLE)
    op.drop_index("ix_network_device_baseline_history_series_idx", table_name=_TABLE)
    op.drop_index("ix_network_device_baseline_history_baseline_idx", table_name=_TABLE)
    op.drop_table(_TABLE)

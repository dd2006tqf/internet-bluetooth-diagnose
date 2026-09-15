"""Add network assurance tables for WeakNet edge telemetry.

Introduces the device asset pool, the append-only assessment time series, and
the outbound action queue. All three are tenant-scoped and receive the same
row-level security treatment as every other tenant table, so a device
registered to one tenant cannot be read or overwritten from another.

The unique constraint on ``network_snapshots`` is the load-bearing part of
this migration: it is what makes edge replay safe when a device re-sends its
ring buffer after a lost acknowledgement.
"""

from __future__ import annotations

from collections.abc import Sequence
from typing import Any

import sqlalchemy as sa
from alembic import op

revision: str = "0087_network_assurance_telemetry"
down_revision: str | None = "0086_maintenance_review_exposure"
branch_labels: str | Sequence[str] | None = None
depends_on: str | Sequence[str] | None = None

_TENANT_TABLES = (
    "network_assets",
    "network_snapshots",
    "network_pending_actions",
)


def _timestamps() -> list[sa.Column[Any]]:
    return [
        sa.Column(name, sa.DateTime(timezone=True), nullable=False, server_default=sa.func.now())
        for name in ("created_at", "updated_at")
    ]


def _policy_name(table_name: str) -> str:
    return f"{table_name}_tenant_isolation"[:63]


def upgrade() -> None:
    op.create_table(
        "network_assets",
        sa.Column("asset_id", sa.String(128), primary_key=True),
        sa.Column("tenant_id", sa.String(64), sa.ForeignKey("tenants.id"), nullable=False),
        # 1. static identity
        sa.Column("display_name", sa.String(255), nullable=True),
        sa.Column("location", sa.String(255), nullable=True),
        sa.Column("hardware_arch", sa.String(64), nullable=True),
        sa.Column("os_kernel", sa.String(128), nullable=True),
        # 2. network identity / topology
        sa.Column("active_iface", sa.String(64), nullable=True),
        sa.Column("link_type", sa.String(32), nullable=False, server_default="UNKNOWN"),
        sa.Column("mac_address", sa.String(32), nullable=True),
        sa.Column("ip_address", sa.String(64), nullable=True),
        sa.Column("gateway_ip", sa.String(64), nullable=True),
        sa.Column("dns_servers_json", sa.JSON(), nullable=False),
        sa.Column("ap_ssid", sa.String(128), nullable=True),
        sa.Column("ap_bssid", sa.String(32), nullable=True),
        # 3. edge base configuration
        sa.Column("config_generation", sa.Integer(), nullable=True),
        sa.Column("network_epoch", sa.BigInteger(), nullable=True),
        sa.Column("rtt_interval_seconds", sa.Float(), nullable=True),
        sa.Column("rtt_probe_targets_json", sa.JSON(), nullable=False),
        # 4. current health
        sa.Column("overall_state", sa.String(16), nullable=False, server_default="UNKNOWN"),
        sa.Column("primary_issue", sa.String(512), nullable=True),
        sa.Column("display_score", sa.Integer(), nullable=False, server_default="50"),
        sa.Column("last_heartbeat_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column("connection_status", sa.String(16), nullable=False, server_default="OFFLINE"),
        # 5. latest evidence
        sa.Column("latest_sequence_id", sa.BigInteger(), nullable=True),
        sa.Column("latest_snapshot_json", sa.JSON(), nullable=True),
        sa.Column("signing_key_id", sa.String(128), nullable=True),
        sa.Column("version", sa.Integer(), nullable=False, server_default="1"),
        *_timestamps(),
    )
    op.create_index("ix_network_assets_tenant_id", "network_assets", ["tenant_id"])
    op.create_index(
        "ix_network_assets_status", "network_assets", ["tenant_id", "connection_status"]
    )
    op.create_index(
        "ix_network_assets_heartbeat", "network_assets", ["tenant_id", "last_heartbeat_at"]
    )

    op.create_table(
        "network_snapshots",
        sa.Column("snapshot_id", sa.String(128), primary_key=True),
        sa.Column("tenant_id", sa.String(64), sa.ForeignKey("tenants.id"), nullable=False),
        sa.Column("asset_id", sa.String(128), nullable=False),
        sa.Column("sequence_id", sa.BigInteger(), nullable=False),
        sa.Column("network_epoch", sa.BigInteger(), nullable=False),
        sa.Column("config_generation", sa.Integer(), nullable=False),
        sa.Column("captured_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("received_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("overall_state", sa.String(16), nullable=False),
        sa.Column("display_score", sa.Integer(), nullable=False),
        sa.Column("primary_issue", sa.String(512), nullable=True),
        sa.Column("experience_json", sa.JSON(), nullable=False),
        *_timestamps(),
        sa.UniqueConstraint(
            "tenant_id",
            "asset_id",
            "network_epoch",
            "sequence_id",
            name="uq_network_snapshot_stream_position",
        ),
    )
    op.create_index("ix_network_snapshots_tenant_id", "network_snapshots", ["tenant_id"])
    op.create_index("ix_network_snapshots_asset_id", "network_snapshots", ["asset_id"])
    op.create_index(
        "ix_network_snapshots_timeline",
        "network_snapshots",
        ["tenant_id", "asset_id", "captured_at"],
    )

    op.create_table(
        "network_pending_actions",
        sa.Column("action_id", sa.String(128), primary_key=True),
        sa.Column("tenant_id", sa.String(64), sa.ForeignKey("tenants.id"), nullable=False),
        sa.Column("asset_id", sa.String(128), nullable=False),
        sa.Column("config_key", sa.String(128), nullable=False),
        sa.Column("config_value", sa.String(512), nullable=False),
        sa.Column("status", sa.String(16), nullable=False, server_default="QUEUED"),
        sa.Column("issued_by", sa.String(128), nullable=False),
        sa.Column("approved_by", sa.String(128), nullable=True),
        sa.Column("issued_at", sa.DateTime(timezone=True), nullable=False),
        sa.Column("delivered_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column("completed_at", sa.DateTime(timezone=True), nullable=True),
        sa.Column("result_detail", sa.String(1024), nullable=True),
        sa.Column("version", sa.Integer(), nullable=False, server_default="1"),
        *_timestamps(),
    )
    op.create_index(
        "ix_network_pending_actions_tenant_id", "network_pending_actions", ["tenant_id"]
    )
    op.create_index(
        "ix_network_pending_actions_asset_id", "network_pending_actions", ["asset_id"]
    )
    op.create_index(
        "ix_network_pending_actions_queue",
        "network_pending_actions",
        ["tenant_id", "asset_id", "status"],
    )

    predicate = "tenant_id = current_setting('app.tenant_id', true)"
    for table_name in _TENANT_TABLES:
        policy_name = _policy_name(table_name)
        op.execute(f"ALTER TABLE {table_name} ENABLE ROW LEVEL SECURITY")
        op.execute(f"ALTER TABLE {table_name} FORCE ROW LEVEL SECURITY")
        op.execute(
            f"CREATE POLICY {policy_name} ON {table_name} "
            f"USING ({predicate}) WITH CHECK ({predicate})"
        )


def downgrade() -> None:
    for table_name in reversed(_TENANT_TABLES):
        policy_name = _policy_name(table_name)
        op.execute(f"DROP POLICY IF EXISTS {policy_name} ON {table_name}")
        op.execute(f"ALTER TABLE {table_name} NO FORCE ROW LEVEL SECURITY")
        op.execute(f"ALTER TABLE {table_name} DISABLE ROW LEVEL SECURITY")

    op.drop_index("ix_network_pending_actions_queue", table_name="network_pending_actions")
    op.drop_index("ix_network_pending_actions_asset_id", table_name="network_pending_actions")
    op.drop_index("ix_network_pending_actions_tenant_id", table_name="network_pending_actions")
    op.drop_table("network_pending_actions")

    op.drop_index("ix_network_snapshots_timeline", table_name="network_snapshots")
    op.drop_index("ix_network_snapshots_asset_id", table_name="network_snapshots")
    op.drop_index("ix_network_snapshots_tenant_id", table_name="network_snapshots")
    op.drop_table("network_snapshots")

    op.drop_index("ix_network_assets_heartbeat", table_name="network_assets")
    op.drop_index("ix_network_assets_status", table_name="network_assets")
    op.drop_index("ix_network_assets_tenant_id", table_name="network_assets")
    op.drop_table("network_assets")

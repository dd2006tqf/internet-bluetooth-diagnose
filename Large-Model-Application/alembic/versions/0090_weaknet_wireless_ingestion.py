"""Add the four Phase 4a wireless-fact tables (edge -> cloud uplink).

Revision ID: 0090_weaknet_wireless_ingestion
Revises: 0089_wireless_incident_diagnosis
Create Date: 2026-10-01

The four tables are the *fact* side of the wireless diagnosis pipeline; the
Phase 4b ``site_incident_diagnoses`` table holds the *interpretation*. Facts
are written only by the signed edge uplink (``POST /network/edge/wireless-
events``) and are never rewritten by the diagnosis path.

This migration also applies the 0087-style row-level-security policy to
``site_incident_diagnoses``, which 0089 created without one.
"""

from __future__ import annotations

from collections.abc import Sequence
from typing import Any

import sqlalchemy as sa
from alembic import op

revision: str = "0090_weaknet_wireless_ingestion"
down_revision: str | None = "0089_wireless_incident_diagnosis"
branch_labels: str | Sequence[str] | None = None
depends_on: str | Sequence[str] | None = None

_TENANT_TABLES = (
    "network_wireless_events",
    "network_site_incidents",
    "network_device_baselines",
    "network_env_windows",
    # 0089 建表时未附 RLS 策略，与本批事实表同属无线诊断子系统，一并补齐
    "site_incident_diagnoses",
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
        "network_wireless_events",
        sa.Column("event_id", sa.String(128), primary_key=True),
        sa.Column("tenant_id", sa.String(64), sa.ForeignKey("tenants.id"), nullable=False),
        sa.Column("asset_id", sa.String(128), nullable=False),
        sa.Column("site_id", sa.String(128), nullable=False, server_default=""),
        sa.Column("gateway_id", sa.String(128), nullable=False, server_default=""),
        sa.Column("protocol", sa.String(32), nullable=False, server_default="BLUETOOTH"),
        sa.Column("device_address", sa.String(32), nullable=False),
        sa.Column("address_type", sa.String(16), nullable=False, server_default="UNKNOWN"),
        sa.Column("hci_index", sa.Integer(), nullable=False, server_default="0"),
        sa.Column("event_type", sa.String(32), nullable=False),
        sa.Column("ts_ms", sa.BigInteger(), nullable=False),
        sa.Column("rssi_at_event_dbm", sa.Integer(), nullable=True),
        sa.Column("raw_reason_code", sa.Integer(), nullable=False, server_default="0"),
        sa.Column("reason", sa.String(64), nullable=False, server_default="UNKNOWN"),
        sa.Column("source", sa.String(32), nullable=False, server_default="UNKNOWN"),
        sa.Column("source_detail", sa.String(64), nullable=False, server_default=""),
        sa.Column("details_json", sa.Text(), nullable=False, server_default=""),
        *_timestamps(),
    )
    op.create_index("ix_network_wireless_events_tenant_id", "network_wireless_events", ["tenant_id"])
    op.create_index(
        "ix_network_wireless_events_ts", "network_wireless_events", ["tenant_id", "ts_ms"]
    )
    op.create_index(
        "ix_network_wireless_events_dev",
        "network_wireless_events",
        ["tenant_id", "device_address"],
    )

    op.create_table(
        "network_site_incidents",
        sa.Column("incident_id", sa.String(128), primary_key=True),
        sa.Column("tenant_id", sa.String(64), sa.ForeignKey("tenants.id"), nullable=False),
        sa.Column("asset_id", sa.String(128), nullable=False),
        sa.Column("site_id", sa.String(128), nullable=False, server_default=""),
        sa.Column("gateway_id", sa.String(128), nullable=False, server_default=""),
        sa.Column("started_at_ms", sa.BigInteger(), nullable=False),
        sa.Column("last_event_ms", sa.BigInteger(), nullable=False),
        sa.Column("resolved_at_ms", sa.BigInteger(), nullable=True),
        sa.Column("affected_devices", sa.Integer(), nullable=False, server_default="0"),
        sa.Column("state", sa.String(16), nullable=False, server_default="OPEN"),
        sa.Column("suspected_cause", sa.Text(), nullable=True),
        sa.Column("evidence_event_ids_json", sa.JSON(), nullable=False, server_default="[]"),
        *_timestamps(),
    )
    op.create_index("ix_network_site_incidents_tenant_id", "network_site_incidents", ["tenant_id"])
    op.create_index(
        "ix_network_site_incidents_window",
        "network_site_incidents",
        ["tenant_id", "started_at_ms"],
    )
    op.create_index(
        "ix_network_site_incidents_state", "network_site_incidents", ["tenant_id", "state"]
    )

    op.create_table(
        "network_device_baselines",
        sa.Column("baseline_id", sa.String(128), primary_key=True),
        sa.Column("tenant_id", sa.String(64), sa.ForeignKey("tenants.id"), nullable=False),
        sa.Column("asset_id", sa.String(128), nullable=False),
        sa.Column("site_id", sa.String(128), nullable=False, server_default=""),
        sa.Column("gateway_id", sa.String(128), nullable=False, server_default=""),
        sa.Column("hci_index", sa.Integer(), nullable=False, server_default="0"),
        sa.Column("protocol", sa.String(32), nullable=False, server_default="BLUETOOTH"),
        sa.Column("address_type", sa.String(16), nullable=False, server_default="UNKNOWN"),
        sa.Column("device_address", sa.String(32), nullable=False),
        sa.Column("baseline_rssi_dbm", sa.Integer(), nullable=True),
        sa.Column("min_seen_rssi_dbm", sa.Integer(), nullable=True),
        sa.Column("max_seen_rssi_dbm", sa.Integer(), nullable=True),
        sa.Column("baseline_sample_count", sa.Integer(), nullable=False, server_default="0"),
        sa.Column("state", sa.String(16), nullable=False, server_default="LEARNING"),
        sa.Column("first_seen_ms", sa.BigInteger(), nullable=True),
        sa.Column("last_seen_ms", sa.BigInteger(), nullable=True),
        *_timestamps(),
    )
    op.create_index(
        "ix_network_device_baselines_tenant_id", "network_device_baselines", ["tenant_id"]
    )
    op.create_index(
        "ix_network_device_baselines_dev",
        "network_device_baselines",
        ["tenant_id", "asset_id", "device_address"],
    )

    op.create_table(
        "network_env_windows",
        sa.Column("window_id", sa.String(128), primary_key=True),
        sa.Column("tenant_id", sa.String(64), sa.ForeignKey("tenants.id"), nullable=False),
        sa.Column("asset_id", sa.String(128), nullable=False),
        sa.Column("from_ms", sa.BigInteger(), nullable=False),
        sa.Column("to_ms", sa.BigInteger(), nullable=False),
        sa.Column("available", sa.Boolean(), nullable=False, server_default="false"),
        sa.Column("link_type", sa.String(32), nullable=False, server_default="UNKNOWN"),
        sa.Column("wifi_anomaly", sa.Boolean(), nullable=False, server_default="false"),
        sa.Column("coexistence_warning", sa.Boolean(), nullable=False, server_default="false"),
        sa.Column("snapshots_json", sa.JSON(), nullable=False, server_default="[]"),
        *_timestamps(),
    )
    op.create_index("ix_network_env_windows_tenant_id", "network_env_windows", ["tenant_id"])
    op.create_index("ix_network_env_windows_range", "network_env_windows", ["tenant_id", "from_ms"])

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

    op.drop_index("ix_network_env_windows_range", table_name="network_env_windows")
    op.drop_index("ix_network_env_windows_tenant_id", table_name="network_env_windows")
    op.drop_table("network_env_windows")

    op.drop_index("ix_network_device_baselines_dev", table_name="network_device_baselines")
    op.drop_index("ix_network_device_baselines_tenant_id", table_name="network_device_baselines")
    op.drop_table("network_device_baselines")

    op.drop_index("ix_network_site_incidents_state", table_name="network_site_incidents")
    op.drop_index("ix_network_site_incidents_window", table_name="network_site_incidents")
    op.drop_index("ix_network_site_incidents_tenant_id", table_name="network_site_incidents")
    op.drop_table("network_site_incidents")

    op.drop_index("ix_network_wireless_events_dev", table_name="network_wireless_events")
    op.drop_index("ix_network_wireless_events_ts", table_name="network_wireless_events")
    op.drop_index("ix_network_wireless_events_tenant_id", table_name="network_wireless_events")
    op.drop_table("network_wireless_events")

"""Add site_incident_diagnoses table for Phase 4b.

Revision ID: 0089_wireless_incident_diagnosis
Revises: 0088_network_action_transaction_metadata
Create Date: 2026-10-01
"""

from __future__ import annotations

from collections.abc import Sequence

import sqlalchemy as sa
from alembic import op

revision: str = "0089_wireless_incident_diagnosis"
down_revision: str | None = "0088_network_action_transaction_metadata"
branch_labels: str | Sequence[str] | None = None
depends_on: str | Sequence[str] | None = None


def upgrade() -> None:
    op.create_table(
        "site_incident_diagnoses",
        sa.Column("diagnosis_id", sa.String(128), primary_key=True),
        sa.Column("tenant_id", sa.String(64), sa.ForeignKey("tenants.id"), nullable=False),
        sa.Column("incident_id", sa.String(128), nullable=False),
        sa.Column("diagnosis_version", sa.String(32), nullable=False),
        sa.Column("rules_version", sa.String(64), nullable=False),
        sa.Column("prompt_version", sa.String(64), nullable=False),
        sa.Column("guardrail_version", sa.String(64), nullable=False),
        sa.Column("evidence_fingerprint", sa.String(64), nullable=False),
        sa.Column("observed_pattern", sa.String(64), nullable=False),
        sa.Column("hypothesis", sa.String(64), nullable=False),
        sa.Column("confidence", sa.String(16), nullable=False),
        sa.Column("deterministic_reasons", sa.JSON(), nullable=False),
        sa.Column("evidence_ids", sa.JSON(), nullable=False),
        sa.Column("device_findings", sa.JSON(), nullable=False, server_default="[]"),
        sa.Column("llm_model_name", sa.String(64), nullable=True),
        sa.Column("llm_used", sa.Boolean(), nullable=False, server_default="false"),
        sa.Column("diagnosis_report", sa.Text(), nullable=False),
        sa.Column("structured_findings", sa.JSON(), nullable=False, server_default="[]"),
        sa.Column("evidence_citations", sa.JSON(), nullable=False, server_default="[]"),
        sa.Column("recommendations", sa.JSON(), nullable=False, server_default="[]"),
        sa.Column("guardrail_status", sa.String(32), nullable=False, server_default="PASSED"),
        sa.Column("guardrail_findings", sa.JSON(), nullable=False, server_default="[]"),
        # ``TimestampMixin`` (via TenantScopedMixin) declares BOTH created_at and
        # updated_at; a table carrying only one of them makes every ORM SELECT
        # fail on PostgreSQL with UndefinedColumn. The hermetic SQLite tests
        # never catch this because they build the schema from the models.
        sa.Column(
            "created_at", sa.DateTime(timezone=True), nullable=False, server_default=sa.func.now()
        ),
        sa.Column(
            "updated_at", sa.DateTime(timezone=True), nullable=False, server_default=sa.func.now()
        ),
        sa.UniqueConstraint(
            "tenant_id",
            "incident_id",
            "evidence_fingerprint",
            "rules_version",
            "prompt_version",
            "guardrail_version",
            name="uq_site_incident_diagnosis_versioned",
        ),
    )
    op.create_index(
        "ix_site_incident_diagnoses_lookup",
        "site_incident_diagnoses",
        ["tenant_id", "incident_id", "created_at"],
    )


def downgrade() -> None:
    op.drop_index("ix_site_incident_diagnoses_lookup", table_name="site_incident_diagnoses")
    op.drop_table("site_incident_diagnoses")

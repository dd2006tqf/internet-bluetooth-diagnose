"""The platform-internal subject that acts for an edge gateway.

Why a single provisioning point
-------------------------------
Two automation paths already act on behalf of the fleet — the incident-draft
bridge (S7) and the knowledge-draft bridge (S6/S8). Both need a
``SubjectRecord`` because the platform's own services call ``lock_subject``,
which refuses an unknown or non-active subject (``review_isolation.py:89``).

Keeping one upsert here means the subject's identity, status spelling and
roles are decided once. It also removes an assumption the first bridge made:
that a member row already exists under this id. It did not — nothing in the
repository provisioned it — so the bridge could never have succeeded against a
real database.

Status spelling matters: ``lock_subject`` compares against the lower-case
``"active"`` while the tenant-administration flow writes ``"ACTIVE"``. Seed
rows (the ones the platform's own demos use) are lower-case, and this subject
follows the seed accordingly. The inconsistency itself is a known legacy item,
not something this module paper over.
"""

from __future__ import annotations

from datetime import UTC, datetime

from sqlalchemy import select
from sqlalchemy.orm import Session

from industrial_ops_agent.persistence.models import (
    SubjectRecord,
    SubjectRoleRecord,
)

#: One subject per tenant, shared by every edge automation path. It is not a
#: device and not a person; it is the platform acting on telemetry it received.
EDGE_AUTOMATION_SUBJECT_ID = "edge-automation"
_EDGE_AUTOMATION_ISSUER = "platform-internal"


def ensure_edge_automation_subject(session: Session, *, tenant_id: str) -> SubjectRecord:
    """Idempotently materialise the edge-automation subject for one tenant.

    Roles are deliberately *not* written here: authorization is derived from the
    identity the caller builds, not from a stored role row, and writing roles
    would imply a grant this module has no mandate to make.
    """

    existing = session.scalar(
        select(SubjectRecord).where(
            SubjectRecord.tenant_id == tenant_id,
            SubjectRecord.subject_id == EDGE_AUTOMATION_SUBJECT_ID,
        )
    )
    if existing is not None:
        return existing

    now = datetime.now(UTC)
    record = SubjectRecord(
        subject_id=EDGE_AUTOMATION_SUBJECT_ID,
        tenant_id=tenant_id,
        oidc_issuer=_EDGE_AUTOMATION_ISSUER,
        oidc_subject=EDGE_AUTOMATION_SUBJECT_ID,
        display_name="Edge automation",
        status="active",
        version=1,
        created_at=now,
        updated_at=now,
    )
    session.add(record)
    session.flush()
    return record


def has_edge_automation_role(session: Session, *, tenant_id: str, role: str) -> bool:
    """Read-only helper for tests and diagnostics."""

    return (
        session.scalar(
            select(SubjectRoleRecord).where(
                SubjectRoleRecord.tenant_id == tenant_id,
                SubjectRoleRecord.subject_id == EDGE_AUTOMATION_SUBJECT_ID,
                SubjectRoleRecord.role == role,
            )
        )
        is not None
    )

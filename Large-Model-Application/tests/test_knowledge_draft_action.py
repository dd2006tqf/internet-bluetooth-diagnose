"""P6a: the knowledge-draft action, its role grants, and the create/manage split.

The platform already separated "create an incident draft" from "control an
incident" the same way: a draft is a proposal in a queue, while the governance
chain (review, build release, promote) stays with the roles that own it. This
module extends that pattern to knowledge, because the knowledge path has the
same separation-of-duties rule: whoever creates a version cannot review it
(``knowledge_review_separation_required``).

What these tests pin:

  1. the narrow action is granted to the human roles that can already publish;
  2. a role holding only the narrow action is refused the governance chain;
  3. ``create_document`` accepts the narrow action while ``create_version``
     (editing an existing, possibly reviewed document) still demands the full one;
  4. the edge-automation subject is provisioned idempotently, with the status
     spelling ``lock_subject`` actually accepts.
"""

from __future__ import annotations

from datetime import UTC, datetime, timedelta

import pytest
from sqlalchemy import create_engine, select
from sqlalchemy.orm import Session
from sqlalchemy.pool import StaticPool

from industrial_ops_agent.auth.errors import AuthorizationDenied
from industrial_ops_agent.auth.identity import IdentityContext, Role
from industrial_ops_agent.auth.policy import Action, Authorizer, ResourceContext
from industrial_ops_agent.knowledge.ingestion import KnowledgeIngestionService
from industrial_ops_agent.network_assurance.automation_subject import (
    EDGE_AUTOMATION_SUBJECT_ID,
    ensure_edge_automation_subject,
)
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import (
    Base,
    SubjectRecord,
    TenantRecord,
)
from industrial_ops_agent.security_audit import (
    InMemorySecurityAuditSink,
    SecurityAuditor,
)

TENANT = "tenant-alpha"


def _authorizer() -> Authorizer:
    return Authorizer(
        SecurityAuditor(InMemorySecurityAuditSink(), hash_key=b"p6a-test-audit-key")
    )


def _identity(roles: frozenset[Role], subject: str = "subject-1") -> IdentityContext:
    now = datetime.now(UTC)
    return IdentityContext(
        subject_id=subject,
        oidc_subject=subject,
        tenant_id=TENANT,
        roles=roles,
        asset_ids=frozenset(),
        site_ids=frozenset(),
        issued_at=now,
        expires_at=now + timedelta(minutes=30),
    )


def test_draft_action_is_granted_to_human_creators():
    for role in (Role.FIELD_ENGINEER, Role.DOMAIN_EXPERT, Role.TENANT_ADMIN):
        identity = _identity(frozenset({role}))
        decision = _authorizer().decide(
            identity,
            Action.CREATE_KNOWLEDGE_DRAFT,
            ResourceContext(TENANT, "new-document"),
        )
        assert decision.allowed, f"{role} should be able to create knowledge drafts"


def test_customer_contact_cannot_create_knowledge_drafts():
    identity = _identity(frozenset({Role.CUSTOMER_CONTACT}))
    decision = _authorizer().decide(
        identity,
        Action.CREATE_KNOWLEDGE_DRAFT,
        ResourceContext(TENANT, "new-document"),
    )
    assert not decision.allowed


def test_draft_action_does_not_confer_publish_rights():
    """持有窄动作的角色不得因此获得治理链权限。"""

    identity = _identity(frozenset({Role.FIELD_ENGINEER}))
    authorizer = _authorizer()
    for action in (Action.PUBLISH_KNOWLEDGE, Action.EVALUATE_KNOWLEDGE_INDEX):
        decision = authorizer.decide(identity, action, ResourceContext(TENANT, "release-1"))
        assert not decision.allowed, f"draft-only role must not hold {action}"


@pytest.fixture
def test_db() -> Database:
    engine = create_engine(
        "sqlite:///:memory:",
        connect_args={"check_same_thread": False},
        poolclass=StaticPool,
    )
    Base.metadata.create_all(engine)
    db = Database.from_engine(engine)
    with Session(db.engine) as session:  # type: ignore[attr-defined]
        session.add(
            TenantRecord(
                id=TENANT,
                status="active",
                display_name="P6a tenant",
                version=1,
                created_at=datetime.now(UTC),
                updated_at=datetime.now(UTC),
            )
        )
        session.commit()
    return db


def _create_document(db: Database, identity: IdentityContext, key: str):
    return KnowledgeIngestionService(db, _authorizer()).create_document(
        identity,
        title="区域无线异常案例",
        source_uri="https://ops.example.com/api/v1/network/assurance/incidents/i-1/diagnosis",
        content="网关上报区域事故，多台设备亚秒级同步断开，同期 Wi-Fi 2.4GHz 异常。",
        classification="internal",
        acl_subject_ids=(),
        acl_roles=("field_engineer", "domain_expert"),
        device_families=(),
        device_models=(),
        valid_from=datetime.now(UTC) - timedelta(minutes=1),
        valid_to=None,
        idempotency_key=key,
        request_id="req-1",
    )


def test_create_document_accepts_the_narrow_action(test_db: Database):
    identity = _identity(frozenset({Role.FIELD_ENGINEER}), subject="edge-automation")
    version = _create_document(test_db, identity, "key-1")
    assert version.status == "DRAFT"
    assert version.created_by_subject_id == "edge-automation"


def test_create_document_rejects_roles_without_the_action(test_db: Database):
    identity = _identity(frozenset({Role.CUSTOMER_CONTACT}), subject="customer-1")
    with pytest.raises(AuthorizationDenied):
        _create_document(test_db, identity, "key-2")


def test_create_version_still_requires_publish_rights(test_db: Database):
    """给既有文档加版本属治理动作：窄动作不够。"""

    creator = _identity(frozenset({Role.FIELD_ENGINEER}), subject="edge-automation")
    version = _create_document(test_db, creator, "key-3")

    editor = _identity(frozenset({Role.FIELD_ENGINEER}), subject="edge-automation")
    with pytest.raises(AuthorizationDenied):
        KnowledgeIngestionService(test_db, _authorizer()).create_version(
            editor,
            version.document_id,
            content="试图改写既有文档内容。",
            acl_subject_ids=(),
            acl_roles=("field_engineer",),
            device_families=(),
            device_models=(),
            valid_from=datetime.now(UTC) - timedelta(minutes=1),
            valid_to=None,
            idempotency_key="key-4",
            request_id="req-4",
        )


def test_edge_automation_subject_is_provisioned_idempotently(test_db: Database):
    with Session(test_db.engine) as session:  # type: ignore[attr-defined]
        first = ensure_edge_automation_subject(session, tenant_id=TENANT)
        session.commit()
        second = ensure_edge_automation_subject(session, tenant_id=TENANT)
        session.commit()

        rows = list(
            session.scalars(
                select(SubjectRecord).where(
                    SubjectRecord.tenant_id == TENANT,
                    SubjectRecord.subject_id == EDGE_AUTOMATION_SUBJECT_ID,
                )
            )
        )

    assert first.subject_id == second.subject_id == EDGE_AUTOMATION_SUBJECT_ID
    assert len(rows) == 1
    # lock_subject 只认小写 "active"；写成 "ACTIVE" 会让草稿路径永远 subject_unavailable
    assert rows[0].status == "active"

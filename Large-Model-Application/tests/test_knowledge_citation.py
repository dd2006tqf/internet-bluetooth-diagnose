"""P6c: knowledge enters a diagnosis as *background*, never as evidence.

The boundary these tests pin:

    retrieved case  ->  payload["knowledge_context"]   (background only)
    incident events ->  payload["evidence_catalog"]    (the only valid evidence)

Consequences worth asserting explicitly:

  1. a reference appears only when the tenant has an active published release
     and the caller's roles pass the document's ACL;
  2. the retrieval step is optional — no identity, no release, backend error,
     all degrade to "no knowledge" while the model call still happens;
  3. the W6 guardrail rejects any attempt to cite a knowledge document id as
     evidence for this incident, because those ids are never in
     ``valid_evidence_ids``.
"""

from __future__ import annotations

import json
from datetime import UTC, datetime, timedelta
from hashlib import sha256
from typing import Any

import pytest
from sqlalchemy import create_engine
from sqlalchemy.orm import Session
from sqlalchemy.pool import StaticPool

from industrial_ops_agent.auth.identity import IdentityContext, Role
from industrial_ops_agent.guardrails.network_causal import (
    NetworkCausalGuardrail,
    WirelessCausalContext,
)
from industrial_ops_agent.knowledge.embedding import deterministic_embedding
from industrial_ops_agent.network_assurance.wireless_contracts import (
    CanonicalDiagnosis,
    ConfidenceLevel,
    DiagnosisHypothesis,
    EnvironmentWindowView,
    IncidentEvidenceBundle,
    IncidentView,
    ObservedPattern,
    WirelessEventView,
)
from industrial_ops_agent.network_assurance.wireless_diagnosis import (
    WirelessDiagnosisService,
)
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import (
    Base,
    CitationAnchorRecord,
    IndexReleaseRecord,
    KnowledgeChunkRecord,
    KnowledgeDocumentRecord,
    KnowledgeDocumentVersionRecord,
    TenantRecord,
)
from industrial_ops_agent.persistence.tenant import TenantContext

TENANT = "tenant-alpha"
INCIDENT = "site-1_1700000001000"
RELEASE = "release-weaknet-cases-v1"
DOC = "knowledge-doc-case-1"
VERSION = "kv-case-1"
CHUNK = "kc-case-1"

#: 与 build_case_content 产出一致：枚举值原文出现，tokenize 双侧同源。
CASE_CONTENT = (
    "# 区域无线异常案例 site-1_1790745159954\n"
    "物理观测模式: SUB_SECOND_SIMULTANEOUS_DISCONNECT\n"
    "假说: COEXISTENCE_RF_INTERFERENCE（置信度 HIGH）\n"
    "依据: 多设备亚秒级同步断开，同期 Wi-Fi 2.4GHz 空口异常与频段竞争。"
)


def _checksum(value: str) -> str:
    return sha256(value.encode()).hexdigest()


def _canonical() -> CanonicalDiagnosis:
    return CanonicalDiagnosis(
        observed_pattern=ObservedPattern.SUB_SECOND_SIMULTANEOUS_DISCONNECT,
        hypothesis=DiagnosisHypothesis.COEXISTENCE_RF_INTERFERENCE,
        confidence=ConfidenceLevel.HIGH,
        evidence_ids=["e1", "e2"],
        deterministic_reasons=["多设备亚秒级同步断开。"],
    )


def _bundle() -> IncidentEvidenceBundle:
    event = WirelessEventView(
        event_id="e1",
        ts_ms=1_700_000_001_000,
        site_id="site-1",
        gateway_id="gw-1",
        protocol="BLUETOOTH",
        device_address="AA:01",
        address_type="LE_RANDOM",
        hci_index=0,
        event_type="LINK_DISCONNECTED",
        reason="CONNECTION_TIMEOUT",
        source="KERNEL_MGMT",
    )
    return IncidentEvidenceBundle(
        incident=IncidentView(
            incident_id=INCIDENT,
            site_id="site-1",
            gateway_id="gw-1",
            started_at_ms=1_700_000_001_000,
            last_event_ms=1_700_000_001_500,
            affected_devices=2,
            state="OPEN",
        ),
        qualifying_events=[event],
        window_events=[event],
        baselines={},
        environment=EnvironmentWindowView(),
    )


def _identity(roles: frozenset[Role]) -> IdentityContext:
    now = datetime.now(UTC)
    return IdentityContext(
        subject_id="operator-1",
        oidc_subject="operator-1",
        tenant_id=TENANT,
        roles=roles,
        asset_ids=frozenset(),
        site_ids=frozenset(),
        issued_at=now,
        expires_at=now + timedelta(minutes=30),
    )


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
                display_name="P6c tenant",
                version=1,
                created_at=datetime.now(UTC),
                updated_at=datetime.now(UTC),
            )
        )
        session.commit()
    return db


def _seed_case_document(
    db: Database, *, acl_roles: tuple[str, ...] = ("field_engineer", "domain_expert")
) -> None:
    """Seed an active published release holding one matching prior case."""

    now = datetime.now(UTC)
    checksum = _checksum(CASE_CONTENT)
    with Session(db.engine) as session:  # type: ignore[attr-defined]
        session.add(
            IndexReleaseRecord(
                tenant_id=TENANT,
                release_id=RELEASE,
                name="weaknet-wireless-cases",
                version=1,
                status="PUBLISHED",
                is_active=True,
                content_checksum=_checksum("release-v1"),
                published_by="reviewer-1",
                published_at=now,
            )
        )
        session.add(
            KnowledgeDocumentRecord(
                tenant_id=TENANT,
                document_id=DOC,
                title="区域无线异常案例 site-1_1790745159954",
                source_uri="https://ops.example.com/api/v1/network/assurance/incidents/x/diagnosis",
                classification="internal",
                source_checksum=checksum,
                current_version=1,
            )
        )
        session.add(
            KnowledgeDocumentVersionRecord(
                tenant_id=TENANT,
                document_version_id=VERSION,
                document_id=DOC,
                version=1,
                parser_version="markdown-parser-v1",
                chunker_version="semantic-chunker-v1",
                acl_subject_ids=[],
                acl_roles=list(acl_roles),
                device_families=[],
                device_models=[],
                valid_from=now - timedelta(days=1),
                valid_to=None,
                source_checksum=checksum,
                content_checksum=checksum,
                status="PUBLISHED",
            )
        )
        session.add(
            KnowledgeChunkRecord(
                tenant_id=TENANT,
                chunk_id=CHUNK,
                document_version_id=VERSION,
                release_id=RELEASE,
                ordinal=1,
                content=CASE_CONTENT,
                content_checksum=checksum,
                embedding=deterministic_embedding(CASE_CONTENT),
                token_count=len(CASE_CONTENT.split()),
            )
        )
        session.add(
            CitationAnchorRecord(
                tenant_id=TENANT,
                citation_id="cite-case-1",
                chunk_id=CHUNK,
                document_version_id=VERSION,
                anchor_kind="page",
                page_number=None,
                bounding_box=None,
                start_offset=0,
                end_offset=len(CASE_CONTENT),
                excerpt=CASE_CONTENT,
                excerpt_checksum=checksum,
            )
        )
        session.commit()


def _diagnose(db: Database, identity: IdentityContext | None, monkeypatch: Any) -> dict[str, Any]:
    """Run POST diagnosis and return the payload the model actually received."""

    captured: dict[str, Any] = {}

    def _fake_complete(system_prompt: str, user_content: str, **kwargs: Any) -> dict[str, Any]:
        captured.update(json.loads(user_content))
        return {
            "diagnosis_report": "现场多设备亚秒级同步断开，同期 Wi-Fi 干扰。",
            "structured_findings": [{"text": "2台设备超时断开", "evidence_ids": ["e1"]}],
            "evidence_citations": [],
            "recommendations": ["检查 2.4GHz 信道"],
        }

    monkeypatch.setattr(
        "industrial_ops_agent.network_assurance.wireless_diagnosis.complete_json",
        _fake_complete,
    )

    service = WirelessDiagnosisService(db)
    context = TenantContext(tenant_id=TENANT, subject_id="operator-1")
    kwargs: dict[str, Any] = {}
    if identity is not None:
        kwargs["identity"] = identity
    service.diagnose_incident(
        context, INCIDENT, bundle_loader=lambda _: _bundle(), **kwargs
    )
    return captured


FIELD_ENGINEER = _identity(frozenset({Role.FIELD_ENGINEER}))
CUSTOMER_CONTACT = _identity(frozenset({Role.CUSTOMER_CONTACT}))


def test_reference_injected_when_active_release_and_acl_match(
    test_db: Database, monkeypatch: Any
):
    _seed_case_document(test_db)
    payload = _diagnose(test_db, FIELD_ENGINEER, monkeypatch)

    references = payload.get("knowledge_context")
    assert references, "应检索到既往案例"
    assert references[0]["title"].startswith("区域无线异常案例")
    assert references[0]["document_id"] == DOC
    # 注入指令必须明确"不得当证据"
    assert "knowledge_context" in payload["instruction"]
    assert "evidence_ids" in payload["instruction"]


def test_retrieval_never_pollutes_the_evidence_catalog(
    test_db: Database, monkeypatch: Any
):
    """本案证据集只含本案事件——知识文档 id 绝不进入。"""

    _seed_case_document(test_db)
    payload = _diagnose(test_db, FIELD_ENGINEER, monkeypatch)

    evidence_ids = {item["id"] for item in payload["evidence_catalog"]}
    assert evidence_ids == {"e1", "WIFI_ANOMALY"} or evidence_ids == {"e1"}
    assert DOC not in evidence_ids
    assert CHUNK not in evidence_ids
    assert RELEASE not in evidence_ids


def test_no_active_release_means_no_reference(
    test_db: Database, monkeypatch: Any
):
    payload = _diagnose(test_db, FIELD_ENGINEER, monkeypatch)  # 未播种 release
    assert "knowledge_context" not in payload


def test_no_identity_means_no_retrieval(test_db: Database, monkeypatch: Any):
    """identity 缺省（GET/既有调用路径）→ 完全不检索，零副作用。"""

    _seed_case_document(test_db)
    payload = _diagnose(test_db, None, monkeypatch)
    assert "knowledge_context" not in payload


def test_role_acl_excludes_caller_without_the_role(
    test_db: Database, monkeypatch: Any
):
    """文档 ACL 只放行 field_engineer/domain_expert——客户角色拿不到内部案例。"""

    _seed_case_document(test_db, acl_roles=("field_engineer", "domain_expert"))
    payload = _diagnose(test_db, CUSTOMER_CONTACT, monkeypatch)
    assert "knowledge_context" not in payload


def test_retrieval_backend_failure_is_silent(
    test_db: Database, monkeypatch: Any
):
    """检索后端炸了 → 没有知识，但模型调用照常（绝不因知识库阻断诊断）。"""

    _seed_case_document(test_db)

    def _boom(*args: Any, **kwargs: Any) -> Any:
        raise RuntimeError("retrieval backend down")

    monkeypatch.setattr(
        "industrial_ops_agent.network_assurance.knowledge_citation.HybridRetriever.search",
        _boom,
    )

    payload = _diagnose(test_db, FIELD_ENGINEER, monkeypatch)
    assert "knowledge_context" not in payload
    assert payload.get("canonical_diagnosis")  # 模型确实被调用了（payload 已捕获）


def test_knowledge_document_id_cannot_be_cited_as_incident_evidence():
    """W6：知识文档 id 不在 valid_evidence_ids → 引用即 BLOCKED。"""

    guardrail = NetworkCausalGuardrail()
    canonical = _canonical()
    context = WirelessCausalContext(
        incident_id=INCIDENT,
        canonical=canonical,
        affected_devices=2,
        wifi_anomaly=True,
        valid_evidence_ids=frozenset({"e1", "e2", "WIFI_ANOMALY"}),
    )
    report = {
        "diagnosis_report": "参考知识库知识",
        "structured_findings": [
            {"text": "拿知识当证据", "evidence_ids": [DOC]},
        ],
        "evidence_citations": [],
        "recommendations": [],
    }

    decision = guardrail.inspect_wireless_report(report, context)
    assert decision.decision == "BLOCKED"
    assert any(
        finding.pattern_id == "fabricated_evidence_id" for finding in decision.findings
    )

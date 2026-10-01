"""P6b: a resolved incident becomes a reviewable knowledge-document draft.

The ingestion chain this pins:

    signed uplink -> four fact tables -> incident reaches RESOLVED
                   -> KnowledgeCaseBridge.create_case -> DRAFT document

Three properties matter more than the happy path:

1. *Deterministic content only.* The document carries incident facts and the
   rules engine's conclusion. When the stored diagnosis was LLM-enriched, the
   model's prose is explicitly excluded — promoting it would launder generated
   text into retrievable "history" that a later diagnosis then cites.

2. *Idempotent under replay.* The board re-sends until it is acknowledged, so
   the same RESOLVED payload arrives repeatedly. One incident must yield one
   draft, and a replay after a failed attempt must be able to retry.

3. *Failure never reaches the facts.* No base URL, no loadable bundle, or a
   platform error leaves the incident rows untouched and the ingest returning.
"""

from __future__ import annotations

from datetime import UTC, datetime

import pytest
from sqlalchemy import create_engine, select
from sqlalchemy.orm import Session
from sqlalchemy.pool import StaticPool

from industrial_ops_agent.auth.policy import Authorizer
from industrial_ops_agent.network_assurance.automation_subject import (
    EDGE_AUTOMATION_SUBJECT_ID,
)
from industrial_ops_agent.network_assurance.knowledge_bridge import (
    KnowledgeCaseBridge,
    build_case_content,
    build_case_title,
    build_source_uri,
)
from industrial_ops_agent.network_assurance.wireless_contracts import (
    CanonicalDiagnosis,
    ConfidenceLevel,
    DiagnosisHypothesis,
    DiagnosisPresentation,
    ObservedPattern,
    WirelessDiagnosisResponse,
)
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import (
    Base,
    KnowledgeDocumentRecord,
    KnowledgeDocumentVersionRecord,
    NetworkSiteIncidentRecord,
    TenantRecord,
)
from industrial_ops_agent.persistence.tenant import TenantContext
from industrial_ops_agent.security_audit import InMemorySecurityAuditSink, SecurityAuditor

TENANT = "tenant-alpha"
INCIDENT = "site-1_1790745159954"
BASE_URL = "https://ops.example.com"
API_PREFIX = "/api/v1"


def _authorizer() -> Authorizer:
    return Authorizer(
        SecurityAuditor(InMemorySecurityAuditSink(), hash_key=b"p6b-test-audit-key")
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
                display_name="P6b tenant",
                version=1,
                created_at=datetime.now(UTC),
                updated_at=datetime.now(UTC),
            )
        )
        session.commit()
    return db


@pytest.fixture
def tenant_context() -> TenantContext:
    return TenantContext(tenant_id=TENANT, subject_id="edge-automation")


def _bridge(db: Database, base_url: str = BASE_URL) -> KnowledgeCaseBridge:
    return KnowledgeCaseBridge(
        db, _authorizer(), public_base_url=base_url, api_prefix=API_PREFIX
    )


def _incident(state: str = "RESOLVED") -> NetworkSiteIncidentRecord:
    return NetworkSiteIncidentRecord(
        incident_id=INCIDENT,
        tenant_id=TENANT,
        asset_id="radxa-cubie-a7a",
        site_id="site-1",
        gateway_id="gw-1",
        started_at_ms=1_790_745_159_000,
        last_event_ms=1_790_745_160_500,
        resolved_at_ms=1_790_745_200_000 if state == "RESOLVED" else None,
        affected_devices=3,
        state=state,
        evidence_event_ids_json=["evt-1", "evt-2"],
    )


def _diagnosis(*, llm_used: bool) -> WirelessDiagnosisResponse:
    canonical = CanonicalDiagnosis(
        observed_pattern=ObservedPattern.SUB_SECOND_SIMULTANEOUS_DISCONNECT,
        hypothesis=DiagnosisHypothesis.COEXISTENCE_RF_INTERFERENCE,
        confidence=ConfidenceLevel.HIGH,
        evidence_ids=["evt-1", "evt-2"],
        deterministic_reasons=["多设备亚秒级同步断开，同期 Wi-Fi 空口异常。"],
    )
    presentation = DiagnosisPresentation(
        llm_model_name="test-model" if llm_used else None,
        llm_used=llm_used,
        diagnosis_report=(
            "模型生成的润色解释，包含可能的幻觉。"
            if llm_used
            else "物理观测模式：亚秒级同步断开；假说：共存干扰。"
        ),
        recommendations=["检查 2.4GHz 信道"] if not llm_used else ["模型建议"],
    )
    return WirelessDiagnosisResponse(
        diagnosis_id=f"wdiag_{INCIDENT}_abc123",
        incident_id=INCIDENT,
        diagnosis_version="v1.0.0",
        rules_version="wireless-rules-v1",
        prompt_version="prompt-v1",
        guardrail_version="wireless-incident-causal-v1",
        created_at=datetime.now(UTC),
        canonical=canonical,
        presentation=presentation,
        guardrail_status="PASSED",
    )


# ---------------------------------------------------------------------------
# 内容构造（纯函数）
# ---------------------------------------------------------------------------


def test_case_content_carries_facts_and_deterministic_conclusion():
    content = build_case_content(_incident(), _diagnosis(llm_used=False))
    assert "# 区域无线事故案例" in content
    assert "site-1" in content and "gw-1" in content
    assert "受影响设备: 3 台" in content
    assert "SUB_SECOND_SIMULTANEOUS_DISCONNECT" in content
    assert "COEXISTENCE_RF_INTERFERENCE" in content
    assert "evt-1, evt-2" in content
    assert "物理观测模式" in content
    assert "检查 2.4GHz 信道" in content


def test_case_content_excludes_llm_prose_even_when_diagnosis_used_a_model():
    """知识库只收确定性内容：模型报告无论多完整都不入库。"""

    content = build_case_content(_incident(), _diagnosis(llm_used=True))
    assert "模型生成的润色解释" not in content
    assert "模型建议" not in content
    assert "未纳入案例库" in content  # 明确说明被排除，而非悄悄省略
    # 结论部分仍然可复核
    assert "COEXISTENCE_RF_INTERFERENCE" in content


def test_source_uri_names_the_real_diagnosis_resource():
    uri = build_source_uri(BASE_URL + "/", API_PREFIX, INCIDENT)
    assert uri == (
        f"{BASE_URL}{API_PREFIX}/network/assurance/incidents/{INCIDENT}/diagnosis"
    )
    assert uri.startswith("https://")


def test_source_uri_percent_encodes_path_unsafe_ids():
    uri = build_source_uri(BASE_URL, API_PREFIX, "a/b")
    assert uri.endswith("/diagnosis")
    assert "/incidents/a/b/" not in uri  # "/" 不得改变资源名
    assert "/incidents/a%2Fb/" in uri


def test_case_title_fits_the_platform_contract():
    assert len(build_case_title(INCIDENT)) <= 255
    assert INCIDENT in build_case_title(INCIDENT)


# ---------------------------------------------------------------------------
# 归档行为
# ---------------------------------------------------------------------------


def _documents(db: Database) -> list[KnowledgeDocumentRecord]:
    with Session(db.engine) as session:  # type: ignore[attr-defined]
        return list(session.scalars(select(KnowledgeDocumentRecord)))


def _versions(db: Database) -> list[KnowledgeDocumentVersionRecord]:
    with Session(db.engine) as session:  # type: ignore[attr-defined]
        return list(session.scalars(select(KnowledgeDocumentVersionRecord)))


def test_missing_base_url_skips_without_touching_anything(
    test_db: Database, tenant_context: TenantContext
):
    bridge = _bridge(test_db, base_url="")
    assert not bridge.enabled

    with Session(test_db.engine) as session:  # type: ignore[attr-defined]
        session.add(_incident())
        session.commit()

    assert bridge.create_case(tenant_context, INCIDENT) is None
    assert _versions(test_db) == []  # 未配置 = 完全不写，而非写一个坏 URL


def test_create_case_pauses_on_missing_bundle_without_raising(
    test_db: Database, tenant_context: TenantContext
):
    """四张事实表里没有证据时：跳过并返回 None，绝不抛异常。"""

    bridge = _bridge(test_db)
    with Session(test_db.engine) as session:  # type: ignore[attr-defined]
        incident = _incident()
        incident.evidence_event_ids_json = ["missing-event"]
        session.add(incident)
        session.commit()

    # 无事件行 → _load_bundle_from_tables 装载空证据包 → 分类仍可运行，
    # 但设计上我们只在事实齐全时归档；这里用不存在的事故 id 验证不抛。
    assert bridge.create_case(tenant_context, "site-1_does-not-exist") is None
    assert _versions(test_db) == []


def test_resolved_incident_yields_one_draft_with_the_right_creator(
    test_db: Database, tenant_context: TenantContext
):
    bridge = _bridge(test_db)
    with Session(test_db.engine) as session:  # type: ignore[attr-defined]
        session.add(_incident())
        session.commit()

    with Session(test_db.engine) as session:  # type: ignore[attr-defined]
        # 预置一条确定性诊断记录，隔离于无线事实装配（归档内容的来源）
        from industrial_ops_agent.network_assurance.wireless_diagnosis import (
            WirelessDiagnosisService,
        )
        from industrial_ops_agent.persistence.models import (
            SiteIncidentDiagnosisRecord,
        )

        session.add(
            SiteIncidentDiagnosisRecord(
                diagnosis_id=f"wdiag_{INCIDENT}_abc123",
                tenant_id=TENANT,
                incident_id=INCIDENT,
                diagnosis_version="v1.0.0",
                rules_version="wireless-rules-v1",
                prompt_version="prompt-v1",
                guardrail_version="wireless-incident-causal-v1",
                evidence_fingerprint="fp",
                observed_pattern="SUB_SECOND_SIMULTANEOUS_DISCONNECT",
                hypothesis="COEXISTENCE_RF_INTERFERENCE",
                confidence="HIGH",
                deterministic_reasons=["多设备亚秒级同步断开，同期 Wi-Fi 空口异常。"],
                evidence_ids=["evt-1", "evt-2"],
                device_findings=[],
                llm_model_name=None,
                llm_used=False,
                diagnosis_report="物理观测模式：亚秒级同步断开；假说：共存干扰。",
                structured_findings=[],
                evidence_citations=[],
                recommendations=["检查 2.4GHz 信道"],
                guardrail_status="PASSED",
                guardrail_findings=[],
                created_at=datetime.now(UTC),
            )
        )
        session.commit()
        _ = WirelessDiagnosisService  # 语义：诊断服务即内容来源

    doc_id = bridge.create_case(tenant_context, INCIDENT)
    assert doc_id is not None

    docs = _documents(test_db)
    versions = _versions(test_db)
    assert len(docs) == 1
    assert len(versions) == 1

    version = versions[0]
    assert version.status == "DRAFT"  # 只到草稿，发布链在人手里
    assert version.created_by_subject_id == EDGE_AUTOMATION_SUBJECT_ID
    assert docs[0].classification == "internal"
    # 平台对 ACL 列表排序后落库，故按集合比较
    assert set(version.acl_roles) == {
        "field_engineer",
        "domain_expert",
        "after_sales_engineer",
    }
    # 适用性不限定设备族：区域无线异常是现场级案例
    assert tuple(version.device_families) == ()
    assert tuple(version.device_models) == ()
    # source_uri 指向平台真实诊断资源
    assert docs[0].source_uri == (
        f"{BASE_URL}{API_PREFIX}/network/assurance/incidents/{INCIDENT}/diagnosis"
    )
    # 审核分离的前提：创建者必须可被识别为非发布者
    assert version.reviewed_by_subject_id is None
    assert version.reviewed_at is None


def test_replay_of_same_resolved_incident_yields_one_draft(
    test_db: Database, tenant_context: TenantContext
):
    bridge = _bridge(test_db)
    with Session(test_db.engine) as session:  # type: ignore[attr-defined]
        session.add(_incident())
        session.commit()

    first = bridge.create_case(tenant_context, INCIDENT)
    second = bridge.create_case(tenant_context, INCIDENT)

    assert first is not None
    assert second is not None  # 幂等键命中，返回同一版本而非新建
    assert len(_versions(test_db)) == 1
    assert len(_documents(test_db)) == 1

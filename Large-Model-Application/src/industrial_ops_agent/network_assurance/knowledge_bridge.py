"""Turn a resolved site incident into a reviewable knowledge-document draft.

Why this exists
---------------
The platform's diagnosis answers "what happened here" for one incident. The
knowledge base answers "what have we seen before" for the next one. Bridging
them means a *resolved* incident becomes a case document that a human can
review, release, and later retrieve during diagnosis.

Two rules shape this module:

1. **The draft is a proposal, not a publication.** Creation uses the narrow
   ``CREATE_KNOWLEDGE_DRAFT`` action and stops at ``DRAFT`` status. Review
   requires a different subject (the platform enforces that), building a
   release requires ``PUBLISH_KNOWLEDGE``, and publishing requires a passed
   evaluation. Nothing here can shorten that chain.

2. **Only deterministic content enters the knowledge base.** The document
   carries the incident facts, the rules engine's canonical conclusion, and —
   only when the stored diagnosis is itself deterministic — its explanation.
   A language-model report is a hallucination surface: it must never become
   retrievable "knowledge" that a later diagnosis then cites as history.

Failure never reaches the uplink. A knowledge draft is a downstream
enrichment; if it fails (bad URL config, missing diagnosis, platform error),
the facts still persisted and the incident still resolved. The operator gets a
log line, not an outage.
"""

from __future__ import annotations

import logging
from datetime import UTC, datetime, timedelta
from urllib.parse import quote

from industrial_ops_agent.auth.identity import IdentityContext, Role
from industrial_ops_agent.auth.policy import Authorizer
from industrial_ops_agent.knowledge.ingestion import KnowledgeIngestionService
from industrial_ops_agent.network_assurance.automation_subject import (
    EDGE_AUTOMATION_SUBJECT_ID,
    ensure_edge_automation_subject,
)
from industrial_ops_agent.network_assurance.wireless_contracts import (
    WirelessDiagnosisResponse,
)
from industrial_ops_agent.network_assurance.wireless_diagnosis import (
    WirelessDiagnosisService,
    WirelessIncidentNotFound,
)
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import NetworkSiteIncidentRecord
from industrial_ops_agent.persistence.tenant import TenantContext

logger = logging.getLogger("industrial_ops_agent.network_assurance.knowledge_bridge")

#: Idempotency key: one draft per incident, stable across replays and restarts.
def case_idempotency_key(incident_id: str) -> str:
    return f"weaknet-knowledge-draft:{incident_id}"


def build_case_title(incident_id: str) -> str:
    """Deterministic title (≤255 chars, enforced by the platform contract)."""

    return f"区域无线事故案例 {incident_id}"[:255]


def build_source_uri(public_base_url: str, api_prefix: str, incident_id: str) -> str:
    """The real platform resource this case documents.

    ``source_uri`` must be https (``_validate_source_uri``), so callers gate on
    a configured base URL rather than inventing an unreachable identifier.
    The incident id is percent-encoded: it lands in the path, and a "/" inside
    it would silently change which resource the URI names.
    """

    return (
        f"{public_base_url.rstrip('/')}"
        f"{api_prefix}/network/assurance/incidents/{quote(incident_id, safe='')}/diagnosis"
    )


def build_case_content(
    incident: NetworkSiteIncidentRecord,
    diagnosis: WirelessDiagnosisResponse,
) -> str:
    """Case document body: incident facts + deterministic conclusion.

    The explanation block is included only when the stored presentation is
    deterministic (``llm_used=False``). A cached LLM report may sound better,
    but promoting model prose into the knowledge base would let a future
    diagnosis retrieve it as prior fact — exactly the laundering this
    integration must not perform.
    """

    canonical = diagnosis.canonical
    presentation = diagnosis.presentation

    lines = [
        f"# 区域无线事故案例 {incident.incident_id}",
        "",
        "## 事故事实",
        f"- 现场: {incident.site_id} / 网关: {incident.gateway_id}",
        f"- 时间窗: {incident.started_at_ms} ~ {incident.last_event_ms}"
        + (f"（结案 {incident.resolved_at_ms}）" if incident.resolved_at_ms else ""),
        f"- 受影响设备: {incident.affected_devices} 台；状态: {incident.state}",
        "",
        "## 确定性诊断结论（规则引擎产出，非大模型）",
        f"- 物理观测模式: {canonical.observed_pattern}",
        f"- 假说: {canonical.hypothesis}（置信度 {canonical.confidence}）",
    ]
    if canonical.deterministic_reasons:
        lines.append("- 依据:")
        lines.extend(f"  - {reason}" for reason in canonical.deterministic_reasons)
    if canonical.evidence_ids:
        lines.append(f"- 证据 ID: {', '.join(canonical.evidence_ids)}")

    lines.extend(["", "## 解释与建议"])
    if presentation.llm_used:
        # LLM 报告不入库：只留确定性可复核的结论部分。
        lines.append("（本事故的深度解释含模型生成内容，未纳入案例库；结论以上方规则引擎产出为准。）")
    else:
        lines.append(presentation.diagnosis_report)
        if presentation.recommendations:
            lines.append("")
            lines.append("建议:")
            lines.extend(f"- {item}" for item in presentation.recommendations)

    lines.extend(
        [
            "",
            "## 来源",
            f"- 诊断记录: {diagnosis.diagnosis_id}（规则版本 {diagnosis.rules_version}）",
            "- 由边缘网关签名上行的无线事实自动归档，待人工审核后进入知识库。",
            "",
        ]
    )
    return "\n".join(lines)


class KnowledgeCaseBridge:
    """Create one knowledge draft for a resolved incident. Never raises.

    ``public_base_url`` is the platform's external origin (https). When it is
    unset the bridge reports why and does nothing — a fabricated source URI
    would be a lie about provenance, and a case document that cannot be traced
    back to its diagnosis is worse than no case document.
    """

    def __init__(
        self,
        database: Database,
        authorizer: Authorizer,
        *,
        public_base_url: str,
        api_prefix: str,
    ) -> None:
        self._database = database
        self._authorizer = authorizer
        self._public_base_url = public_base_url.rstrip("/")
        self._api_prefix = api_prefix

    @property
    def enabled(self) -> bool:
        return bool(self._public_base_url)

    def create_case(self, context: TenantContext, incident_id: str) -> str | None:
        """Return the new document id, or None when skipped or failed."""

        if not self.enabled:
            logger.warning(
                "knowledge case skipped for %s: network_public_base_url is not "
                "configured (set it to the platform's https origin)",
                incident_id,
            )
            return None

        try:
            with self._database.transaction(context) as session:
                ensure_edge_automation_subject(session, tenant_id=context.tenant_id)

            # 结案即固化确定性诊断：GET 语义不调模型，且按证据指纹幂等。
            diagnosis = WirelessDiagnosisService(self._database).get_diagnosis(
                context, incident_id
            )
            with self._database.transaction(context) as session:
                incident = session.get(NetworkSiteIncidentRecord, incident_id)
                if incident is None or incident.tenant_id != context.tenant_id:
                    logger.warning(
                        "knowledge case skipped for %s: incident row not visible", incident_id
                    )
                    return None

                ingestion = KnowledgeIngestionService(self._database, self._authorizer)
                version = ingestion.create_document(
                    self._identity(context),
                    title=build_case_title(incident.incident_id),
                    source_uri=build_source_uri(
                        self._public_base_url, self._api_prefix, incident.incident_id
                    ),
                    content=build_case_content(incident, diagnosis),
                    classification="internal",
                    acl_subject_ids=(),
                    acl_roles=("field_engineer", "domain_expert", "after_sales_engineer"),
                    device_families=(),
                    device_models=(),
                    valid_from=datetime.fromtimestamp(
                        incident.started_at_ms / 1000, tz=UTC
                    ),
                    valid_to=None,
                    idempotency_key=case_idempotency_key(incident.incident_id),
                    request_id=f"knowledge-case:{incident.incident_id}",
                )
            logger.info(
                "knowledge case draft %s created for incident %s (version %s)",
                version.document_version_id,
                incident.incident_id,
                version.version,
            )
            return version.document_version_id
        except WirelessIncidentNotFound:
            logger.warning(
                "knowledge case skipped for %s: no loadable diagnosis bundle", incident_id
            )
            return None
        except Exception as exc:  # noqa: BLE001 - 下游增强绝不拖垮事实入库
            logger.warning(
                "knowledge case failed for %s: %s", incident_id, exc, exc_info=True
            )
            return None

    @staticmethod
    def _identity(context: TenantContext) -> IdentityContext:
        """edge-automation acting for this tenant.

        FIELD_ENGINEER carries CREATE_KNOWLEDGE_DRAFT (see policy.py); it does
        not carry PUBLISH_KNOWLEDGE, so this subject can neither review its own
        draft nor release it — the platform's review separation and this role
        boundary are two independent locks on the same chain.
        """

        now = datetime.now(UTC)
        return IdentityContext(
            subject_id=EDGE_AUTOMATION_SUBJECT_ID,
            oidc_subject=EDGE_AUTOMATION_SUBJECT_ID,
            tenant_id=context.tenant_id,
            roles=frozenset({Role.FIELD_ENGINEER}),
            asset_ids=frozenset(),
            site_ids=frozenset(),
            issued_at=now,
            expires_at=now + timedelta(minutes=5),
        )

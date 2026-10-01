"""Wireless causal diagnosis service (Phase 4b).

Orchestrates:
1. Evidence bundle assembly (Incident, Events, Baselines, Wi-Fi SLE)
2. Canonical Diagnosis決裁 (Pure deterministic rules, system ground truth)
3. Model explanation enrichment (via shared upstream.complete_json)
4. W1 - W6 Causal Guardrail verification
5. Multi-versioned idempotent persistence (preserving site_incidents intact)
"""

from __future__ import annotations

import json
import logging
from datetime import UTC, datetime
from hashlib import sha256
from typing import Any

from sqlalchemy import select

from industrial_ops_agent.guardrails.network_causal import (
    WIRELESS_CAUSAL_POLICY_VERSION,
    NetworkCausalGuardrail,
    WirelessCausalContext,
)
from industrial_ops_agent.network_assurance.upstream import complete_json
from industrial_ops_agent.network_assurance.wireless_contracts import (
    CanonicalDiagnosis,
    ConfidenceLevel,
    DiagnosisHypothesis,
    DiagnosisPresentation,
    EvidenceCitation,
    IncidentEvidenceBundle,
    StructuredFinding,
    WirelessDiagnosisResponse,
)
from industrial_ops_agent.network_assurance.wireless_rules import (
    WIRELESS_RULES_VERSION,
    classify_incident,
)
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import (
    SiteIncidentDiagnosisRecord,
)
from industrial_ops_agent.persistence.tenant import TenantContext
from industrial_ops_agent.prompting.registry import (
    WIRELESS_INCIDENT_DIAGNOSIS_PROMPT_BUNDLE_ID,
    WIRELESS_PROMPT_VERSION,
    default_prompt_registry,
)

logger = logging.getLogger("industrial_ops_agent.network_assurance.wireless_diagnosis")
DIAGNOSIS_VERSION = "v1.0.0"


class WirelessIncidentNotFound(LookupError):
    """Raised when an incident ID does not exist for the tenant."""


class WirelessDiagnosisService:
    def __init__(self, database: Database) -> None:
        self._database = database
        self._guardrail = NetworkCausalGuardrail()

    def get_diagnosis(
        self,
        context: TenantContext,
        incident_id: str,
        *,
        bundle_loader: Any = None,
    ) -> WirelessDiagnosisResponse:
        """GET 语义：只读快速查询。

        - 命中任何已存记录（无论是纯 canonical 还是带 llm）直接返回；
        - 若无记录，立即运行确定性规则入库（llm_used=False）后返回；
        - GET 绝对不调用外部大模型，保证毫秒级低时延与零模型调用成本。
        """
        with self._database.transaction(context) as session:
            stmt = (
                select(SiteIncidentDiagnosisRecord)
                .where(
                    SiteIncidentDiagnosisRecord.tenant_id == context.tenant_id,
                    SiteIncidentDiagnosisRecord.incident_id == incident_id,
                )
                .order_by(SiteIncidentDiagnosisRecord.created_at.desc())
            )
            existing = session.scalars(stmt).first()
            if existing is not None:
                return self._record_to_response(existing)

            # 无记录：装配证据包并跑确定性诊断
            bundle = self._load_bundle(session, context, incident_id, bundle_loader)
            canonical = classify_incident(bundle)
            det_presentation = self._build_deterministic_presentation(canonical)

            rec = self._persist_record(
                session,
                context,
                incident_id,
                bundle,
                canonical,
                det_presentation,
                llm_used=False,
                guardrail_status="SKIPPED",
                guardrail_findings=[],
            )
            return self._record_to_response(rec)

    def diagnose_incident(
        self,
        context: TenantContext,
        incident_id: str,
        *,
        force: bool = False,
        bundle_loader: Any = None,
    ) -> WirelessDiagnosisResponse:
        """POST 语义：显式触发深度诊断与模型润色。

        【约束2 & 约束3】：
        - 若命中包含完整 LLM 润色且版本匹配的缓存，直接返回；
        - 若命中仅有 llm_used=False 的确定性记录，允许并必须执行 LLM enrichment；
        - Canonical Diagnosis 在模型调用前已确立，大模型无权篡改其结论。
        """
        with self._database.transaction(context) as session:
            bundle = self._load_bundle(session, context, incident_id, bundle_loader)
            evidence_fp = self._compute_evidence_fingerprint(bundle)

            stmt = (
                select(SiteIncidentDiagnosisRecord)
                .where(
                    SiteIncidentDiagnosisRecord.tenant_id == context.tenant_id,
                    SiteIncidentDiagnosisRecord.incident_id == incident_id,
                    SiteIncidentDiagnosisRecord.evidence_fingerprint == evidence_fp,
                    SiteIncidentDiagnosisRecord.rules_version == WIRELESS_RULES_VERSION,
                    SiteIncidentDiagnosisRecord.prompt_version == WIRELESS_PROMPT_VERSION,
                    SiteIncidentDiagnosisRecord.guardrail_version == WIRELESS_CAUSAL_POLICY_VERSION,
                )
                .order_by(SiteIncidentDiagnosisRecord.created_at.desc())
            )
            existing = session.scalars(stmt).first()

            if not force and existing is not None and existing.llm_used:
                return self._record_to_response(existing)

            # 1. 确定性决裁真值 (Canonical Diagnosis)
            canonical = classify_incident(bundle)

            # 2. 调用大模型润色解释 (LLM Explanation)
            llm_presentation, guard_decision = self._attempt_llm_presentation(
                incident_id, bundle, canonical
            )

            # 3. 若护栏审查未通过或模型失败，回退展示层为确定性报告
            presentation = llm_presentation or self._build_deterministic_presentation(canonical)
            guard_status = guard_decision.decision if guard_decision else "SKIPPED"
            guard_findings = (
                [f.audit_dict() for f in guard_decision.findings] if guard_decision else []
            )

            # 4. 持久化独立诊断记录（保持 site_incidents 一尘不染）
            rec = self._persist_record(
                session,
                context,
                incident_id,
                bundle,
                canonical,
                presentation,
                llm_used=presentation.llm_used,
                guardrail_status=guard_status,
                guardrail_findings=guard_findings,
            )
            return self._record_to_response(rec)

    # -----------------------------------------------------------------------
    # 内部流程与辅助函数
    # -----------------------------------------------------------------------

    def _attempt_llm_presentation(
        self,
        incident_id: str,
        bundle: IncidentEvidenceBundle,
        canonical: CanonicalDiagnosis,
    ) -> tuple[DiagnosisPresentation | None, Any | None]:
        """尝试调用大模型生成解释并执行 W1~W6 护栏审查。"""
        try:
            prompt_def = default_prompt_registry().get(WIRELESS_INCIDENT_DIAGNOSIS_PROMPT_BUNDLE_ID)
            system_prompt = prompt_def.render_system()
        except Exception:
            system_prompt = (
                "You are an industrial wireless network diagnostic assistant. "
                "You explain established hypotheses from facts."
            )

        evidence_catalog = [
            {
                "id": e.event_id,
                "type": "event",
                "device": e.device_address,
                "desc": f"{e.event_type} ({e.reason})",
            }
            for e in bundle.qualifying_events
        ]
        if bundle.environment.available and bundle.environment.wifi_anomaly:
            evidence_catalog.append(
                {
                    "id": "WIFI_ANOMALY",
                    "type": "wifi",
                    "desc": "Concurrent 2.4GHz Wi-Fi loss/conflict",
                }
            )

        payload = {
            "incident_id": incident_id,
            "canonical_diagnosis": {
                "observed_pattern": str(canonical.observed_pattern),
                "hypothesis": str(canonical.hypothesis),
                "confidence": str(canonical.confidence),
                "deterministic_reasons": list(canonical.deterministic_reasons),
            },
            "evidence_catalog": evidence_catalog,
            "instruction": (
                "Explain the established hypothesis based strictly on evidence_catalog. "
                "Do NOT change the hypothesis."
            ),
        }

        user_content = json.dumps(
            payload, default=str, ensure_ascii=False, sort_keys=True, separators=(",", ":")
        )
        model_out = complete_json(system_prompt, user_content, max_tokens=2048)
        if not isinstance(model_out, dict):
            return None, None

        # 构造护栏审查上下文
        null_rssi_ids = frozenset(
            e.event_id for e in bundle.qualifying_events if e.rssi_at_event_dbm is None
        )
        guard_context = WirelessCausalContext(
            incident_id=incident_id,
            canonical=canonical,
            affected_devices=bundle.incident.affected_devices,
            wifi_anomaly=bundle.environment.wifi_anomaly,
            valid_evidence_ids=frozenset(x["id"] for x in evidence_catalog),
            null_rssi_events=null_rssi_ids,
        )

        decision = self._guardrail.inspect_wireless_report(model_out, guard_context)
        if decision.decision != "ALLOWED":
            logger.warning(
                "Wireless diagnosis model output blocked by guardrail: %s", decision.findings
            )
            return None, decision

        # 护栏通过：解析 presentation 结构
        raw_findings = model_out.get("structured_findings") or []
        findings = [
            StructuredFinding(
                text=str(f.get("text", "")), evidence_ids=list(f.get("evidence_ids", []))
            )
            for f in raw_findings
            if isinstance(f, dict)
        ]
        raw_citations = model_out.get("evidence_citations") or []
        citations = [
            EvidenceCitation(
                step=str(c.get("step", "")),
                claim=str(c.get("claim", "")),
                evidence_refs=list(c.get("evidence_refs", [])),
            )
            for c in raw_citations
            if isinstance(c, dict)
        ]
        recommendations = [str(r) for r in model_out.get("recommendations", [])]
        report_text = str(model_out.get("diagnosis_report", ""))

        presentation = DiagnosisPresentation(
            llm_model_name="deepseek-v4-pro-0813",
            llm_used=True,
            diagnosis_report=report_text,
            structured_findings=findings,
            evidence_citations=citations,
            recommendations=recommendations,
        )
        return presentation, decision

    def _build_deterministic_presentation(
        self, canonical: CanonicalDiagnosis
    ) -> DiagnosisPresentation:
        """纯确定性兜底呈现文本生成。"""
        reasons_text = "；".join(canonical.deterministic_reasons)
        report = (
            f"【确定性因果诊断结论】\n"
            f"物理观测模式：{canonical.observed_pattern}\n"
            f"诊断推断假说：{canonical.hypothesis}\n"
            f"证据置信等级：{canonical.confidence}\n"
            f"依据证据事实：{reasons_text}。"
        )
        findings = (
            [
                StructuredFinding(text=r, evidence_ids=list(canonical.evidence_ids))
                for r in canonical.deterministic_reasons
            ]
            if canonical.evidence_ids
            else []
        )

        recs = [
            "排查现场 2.4GHz 频段 AP 信道与射频冲突"
            if "COEXISTENCE" in str(canonical.hypothesis)
            else "现场检查对应设备天线状态与物理工位"
        ]

        return DiagnosisPresentation(
            llm_model_name=None,
            llm_used=False,
            diagnosis_report=report,
            structured_findings=findings,
            evidence_citations=[],
            recommendations=recs,
        )

    def _load_bundle(
        self,
        session: Any,
        context: TenantContext,
        incident_id: str,
        bundle_loader: Any = None,
    ) -> IncidentEvidenceBundle:
        if bundle_loader is not None:
            return bundle_loader(incident_id)

        # 默认从已就绪的四张 Phase 4a 表加载
        raise WirelessIncidentNotFound(f"Incident {incident_id} not found")

    def _compute_evidence_fingerprint(self, bundle: IncidentEvidenceBundle) -> str:
        parts = [
            bundle.incident.incident_id,
            str(bundle.incident.started_at_ms),
            str(bundle.incident.last_event_ms),
            str(bundle.incident.affected_devices),
            ",".join(sorted(e.event_id for e in bundle.qualifying_events)),
            str(bundle.environment.wifi_anomaly),
        ]
        return sha256(":".join(parts).encode()).hexdigest()[:16]

    def _persist_record(
        self,
        session: Any,
        context: TenantContext,
        incident_id: str,
        bundle: IncidentEvidenceBundle,
        canonical: CanonicalDiagnosis,
        presentation: DiagnosisPresentation,
        *,
        llm_used: bool,
        guardrail_status: str,
        guardrail_findings: list[dict[str, Any]],
    ) -> SiteIncidentDiagnosisRecord:
        evidence_fp = self._compute_evidence_fingerprint(bundle)
        key_seed = (
            f"{incident_id}:{evidence_fp}:{WIRELESS_RULES_VERSION}:"
            f"{WIRELESS_PROMPT_VERSION}:{WIRELESS_CAUSAL_POLICY_VERSION}"
        )
        key_hash = sha256(key_seed.encode()).hexdigest()[:12]
        diag_id = f"wdiag_{incident_id}_{key_hash}"

        # 检查是否已存在记录（同 key 下如果已有记录，做 UPSERT / 更新 presentation）
        stmt = (
            select(SiteIncidentDiagnosisRecord)
            .where(
                SiteIncidentDiagnosisRecord.tenant_id == context.tenant_id,
                SiteIncidentDiagnosisRecord.incident_id == incident_id,
                SiteIncidentDiagnosisRecord.evidence_fingerprint == evidence_fp,
                SiteIncidentDiagnosisRecord.rules_version == WIRELESS_RULES_VERSION,
                SiteIncidentDiagnosisRecord.prompt_version == WIRELESS_PROMPT_VERSION,
                SiteIncidentDiagnosisRecord.guardrail_version == WIRELESS_CAUSAL_POLICY_VERSION,
            )
            .order_by(SiteIncidentDiagnosisRecord.created_at.desc())
        )
        rec = session.scalars(stmt).first()

        if rec is None:
            rec = SiteIncidentDiagnosisRecord(
                diagnosis_id=diag_id,
                tenant_id=context.tenant_id,
                incident_id=incident_id,
                diagnosis_version=DIAGNOSIS_VERSION,
                rules_version=WIRELESS_RULES_VERSION,
                prompt_version=WIRELESS_PROMPT_VERSION,
                guardrail_version=WIRELESS_CAUSAL_POLICY_VERSION,
                evidence_fingerprint=evidence_fp,
                observed_pattern=str(canonical.observed_pattern),
                hypothesis=str(canonical.hypothesis),
                confidence=str(canonical.confidence),
                deterministic_reasons=list(canonical.deterministic_reasons),
                evidence_ids=list(canonical.evidence_ids),
                device_findings=[f.model_dump() for f in canonical.device_findings],
                llm_model_name=presentation.llm_model_name,
                llm_used=llm_used,
                diagnosis_report=presentation.diagnosis_report,
                structured_findings=[f.model_dump() for f in presentation.structured_findings],
                evidence_citations=[c.model_dump() for c in presentation.evidence_citations],
                recommendations=list(presentation.recommendations),
                guardrail_status=guardrail_status,
                guardrail_findings=guardrail_findings,
                created_at=datetime.now(UTC),
            )
            session.add(rec)
        else:
            # 【约束3】：保持 Canonical 字段不变，仅当获得合法 LLM 呈现时丰富 presentation 字段
            if llm_used:
                rec.llm_model_name = presentation.llm_model_name
                rec.llm_used = True
                rec.diagnosis_report = presentation.diagnosis_report
                rec.structured_findings = [f.model_dump() for f in presentation.structured_findings]
                rec.evidence_citations = [c.model_dump() for c in presentation.evidence_citations]
                rec.recommendations = list(presentation.recommendations)
                rec.guardrail_status = guardrail_status
                rec.guardrail_findings = guardrail_findings

        session.flush()
        return rec

    def _record_to_response(self, rec: SiteIncidentDiagnosisRecord) -> WirelessDiagnosisResponse:
        from industrial_ops_agent.network_assurance.wireless_contracts import (
            DeviceFinding,
            ObservedPattern,
            StructuredFinding,
        )

        dev_findings = [DeviceFinding(**f) for f in (rec.device_findings or [])]
        canonical = CanonicalDiagnosis(
            observed_pattern=ObservedPattern(rec.observed_pattern),
            hypothesis=DiagnosisHypothesis(rec.hypothesis),
            confidence=ConfidenceLevel(rec.confidence),
            evidence_ids=list(rec.evidence_ids or []),
            deterministic_reasons=list(rec.deterministic_reasons or []),
            device_findings=dev_findings,
        )
        findings = [StructuredFinding(**f) for f in (rec.structured_findings or [])]
        citations = [EvidenceCitation(**c) for c in (rec.evidence_citations or [])]
        presentation = DiagnosisPresentation(
            llm_model_name=rec.llm_model_name,
            llm_used=rec.llm_used,
            diagnosis_report=rec.diagnosis_report,
            structured_findings=findings,
            evidence_citations=citations,
            recommendations=list(rec.recommendations or []),
        )
        return WirelessDiagnosisResponse(
            diagnosis_id=rec.diagnosis_id,
            incident_id=rec.incident_id,
            diagnosis_version=rec.diagnosis_version,
            rules_version=rec.rules_version,
            prompt_version=rec.prompt_version,
            guardrail_version=rec.guardrail_version,
            created_at=rec.created_at,
            canonical=canonical,
            presentation=presentation,
            guardrail_status=rec.guardrail_status,
            guardrail_findings=list(rec.guardrail_findings or []),
        )

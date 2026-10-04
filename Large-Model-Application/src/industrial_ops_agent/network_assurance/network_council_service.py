"""NetworkCouncilService —— 会商请求/查询与幂等（路线图 ④，止于 E1）。

职责：
  1. 组装 `CouncilInput`（从 L1 事实 + L2 诊断 + L3 预测 + 约束）；
  2. 计算 `council_input_fingerprint`，按指纹复用既有会商（幂等）；
  3. 调 `NetworkCouncilRunner` 产出 `ActionProposal[]` 并落库；
  4. 提供只读查询。

**本服务不触达 L5**：不 import `queue_action`、不写 `network_pending_actions`、
不创建 ApprovalRequest。批准与执行属于路线图 ⑤。

失败语义（fail closed）：Runner 抛 `CouncilFailure` 时，本服务把 Council 记录
写为 `FAILED` 并**清空 proposals**——绝不保存"半合法"结果。
"""

from __future__ import annotations

import hashlib
from collections.abc import Sequence
from dataclasses import dataclass
from datetime import UTC, datetime
from typing import Any
from uuid import uuid4

from sqlalchemy import select

from industrial_ops_agent.network_assurance.council_contracts import (
    COUNCIL_CONTRACT_VERSION,
    SPECIALIST_ROLES,
    ActionProposal,
    CouncilFailure,
    CouncilInput,
    CouncilStatus,
    CouncilView,
    ExpertOpinion,
    OperationalConstraints,
)
from industrial_ops_agent.network_assurance.council_fingerprint import (
    council_input_fingerprint,
)
from industrial_ops_agent.network_assurance.edge_action_catalog import (
    TOP_LEVEL_CATALOG_VERSION,
)
from industrial_ops_agent.network_assurance.network_council import (
    NetworkCouncilRunner,
)
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import (
    NetworkCouncilContributionRecord,
    NetworkCouncilRecord,
    NetworkSiteIncidentRecord,
    NetworkWirelessEventRecord,
    SiteIncidentDiagnosisRecord,
)
from industrial_ops_agent.persistence.tenant import TenantContext

#: 生产提示词包标识与哈希（由 prompting registry 提供；测试可注入）
COUNCIL_PROMPT_BUNDLE_ID = "network-operations-council-v1"


class NetworkCouncilNotFound(LookupError):
    """该事故尚无可读的会商记录。"""


@dataclass(frozen=True, slots=True)
class CouncilRequestInput:
    """请求会商所需的、来自其他层的只读材料。"""

    incident_id: str
    canonical_diagnosis_digest: str
    prediction_results: list[Any]
    device_facts: list[str]
    rf_facts: list[str]
    kernel_facts: list[str]
    diagnosis_evidence_ids: list[str]
    prediction_evidence_ids: list[str]
    affected_assets: list[str]
    production_critical: bool = False
    gateway_catalog_version: str = TOP_LEVEL_CATALOG_VERSION
    knowledge_cases: list[tuple[str, str]] | None = None
    retrieval_strategy_version: str = "rag-v1"


class NetworkCouncilService:
    def __init__(
        self,
        database: Database,
        runner: NetworkCouncilRunner,
        *,
        prompt_bundle_hash: str = f"sha256:{COUNCIL_PROMPT_BUNDLE_ID}",
    ) -> None:
        self._database = database
        self._runner = runner
        self._prompt_bundle_hash = prompt_bundle_hash

    # ------------------------------------------------------------------
    # 组装输入
    # ------------------------------------------------------------------

    def build_input(self, request: CouncilRequestInput) -> CouncilInput:
        from industrial_ops_agent.network_assurance.council_contracts import (
            DecisionEvidenceContext,
            KnowledgeCaseRef,
            KnowledgeContext,
        )

        return CouncilInput(
            incident_id=request.incident_id,
            canonical_diagnosis_digest=request.canonical_diagnosis_digest,
            prediction_results=list(request.prediction_results),
            evidence_context=DecisionEvidenceContext(
                diagnosis_evidence_ids=list(request.diagnosis_evidence_ids),
                prediction_evidence_ids=list(request.prediction_evidence_ids),
                rf_facts=list(request.rf_facts),
                kernel_facts=list(request.kernel_facts),
                device_facts=list(request.device_facts),
            ),
            operational_constraints=OperationalConstraints(
                affected_assets=list(request.affected_assets),
                production_critical=request.production_critical,
                action_catalog_version=TOP_LEVEL_CATALOG_VERSION,
                gateway_catalog_version=request.gateway_catalog_version,
            ),
            knowledge_context=KnowledgeContext(
                cases=[
                    KnowledgeCaseRef(case_id=cid, case_version=cver)
                    for cid, cver in (request.knowledge_cases or [])
                ],
                retrieval_strategy_version=request.retrieval_strategy_version,
            ),
        )

    def fingerprint(self, council_input: CouncilInput) -> str:
        return council_input_fingerprint(
            council_input,
            prompt_bundle_hash=self._prompt_bundle_hash,
            contract_version=COUNCIL_CONTRACT_VERSION,
        )

    # ------------------------------------------------------------------
    # 从真实表装配请求输入（L1 事实 + L2 诊断 + L3 预测）
    # ------------------------------------------------------------------

    def build_request_from_tables(
        self,
        context: TenantContext,
        incident_id: str,
        *,
        risk_service: Any = None,
        gateway_catalog_version: str = TOP_LEVEL_CATALOG_VERSION,
        knowledge_cases: list[tuple[str, str]] | None = None,
        retrieval_strategy_version: str = "rag-v1",
    ) -> CouncilRequestInput:
        """装配 CouncilInput 的原材料（只读；不产生任何事实）。

        - 事故本体与证据回链：``network_site_incidents``（权威事实）
        - 诊断摘要：最新一行 ``site_incident_diagnoses``（L2 产物）；缺失时用事故
          自身指纹兜底——**不允许**因缺诊断而跳过会商（诊断只影响输入语义，
          不影响可否会商）
        - 逐设备预测：受影响设备各取一次 L3 预测（``risk_service`` 注入时）；
          未注入或预测不可得时留空——预测是增强输入，不是召集前置
        """
        with self._database.transaction(context) as session:
            incident = session.scalars(
                select(NetworkSiteIncidentRecord).where(
                    NetworkSiteIncidentRecord.tenant_id == context.tenant_id,
                    NetworkSiteIncidentRecord.incident_id == incident_id,
                )
            ).first()
            if incident is None:
                raise NetworkCouncilNotFound(incident_id)

            diagnosis = session.scalars(
                select(SiteIncidentDiagnosisRecord)
                .where(
                    SiteIncidentDiagnosisRecord.tenant_id == context.tenant_id,
                    SiteIncidentDiagnosisRecord.incident_id == incident_id,
                )
                .order_by(SiteIncidentDiagnosisRecord.created_at.desc())
                .limit(1)
            ).first()

            evidence_event_ids = list(incident.evidence_event_ids_json or [])
            device_addresses: list[str] = []
            if evidence_event_ids:
                rows = session.scalars(
                    select(NetworkWirelessEventRecord).where(
                        NetworkWirelessEventRecord.tenant_id == context.tenant_id,
                        NetworkWirelessEventRecord.event_id.in_(evidence_event_ids),
                    )
                ).all()
                for row in rows:
                    if row.device_address and row.device_address not in device_addresses:
                        device_addresses.append(row.device_address)

        # L3 预测：逐设备（DEVICE 级），可为空
        predictions: list[Any] = []
        prediction_evidence_ids: list[str] = []
        if risk_service is not None:
            for address in device_addresses[:16]:
                try:
                    result = risk_service.predict_device(
                        context, incident.gateway_id or incident.asset_id, address
                    )
                except Exception:  # noqa: BLE001 - 预测不可得不得阻断会商
                    continue
                predictions.append(result)
                for evidence_id in getattr(result, "trend_evidence", []) or []:
                    prediction_evidence_ids.extend(
                        getattr(evidence_id, "evidence_ids", []) or []
                    )

        diagnosis_digest = (
            f"sha256:{diagnosis.evidence_fingerprint}"
            if diagnosis is not None
            else f"sha256:incident-{incident_id}"
        )
        return CouncilRequestInput(
            incident_id=incident_id,
            canonical_diagnosis_digest=diagnosis_digest,
            prediction_results=predictions,
            device_facts=device_addresses,
            rf_facts=[],
            kernel_facts=[],
            diagnosis_evidence_ids=(
                list(diagnosis.evidence_ids) if diagnosis is not None else []
            ),
            prediction_evidence_ids=sorted(set(prediction_evidence_ids)),
            affected_assets=[incident.gateway_id or incident.asset_id],
            production_critical=False,
            gateway_catalog_version=gateway_catalog_version,
            knowledge_cases=knowledge_cases,
            retrieval_strategy_version=retrieval_strategy_version,
        )

    # ------------------------------------------------------------------
    # 请求会商（幂等）
    # ------------------------------------------------------------------

    def request_council(
        self,
        context: TenantContext,
        request: CouncilRequestInput,
        *,
        force: bool = False,
    ) -> CouncilView:
        council_input = self.build_input(request)
        fingerprint = self.fingerprint(council_input)

        existing = self._find_by_fingerprint(context, request.incident_id, fingerprint)
        if existing is not None and not force:
            # 幂等复用：同一输入不重复花模型调用、不产生第二份决议
            return self._to_view(existing)

        attempt = (existing.attempt_count + 1) if existing is not None else 1
        council_id = existing.council_id if existing is not None else self._new_council_id()

        # 先落 QUEUED，确保失败路径也有记录（审计不留白）
        self._upsert_record(
            context,
            council_id=council_id,
            incident_id=request.incident_id,
            fingerprint=fingerprint,
            status=CouncilStatus.QUEUED,
            stage="QUEUED",
            attempt=attempt,
            proposals=None,
            opinions=None,
            failure_code=None,
        )

        try:
            result = self._runner.run(council_input)
        except CouncilFailure as failure:
            self._upsert_record(
                context,
                council_id=council_id,
                incident_id=request.incident_id,
                fingerprint=fingerprint,
                status=CouncilStatus.FAILED,
                stage="FAILED",
                attempt=attempt,
                proposals=None,  # fail closed：不留半合法结果
                opinions=None,
                failure_code=failure.failure_code,
            )
            raise

        self._upsert_record(
            context,
            council_id=council_id,
            incident_id=request.incident_id,
            fingerprint=fingerprint,
            status=result.status,
            stage=result.stage,
            attempt=attempt,
            proposals=result.proposals,
            opinions=result.expert_opinions,
            failure_code=None,
        )
        self._persist_contributions(
            context, council_id, attempt, council_input, result.expert_opinions
        )
        record = self._require_record(context, council_id)
        # ⑤：Policy 裁决 + 提案身份 + 审批落库（所有提案先有 id，allowed 才有审批）
        self._record_policy_outcomes(
            context, record=record, council_input=council_input, proposals=result.proposals
        )
        record = self._require_record(context, council_id)
        return self._to_view(record)

    # ------------------------------------------------------------------
    # 查询
    # ------------------------------------------------------------------

    def get_council(
        self, context: TenantContext, incident_id: str
    ) -> CouncilView | None:
        with self._database.transaction(context) as session:
            record = session.scalars(
                select(NetworkCouncilRecord)
                .where(
                    NetworkCouncilRecord.tenant_id == context.tenant_id,
                    NetworkCouncilRecord.incident_id == incident_id,
                )
                .order_by(NetworkCouncilRecord.updated_at.desc())
                .limit(1)
            ).first()
        return self._to_view(record) if record is not None else None

    # ------------------------------------------------------------------
    # 内部
    # ------------------------------------------------------------------

    @staticmethod
    def _new_council_id() -> str:
        return f"ncouncil-{uuid4().hex[:16]}"

    def _find_by_fingerprint(
        self, context: TenantContext, incident_id: str, fingerprint: str
    ) -> NetworkCouncilRecord | None:
        with self._database.transaction(context) as session:
            return session.scalars(
                select(NetworkCouncilRecord).where(
                    NetworkCouncilRecord.tenant_id == context.tenant_id,
                    NetworkCouncilRecord.incident_id == incident_id,
                    NetworkCouncilRecord.input_fingerprint == fingerprint,
                )
            ).first()

    def _require_record(
        self, context: TenantContext, council_id: str
    ) -> NetworkCouncilRecord:
        with self._database.transaction(context) as session:
            record = session.get(NetworkCouncilRecord, council_id)
            if record is None:  # pragma: no cover - 刚写入即消失属严重设施故障
                raise NetworkCouncilNotFound(council_id)
            return record

    def _upsert_record(
        self,
        context: TenantContext,
        *,
        council_id: str,
        incident_id: str,
        fingerprint: str,
        status: CouncilStatus,
        stage: str,
        attempt: int,
        proposals: list[ActionProposal] | None,
        opinions: list[ExpertOpinion] | None,
        failure_code: str | None,
    ) -> None:
        now = datetime.now(UTC)
        with self._database.transaction(context) as session:
            record = session.get(NetworkCouncilRecord, council_id)
            if record is None:
                record = NetworkCouncilRecord(
                    council_id=council_id,
                    tenant_id=context.tenant_id,
                    incident_id=incident_id,
                    input_fingerprint=fingerprint,
                    status=str(status),
                    stage=stage,
                    attempt_count=attempt,
                    proposals_json=(
                        [p.model_dump(mode="json") for p in proposals]
                        if proposals is not None
                        else None
                    ),
                    expert_opinions_json=(
                        [o.model_dump(mode="json") for o in opinions]
                        if opinions is not None
                        else None
                    ),
                    failure_code=failure_code,
                    requested_by_subject_id=context.subject_id,
                    completed_at=now if status is not CouncilStatus.QUEUED else None,
                    version=1,
                )
                session.add(record)
            else:
                record.status = str(status)
                record.stage = stage
                record.attempt_count = attempt
                record.proposals_json = (
                    [p.model_dump(mode="json") for p in proposals]
                    if proposals is not None
                    else None
                )
                record.expert_opinions_json = (
                    [o.model_dump(mode="json") for o in opinions]
                    if opinions is not None
                    else None
                )
                record.failure_code = failure_code
                record.completed_at = now if status is not CouncilStatus.QUEUED else None
                record.version += 1
            session.flush()

    def _record_policy_outcomes(
        self,
        context: TenantContext,
        *,
        record: Any,
        council_input: CouncilInput,
        proposals: Sequence[ActionProposal],
    ) -> None:
        """Policy 裁决 + 提案/审批持久化（⑤ 的准入与审批链入口）。

        - 每条 E1 提案先铸造稳定 ``proposal_id``（含 BLOCKED——审计要能查到
          "建议了什么、为什么被拦"）；
        - ``decide_proposal`` 计算 policy_decision（risk / approval_required /
          execution_mode / block_reason 全部是机器规则，不是模型输出）；
        - 仅 ``allowed`` 的提案创建 ApprovalRequest（P0-2：BLOCKED 零审批行）。
        """
        from industrial_ops_agent.network_assurance.action_policy import decide_proposal
        from industrial_ops_agent.network_assurance.network_action_approval import (
            NetworkActionApprovalService,
            proposal_digest,
        )

        constraints = council_input.operational_constraints
        # v1 invariant: one gateway per site——affected_assets[0] 即目标网关。
        # 不要在此泛化为跨网关 targeting；multi-gateway 时升级 Council target model。
        target_gateway = constraints.affected_assets[0] if constraints.affected_assets else ""
        decisions = [
            decide_proposal(
                proposal,
                proposal_digest=proposal_digest(proposal),
                target_gateway=target_gateway,
                production_critical=constraints.production_critical,
                cloud_catalog_version=constraints.action_catalog_version,
                gateway_catalog_version=constraints.gateway_catalog_version,
            )
            for proposal in proposals
        ]
        NetworkActionApprovalService(self._database).record_council_outcomes(
            context,
            council=record,
            proposals=proposals,
            decisions=decisions,
            initiated_by=record.requested_by_subject_id,
        )

    def _persist_contributions(
        self,
        context: TenantContext,
        council_id: str,
        attempt: int,
        council_input: CouncilInput,
        opinions: list[ExpertOpinion],
    ) -> None:
        """每位专家一条（append-only）；协调官的产出体现在 Council 行本身。"""
        now = datetime.now(UTC)
        roles = {opinion.role for opinion in opinions}
        with self._database.transaction(context) as session:
            for opinion in opinions:
                payload = opinion.model_dump(mode="json")
                digest = hashlib.sha256(
                    f"{council_id}|{attempt}|{opinion.role}".encode()
                ).hexdigest()[:32]
                session.add(
                    NetworkCouncilContributionRecord(
                        contribution_id=f"nccontrib-{digest}",
                        tenant_id=context.tenant_id,
                        council_id=council_id,
                        attempt_number=attempt,
                        agent_role=str(opinion.role),
                        input_digest=council_input.canonical_diagnosis_digest,
                        output_json=payload,
                        output_digest="sha256:"
                        + hashlib.sha256(
                            str(sorted(payload.items())).encode()
                        ).hexdigest()[:32],
                        model_release_id=None,
                        prompt_bundle_hash=self._prompt_bundle_hash,
                        completed_at=now,
                    )
                )
            session.flush()
        assert roles <= set(SPECIALIST_ROLES)  # 专家贡献只含三位专家

    @staticmethod
    def _to_view(record: NetworkCouncilRecord) -> CouncilView:
        proposals = [
            ActionProposal.model_validate(item) for item in (record.proposals_json or [])
        ]
        opinions = [
            ExpertOpinion.model_validate(item) for item in (record.expert_opinions_json or [])
        ]
        return CouncilView(
            council_id=record.council_id,
            incident_id=record.incident_id,
            input_fingerprint=record.input_fingerprint,
            status=CouncilStatus(record.status),
            stage=record.stage,
            proposals=proposals,
            expert_opinions=opinions,
            failure_code=record.failure_code,
            requested_by_subject_id=record.requested_by_subject_id,
            created_at=record.created_at,
            updated_at=record.updated_at,
            version=record.version,
        )

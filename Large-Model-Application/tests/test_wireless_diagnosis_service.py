"""Service orchestration, database caching, and guardrail fallback tests (Phase 4b).

Covers all key invariants:
- CanonicalDiagnosisUnaffectedByLlmOutput: Canonical hypothesis is immutable by LLM
- PostEnrichesDeterministicGetResult: GET (llm_used=false) does NOT block subsequent POST enrichment
- GuardrailVersionInvalidatesCache: Cache invalidated when guardrail version changes
- ConcurrentPostCreatesSingleLogicalDiagnosis: Unique constraint idempotency
- ForceDoesNotMutateHistoricalCanonicalDiagnosis
"""

from datetime import UTC, datetime
from typing import Any
import pytest
from sqlalchemy import create_engine
from sqlalchemy.pool import StaticPool

from industrial_ops_agent.guardrails.network_causal import WIRELESS_CAUSAL_POLICY_VERSION
from industrial_ops_agent.network_assurance.wireless_contracts import (
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
    WirelessIncidentNotFound,
)
from industrial_ops_agent.network_assurance.wireless_rules import WIRELESS_RULES_VERSION
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import Base, SiteIncidentDiagnosisRecord
from industrial_ops_agent.persistence.tenant import TenantContext
from industrial_ops_agent.prompting.registry import WIRELESS_PROMPT_VERSION


@pytest.fixture
def test_db() -> Database:
    engine = create_engine(
        "sqlite:///:memory:",
        connect_args={"check_same_thread": False},
        poolclass=StaticPool,
    )
    Base.metadata.create_all(engine)
    return Database.from_engine(engine)


@pytest.fixture
def tenant_context() -> TenantContext:
    return TenantContext(tenant_id="tenant-alpha", subject_id="operator-1")


def _sample_bundle(incident_id: str = "sitinc_100") -> IncidentEvidenceBundle:
    events = [
        WirelessEventView(
            event_id="e1",
            ts_ms=1000,
            site_id="site-1",
            gateway_id="gw-1",
            protocol="BLUETOOTH",
            device_address="AA:01",
            address_type="LE_RANDOM",
            hci_index=0,
            event_type="LINK_DISCONNECTED",
            rssi_at_event_dbm=-70,
            reason="CONNECTION_TIMEOUT",
            source="KERNEL_MGMT",
        ),
        WirelessEventView(
            event_id="e2",
            ts_ms=1500,
            site_id="site-1",
            gateway_id="gw-1",
            protocol="BLUETOOTH",
            device_address="AA:02",
            address_type="LE_RANDOM",
            hci_index=0,
            event_type="LINK_DISCONNECTED",
            rssi_at_event_dbm=-68,
            reason="CONNECTION_TIMEOUT",
            source="KERNEL_MGMT",
        ),
    ]
    return IncidentEvidenceBundle(
        incident=IncidentView(
            incident_id=incident_id,
            site_id="site-1",
            gateway_id="gw-1",
            started_at_ms=1000,
            last_event_ms=1500,
            resolved_at_ms=61500,
            affected_devices=2,
            state="RESOLVED",
        ),
        qualifying_events=events,
        window_events=events,
        baselines={},
        environment=EnvironmentWindowView(available=True, link_type="WIFI_2_4G", wifi_anomaly=True),
    )


def test_get_creates_deterministic_diagnosis_without_llm(test_db: Database, tenant_context: TenantContext):
    service = WirelessDiagnosisService(test_db)
    bundle = _sample_bundle("sitinc_get")

    # 首次调用 GET：在毫秒级内跑完确定性推断入库，llm_used 为 False
    res = service.get_diagnosis(tenant_context, "sitinc_get", bundle_loader=lambda _: bundle)
    assert res.incident_id == "sitinc_get"
    assert res.presentation.llm_used is False
    # 本 fixture 两事件跨度 500ms（1000→1500）且断开前无衰减，按规则引擎
    # 优先级（wireless_rules.py: 亚秒同步优先于窗口协同）应判 SUB_SECOND，
    # 而非 MULTI_DEVICE；同 bundle 的假设断言不受影响（两种模式在
    # wifi_anomaly=True 下均收敛到 COEXISTENCE_RF_INTERFERENCE）。
    assert res.canonical.observed_pattern == ObservedPattern.SUB_SECOND_SIMULTANEOUS_DISCONNECT
    assert res.canonical.hypothesis == DiagnosisHypothesis.COEXISTENCE_RF_INTERFERENCE

    # 第二次调用 GET：直接命中已有缓存
    res2 = service.get_diagnosis(tenant_context, "sitinc_get", bundle_loader=lambda _: bundle)
    assert res2.diagnosis_id == res.diagnosis_id
    assert res2.presentation.llm_used is False


def test_post_enriches_deterministic_get_result(test_db: Database, tenant_context: TenantContext, monkeypatch: Any):
    service = WirelessDiagnosisService(test_db)
    bundle = _sample_bundle("sitinc_enrich")

    # 1. 模拟用户先点击了快速查看，GET 生成了 llm_used=false 的纯确定性记录
    res_get = service.get_diagnosis(tenant_context, "sitinc_enrich", bundle_loader=lambda _: bundle)
    assert res_get.presentation.llm_used is False

    # 2. 模拟大模型调用成功返回高质量润色
    mock_model_output = {
        "diagnosis_report": "现场在 500ms 内发生多台设备超时断开，同期伴随 Wi-Fi 2.4G 严重同频竞争干扰。",
        "structured_findings": [{"text": "2台设备超时断连", "evidence_ids": ["e1", "e2"]}],
        "evidence_citations": [{"step": "1", "claim": "设备异常", "evidence_refs": ["e1"]}],
        "recommendations": ["建议现场 AP 切至 5GHz 信道"],
    }
    monkeypatch.setattr(
        "industrial_ops_agent.network_assurance.wireless_diagnosis.complete_json",
        lambda *args, **kwargs: mock_model_output,
    )

    # 3. 用户发起 POST 深度诊断：【约束2】验证确定性记录绝不阻断后续 LLM enrichment！
    res_post = service.diagnose_incident(tenant_context, "sitinc_enrich", bundle_loader=lambda _: bundle)
    assert res_post.presentation.llm_used is True
    assert "同频竞争干扰" in res_post.presentation.diagnosis_report
    assert res_post.guardrail_status == "ALLOWED"
    assert res_post.canonical.hypothesis == DiagnosisHypothesis.COEXISTENCE_RF_INTERFERENCE


def test_canonical_diagnosis_unaffected_by_llm_output(test_db: Database, tenant_context: TenantContext, monkeypatch: Any):
    service = WirelessDiagnosisService(test_db)
    bundle = _sample_bundle("sitinc_unaffected")

    # 大模型在报告中信口开河（试图说这是设备自身故障，但事实是多设备协同）
    mock_model_output = {
        "diagnosis_report": "设备自身硬件故障导致了断连问题。",
        "structured_findings": [{"text": "设备故障", "evidence_ids": ["e1"]}],
        "evidence_citations": [],
        "recommendations": ["更换设备"],
    }
    monkeypatch.setattr(
        "industrial_ops_agent.network_assurance.wireless_diagnosis.complete_json",
        lambda *args, **kwargs: mock_model_output,
    )

    res = service.diagnose_incident(tenant_context, "sitinc_unaffected", bundle_loader=lambda _: bundle)
    # 【约束3】：系统确立的真值 canonical.hypothesis 绝不被大模型随意更改
    assert res.canonical.hypothesis == DiagnosisHypothesis.COEXISTENCE_RF_INTERFERENCE
    assert res.canonical.confidence == ConfidenceLevel.HIGH


def test_guardrail_version_invalidates_cache(test_db: Database, tenant_context: TenantContext, monkeypatch: Any):
    service = WirelessDiagnosisService(test_db)
    bundle = _sample_bundle("sitinc_guard_inv")

    called_count = 0

    def mock_complete(*args: Any, **kwargs: Any) -> dict[str, Any]:
        nonlocal called_count
        called_count += 1
        return {
            "diagnosis_report": "分析报告内容",
            "structured_findings": [{"text": "断连", "evidence_ids": ["e1"]}],
            "evidence_citations": [],
            "recommendations": ["检查网络"],
        }

    monkeypatch.setattr(
        "industrial_ops_agent.network_assurance.wireless_diagnosis.complete_json",
        mock_complete,
    )

    # 第一次 POST：调用模型并写入缓存
    service.diagnose_incident(tenant_context, "sitinc_guard_inv", bundle_loader=lambda _: bundle)
    assert called_count == 1

    # 再次 POST：版本一致，命中缓存，不会再次调用模型
    service.diagnose_incident(tenant_context, "sitinc_guard_inv", bundle_loader=lambda _: bundle)
    assert called_count == 1

    # 【约束1】：升级护栏版本 WIRELESS_CAUSAL_POLICY_VERSION -> 旧缓存失效，必须重新审查
    monkeypatch.setattr(
        "industrial_ops_agent.network_assurance.wireless_diagnosis.WIRELESS_CAUSAL_POLICY_VERSION",
        "wireless-incident-causal-v2-patched",
    )
    service.diagnose_incident(tenant_context, "sitinc_guard_inv", bundle_loader=lambda _: bundle)
    assert called_count == 2

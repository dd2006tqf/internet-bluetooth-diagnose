"""Unit tests for wireless causal guardrail invariants (W1 - W6)."""

import pytest
from industrial_ops_agent.guardrails.network_causal import (
    NetworkCausalGuardrail,
    WirelessCausalContext,
    WIRELESS_CAUSAL_POLICY_VERSION,
)
from industrial_ops_agent.network_assurance.wireless_contracts import (
    CanonicalDiagnosis,
    ConfidenceLevel,
    DiagnosisHypothesis,
    ObservedPattern,
)


def _make_context(
    affected_devices: int = 2,
    pattern: ObservedPattern = ObservedPattern.MULTI_DEVICE_CONCURRENT_ANOMALY,
    hypothesis: DiagnosisHypothesis = DiagnosisHypothesis.COEXISTENCE_RF_INTERFERENCE,
    wifi_anomaly: bool = True,
    valid_evidence_ids: frozenset[str] | None = None,
    null_rssi_events: frozenset[str] | None = None,
) -> WirelessCausalContext:
    canonical = CanonicalDiagnosis(
        observed_pattern=pattern,
        hypothesis=hypothesis,
        confidence=ConfidenceLevel.HIGH,
        evidence_ids=list(valid_evidence_ids or {"E1", "E2"}),
    )
    return WirelessCausalContext(
        incident_id="sitinc_123",
        canonical=canonical,
        affected_devices=affected_devices,
        wifi_anomaly=wifi_anomaly,
        valid_evidence_ids=valid_evidence_ids or frozenset({"E1", "E2"}),
        null_rssi_events=null_rssi_events or frozenset(),
    )


def test_w1_cannot_blame_explicit_termination_as_fault():
    guard = NetworkCausalGuardrail()
    ctx = _make_context(
        pattern=ObservedPattern.EXPLICIT_TERMINATION_PATTERN,
        hypothesis=DiagnosisHypothesis.NORMAL_USER_ACTIVITY,
    )
    # 模型输出中包含了"严重空口干扰"或"网络故障"等诬陷用词
    bad_report = {
        "diagnosis_report": "现场发生严重无线电磁干扰，导致设备通信故障断开。",
        "structured_findings": [{"text": "设备断开", "evidence_ids": ["E1"]}],
        "evidence_citations": [],
        "recommendations": ["更换信道"],
    }
    decision = guard.inspect_wireless_report(bad_report, ctx)
    assert decision.decision == "BLOCKED"
    assert any(f.pattern_id == "explicit_termination_blamed_as_fault" for f in decision.findings)


def test_w2_multi_device_incident_cannot_blame_single_device_battery():
    guard = NetworkCausalGuardrail()
    ctx = _make_context(affected_devices=3)
    bad_report = {
        "diagnosis_report": "本次区域事故主要是因为设备电池没电导致突然失联。",
        "structured_findings": [{"text": "电池耗尽失联", "evidence_ids": ["E1"]}],
        "evidence_citations": [],
        "recommendations": ["给设备充电"],
    }
    decision = guard.inspect_wireless_report(bad_report, ctx)
    assert decision.decision == "BLOCKED"
    assert any(f.pattern_id == "single_device_blame_on_multi_device_incident" for f in decision.findings)


def test_w3_wifi_blame_requires_concurrent_wifi_anomaly():
    guard = NetworkCausalGuardrail()
    # 没有 Wi-Fi 异常 (wifi_anomaly = False)
    ctx = _make_context(
        wifi_anomaly=False,
        hypothesis=DiagnosisHypothesis.AREA_RF_DEGRADATION_OR_OBSTACLE
    )
    bad_report = {
        "diagnosis_report": "本次故障是由 Wi-Fi 2.4GHz 信道严重冲突和大流量下载干扰引起的。",
        "structured_findings": [{"text": "Wi-Fi 干扰", "evidence_ids": ["E1"]}],
        "evidence_citations": [],
        "recommendations": ["关闭 Wi-Fi"],
    }
    decision = guard.inspect_wireless_report(bad_report, ctx)
    assert decision.decision == "BLOCKED"
    assert any(f.pattern_id == "wifi_blame_without_wifi_anomaly" for f in decision.findings)


def test_w4_null_rssi_treated_as_zero():
    guard = NetworkCausalGuardrail()
    ctx = _make_context(null_rssi_events=frozenset({"E1"}))
    bad_report = {
        "diagnosis_report": "设备 E1 断开瞬时信号强度达到 0 dBm 极高功率，疑似过载。",
        "structured_findings": [{"text": "信号强度 0 dBm", "evidence_ids": ["E1"]}],
        "evidence_citations": [],
        "recommendations": ["拉开距离"],
    }
    decision = guard.inspect_wireless_report(bad_report, ctx)
    assert decision.decision == "BLOCKED"
    assert any(f.pattern_id == "null_rssi_treated_as_zero" for f in decision.findings)


def test_w5_fabricated_evidence_id():
    guard = NetworkCausalGuardrail()
    ctx = _make_context(valid_evidence_ids=frozenset({"E1", "E2"}))
    bad_report = {
        "diagnosis_report": "多台设备受到环境噪音影响断连。",
        # E999 不在 valid_evidence_ids 里
        "structured_findings": [{"text": "设备断连", "evidence_ids": ["E999"]}],
        "evidence_citations": [],
        "recommendations": ["排查现场"],
    }
    decision = guard.inspect_wireless_report(bad_report, ctx)
    assert decision.decision == "BLOCKED"
    assert any(f.pattern_id == "fabricated_evidence_id" for f in decision.findings)


def test_w6_structured_finding_empty_evidence_ids():
    guard = NetworkCausalGuardrail()
    ctx = _make_context(valid_evidence_ids=frozenset({"E1", "E2"}))
    bad_report = {
        "diagnosis_report": "多台设备受到环境噪音影响断连。",
        # 事实断言没有提供任何 Evidence ID 引用
        "structured_findings": [{"text": "设备断连", "evidence_ids": []}],
        "evidence_citations": [],
        "recommendations": ["排查现场"],
    }
    decision = guard.inspect_wireless_report(bad_report, ctx)
    assert decision.decision == "BLOCKED"
    assert any(f.pattern_id == "missing_evidence_citation" for f in decision.findings)


def test_clean_report_passes_guardrail():
    guard = NetworkCausalGuardrail()
    ctx = _make_context(
        affected_devices=2,
        hypothesis=DiagnosisHypothesis.COEXISTENCE_RF_INTERFERENCE,
        wifi_anomaly=True,
        valid_evidence_ids=frozenset({"E1", "E2"})
    )
    good_report = {
        "diagnosis_report": "现场在 30 秒内有 2 台机械臂传感器相继超时断连，同期检测到 Wi-Fi 2.4GHz 频段冲突与丢包恶化，分析为同频竞争干扰。",
        "structured_findings": [
            {"text": "2 台设备超时断开", "evidence_ids": ["E1", "E2"]}
        ],
        "evidence_citations": [
            {"step": "1", "claim": "设备断连", "evidence_refs": ["E1"]}
        ],
        "recommendations": ["建议现场 AP 切换至 5GHz 信道"],
    }
    decision = guard.inspect_wireless_report(good_report, ctx)
    assert decision.decision == "ALLOWED"
    assert decision.policy_version == WIRELESS_CAUSAL_POLICY_VERSION
    assert len(decision.findings) == 0

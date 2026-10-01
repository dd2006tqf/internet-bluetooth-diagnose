"""Unit tests for wireless causal rules engine (Phase 4b).

100% pure deterministic tests without DB or network dependencies.
"""

import pytest
from industrial_ops_agent.network_assurance.wireless_contracts import (
    BaselineView,
    ConfidenceLevel,
    DiagnosisHypothesis,
    EnvironmentWindowView,
    IncidentEvidenceBundle,
    IncidentView,
    ObservedPattern,
    WirelessEventView,
)
from industrial_ops_agent.network_assurance.wireless_rules import (
    WIRELESS_RULES_VERSION,
    classify_incident,
)


def _make_event(
    event_id: str,
    device: str,
    ts_ms: int,
    event_type: str = "LINK_DISCONNECTED",
    reason: str = "CONNECTION_TIMEOUT",
    rssi: int | None = -65,
) -> WirelessEventView:
    return WirelessEventView(
        event_id=event_id,
        ts_ms=ts_ms,
        site_id="site-1",
        gateway_id="gw-1",
        protocol="BLUETOOTH",
        device_address=device,
        address_type="LE_RANDOM",
        hci_index=0,
        event_type=event_type,
        rssi_at_event_dbm=rssi,
        reason=reason,
        source="KERNEL_MGMT",
    )


def _make_bundle(
    events: list[WirelessEventView],
    affected_devices: int = 2,
    wifi_anomaly: bool = False,
    coexistence_warning: bool = False,
    link_type: str = "WIFI_2_4G",
    baselines: dict[str, BaselineView] | None = None,
    env_available: bool = True,
) -> IncidentEvidenceBundle:
    min_ts = min((e.ts_ms for e in events), default=1000)
    max_ts = max((e.ts_ms for e in events), default=1000)
    return IncidentEvidenceBundle(
        incident=IncidentView(
            incident_id="sitinc_test",
            site_id="site-1",
            gateway_id="gw-1",
            started_at_ms=min_ts,
            last_event_ms=max_ts,
            resolved_at_ms=max_ts + 60000,
            affected_devices=affected_devices,
            state="RESOLVED",
        ),
        qualifying_events=events,
        window_events=events,
        baselines=baselines or {},
        environment=EnvironmentWindowView(
            available=env_available,
            link_type=link_type,
            wifi_anomaly=wifi_anomaly,
            coexistence_warning=coexistence_warning,
        ),
    )


def test_pure_deterministic_property():
    evs = [
        _make_event("e1", "AA:01", 1000),
        _make_event("e2", "AA:02", 1500),
    ]
    bundle = _make_bundle(evs)
    v1 = classify_incident(bundle)
    v2 = classify_incident(bundle)
    assert v1 == v2
    assert WIRELESS_RULES_VERSION.startswith("rules-")


def test_explicit_termination_pattern_and_hypothesis():
    evs = [
        _make_event("e1", "AA:01", 1000, reason="REMOTE_USER_TERMINATED"),
        _make_event("e2", "AA:02", 1500, reason="LOCAL_HOST_TERMINATED"),
    ]
    bundle = _make_bundle(evs)
    verdict = classify_incident(bundle)
    assert verdict.observed_pattern == ObservedPattern.EXPLICIT_TERMINATION_PATTERN
    assert verdict.hypothesis == DiagnosisHypothesis.NORMAL_USER_ACTIVITY
    assert verdict.confidence == ConfidenceLevel.HIGH


def test_adapter_stall_sub_second_sync():
    # 2台设备在 800ms 内相继断开，无前置劣化，无 Wi-Fi 异常
    evs = [
        _make_event("e1", "AA:01", 1000),
        _make_event("e2", "AA:02", 1800),
    ]
    bundle = _make_bundle(evs, affected_devices=2, wifi_anomaly=False)
    verdict = classify_incident(bundle)
    assert verdict.observed_pattern == ObservedPattern.SUB_SECOND_SIMULTANEOUS_DISCONNECT
    assert verdict.hypothesis == DiagnosisHypothesis.LOCAL_ADAPTER_OR_HOST_STALL
    # 【约束2】：未有外部 host/controller stall 证据前，封顶 MEDIUM
    assert verdict.confidence == ConfidenceLevel.MEDIUM


def test_adapter_stall_contradicted_by_wifi_anomaly():
    # 虽然亚秒同步断开，但伴随并发 Wi-Fi 严重异常，排除本地芯片假死，归因于共存/空口冲击
    evs = [
        _make_event("e1", "AA:01", 1000),
        _make_event("e2", "AA:02", 1500),
    ]
    bundle = _make_bundle(evs, affected_devices=2, wifi_anomaly=True, coexistence_warning=True)
    verdict = classify_incident(bundle)
    assert verdict.hypothesis == DiagnosisHypothesis.COEXISTENCE_RF_INTERFERENCE
    assert verdict.confidence == ConfidenceLevel.HIGH


def test_coexistence_rf_interference_detection():
    # 2台设备在 30 秒内超时断开，Wi-Fi 2.4G 丢包高
    evs = [
        _make_event("e1", "AA:01", 10000),
        _make_event("e2", "AA:02", 25000),
    ]
    bundle = _make_bundle(evs, affected_devices=2, wifi_anomaly=True)
    verdict = classify_incident(bundle)
    assert verdict.observed_pattern == ObservedPattern.MULTI_DEVICE_CONCURRENT_ANOMALY
    assert verdict.hypothesis == DiagnosisHypothesis.COEXISTENCE_RF_INTERFERENCE
    assert verdict.confidence == ConfidenceLevel.HIGH


def test_area_rf_degradation_without_wifi_correlation():
    # 多设备协同掉线，但 Wi-Fi 指标完全平稳且无告警 -> 区域无线衰落/遮挡/通用噪声
    evs = [
        _make_event("e1", "AA:01", 10000),
        _make_event("e2", "AA:02", 25000),
    ]
    bundle = _make_bundle(evs, affected_devices=2, wifi_anomaly=False)
    verdict = classify_incident(bundle)
    assert verdict.observed_pattern == ObservedPattern.MULTI_DEVICE_CONCURRENT_ANOMALY
    assert verdict.hypothesis == DiagnosisHypothesis.AREA_RF_DEGRADATION_OR_OBSTACLE
    assert verdict.confidence == ConfidenceLevel.HIGH


def test_single_device_distance_or_shadowing():
    # 单设备先有 LINK_DEGRADED，断连瞬时 RSSI 大跌
    evs = [
        _make_event("e0", "AA:01", 5000, event_type="LINK_DEGRADED", rssi=-80),
        _make_event("e1", "AA:01", 10000, event_type="LINK_DISCONNECTED", rssi=-85),
    ]
    baselines = {
        "AA:01": BaselineView(device_address="AA:01", baseline_rssi_dbm=-60, state="STABLE")
    }
    bundle = _make_bundle(evs, affected_devices=1, baselines=baselines)
    verdict = classify_incident(bundle)
    assert verdict.observed_pattern == ObservedPattern.SINGLE_DEVICE_GRADUAL_DEGRADATION
    assert verdict.hypothesis == DiagnosisHypothesis.DEVICE_DISTANCE_OR_SHADOWING
    assert verdict.confidence == ConfidenceLevel.MEDIUM


def test_hypothesis_confidence_cap_for_abrupt_loss():
    # 【约束4】：单设备突然断开且断开瞬时信号很好 -> DEVICE_POWER_LOSS_OR_CRASH
    # 无论条件多么支持，置信度上限强制封顶 LOW
    evs = [
        _make_event("e1", "AA:01", 10000, event_type="LINK_DISCONNECTED", rssi=-55),
    ]
    baselines = {
        "AA:01": BaselineView(device_address="AA:01", baseline_rssi_dbm=-55, state="STABLE")
    }
    bundle = _make_bundle(evs, affected_devices=1, baselines=baselines)
    verdict = classify_incident(bundle)
    assert verdict.observed_pattern == ObservedPattern.SINGLE_DEVICE_ABRUPT_LOSS
    assert verdict.hypothesis == DiagnosisHypothesis.DEVICE_POWER_LOSS_OR_CRASH
    assert verdict.confidence == ConfidenceLevel.LOW  # 必须封顶 LOW


def test_null_rssi_never_treated_as_zero():
    # RSSI 为 None 时，严禁当作 0 dBm 计算，不能误诊为突发断开或距离超限
    evs = [
        _make_event("e1", "AA:01", 10000, event_type="LINK_DISCONNECTED", rssi=None),
    ]
    bundle = _make_bundle(evs, affected_devices=1)
    verdict = classify_incident(bundle)
    # 因为缺少 RSSI 证据支撑特定假说，归入证据不足
    assert verdict.hypothesis == DiagnosisHypothesis.INSUFFICIENT_EVIDENCE
    assert verdict.confidence == ConfidenceLevel.INSUFFICIENT
    assert any("未采集" in r for r in verdict.deterministic_reasons)

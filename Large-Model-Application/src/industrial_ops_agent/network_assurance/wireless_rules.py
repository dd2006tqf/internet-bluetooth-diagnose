"""Pure deterministic causal rules engine for wireless site incidents (Phase 4b).

Strictly pure functions: zero network, zero I/O, zero clock reading.
Implements:
1. Pattern extraction from immutable facts
2. Hypothesis filtering & contradiction elimination
3. Confidence determination with structural caps
"""

from __future__ import annotations

from typing import Final

from industrial_ops_agent.network_assurance.wireless_contracts import (
    HYPOTHESIS_MAX_CONFIDENCE,
    CanonicalDiagnosis,
    ConfidenceLevel,
    DeviceFinding,
    DiagnosisHypothesis,
    IncidentEvidenceBundle,
    ObservedPattern,
)

WIRELESS_RULES_VERSION: Final[str] = "rules-2026.10"


def classify_incident(bundle: IncidentEvidenceBundle) -> CanonicalDiagnosis:
    """对事故证据包进行纯确定性物理模式提取与因果决裁。"""
    events = bundle.qualifying_events

    # 1. 基础事实特征提取
    evidence_ids = [e.event_id for e in events]
    reasons: list[str] = []

    # 检查是否全部为主动正常断开
    is_all_explicit_term = False
    if bundle.window_events:
        term_reasons = {"REMOTE_USER_TERMINATED", "LOCAL_HOST_TERMINATED"}
        disconn_events = [e for e in bundle.window_events if e.event_type == "LINK_DISCONNECTED"]
        if disconn_events and all(e.reason in term_reasons for e in disconn_events):
            is_all_explicit_term = True

    if is_all_explicit_term:
        reasons.append(
            "所记录断开事件全部为用户主动断连（RemoteUserTerminated / LocalHostTerminated）。"
        )
        return CanonicalDiagnosis(
            observed_pattern=ObservedPattern.EXPLICIT_TERMINATION_PATTERN,
            hypothesis=DiagnosisHypothesis.NORMAL_USER_ACTIVITY,
            confidence=ConfidenceLevel.HIGH,
            evidence_ids=evidence_ids,
            deterministic_reasons=reasons,
        )

    # 2. 检查多设备亚秒级同步断开
    dev_addresses = {e.device_address for e in events}
    disconn_ts = [e.ts_ms for e in events if e.event_type == "LINK_DISCONNECTED"]
    has_prior_degraded = any(e.event_type == "LINK_DEGRADED" for e in events)

    is_sub_second_sync = False
    if (
        len(dev_addresses) >= 2
        and disconn_ts
        and (max(disconn_ts) - min(disconn_ts)) <= 1000
        and not has_prior_degraded
    ):
        is_sub_second_sync = True

    # 3. 检查环境 Wi-Fi 异常与频段冲突证据
    env = bundle.environment
    wifi_abnormal = (
        env.available
        and env.link_type.startswith("WIFI")
        and (env.wifi_anomaly or env.coexistence_warning)
    )

    # 4. 模式决裁 (Pattern Classifier)
    if is_sub_second_sync:
        pattern = ObservedPattern.SUB_SECOND_SIMULTANEOUS_DISCONNECT
    elif len(dev_addresses) >= 2:
        pattern = ObservedPattern.MULTI_DEVICE_CONCURRENT_ANOMALY
    elif len(dev_addresses) == 1:
        if has_prior_degraded:
            pattern = ObservedPattern.SINGLE_DEVICE_GRADUAL_DEGRADATION
        else:
            pattern = ObservedPattern.SINGLE_DEVICE_ABRUPT_LOSS
    else:
        pattern = ObservedPattern.SPARSE_UNCLASSIFIED_EVENTS

    # 5. 假说决裁 (Hypothesis Engine: 排除与反证机制)
    hypothesis: DiagnosisHypothesis
    raw_confidence: ConfidenceLevel

    if pattern == ObservedPattern.SUB_SECOND_SIMULTANEOUS_DISCONNECT:
        # 反证排除：若同期存在严重 Wi-Fi 恶化，说明更可能是空口射频冲击，而非本地适配器假死
        if wifi_abnormal:
            hypothesis = DiagnosisHypothesis.COEXISTENCE_RF_INTERFERENCE
            raw_confidence = ConfidenceLevel.HIGH
            reasons.append("多设备亚秒级同步断开，且同期检测到 Wi-Fi 2.4GHz 空口异常与频段竞争。")
        else:
            hypothesis = DiagnosisHypothesis.LOCAL_ADAPTER_OR_HOST_STALL
            raw_confidence = ConfidenceLevel.MEDIUM
            reasons.append(
                "多设备在 1000ms 内亚秒级同步断开且断开前无衰减，高度符合本机适配器/总线假死特征。"
            )

    elif pattern == ObservedPattern.MULTI_DEVICE_CONCURRENT_ANOMALY:
        if wifi_abnormal:
            hypothesis = DiagnosisHypothesis.COEXISTENCE_RF_INTERFERENCE
            raw_confidence = ConfidenceLevel.HIGH
            reasons.append(
                f"事故窗口内 {len(dev_addresses)} 台设备协同异常，"
                "且伴随 Wi-Fi 2.4GHz 恶化/频段冲突警告。"
            )
        else:
            hypothesis = DiagnosisHypothesis.AREA_RF_DEGRADATION_OR_OBSTACLE
            raw_confidence = ConfidenceLevel.HIGH
            reasons.append(
                f"事故窗口内 {len(dev_addresses)} 台设备协同异常，"
                "但未观察到 Wi-Fi 强相关性，指向区域 RF 遮挡或环境噪声。"
            )

    elif pattern == ObservedPattern.SINGLE_DEVICE_GRADUAL_DEGRADATION:
        dev_addr = next(iter(dev_addresses))
        baseline = bundle.baselines.get(dev_addr)
        last_rssi = next(
            (e.rssi_at_event_dbm for e in reversed(events) if e.rssi_at_event_dbm is not None), None
        )

        if last_rssi is None:
            hypothesis = DiagnosisHypothesis.INSUFFICIENT_EVIDENCE
            raw_confidence = ConfidenceLevel.INSUFFICIENT
            reasons.append(
                "设备虽然经历前置劣化，但断开瞬时有效 RSSI 未采集，无法定量判定距离/遮挡。"
            )
        elif (
            baseline
            and baseline.baseline_rssi_dbm is not None
            and (last_rssi <= baseline.baseline_rssi_dbm - 10)
        ):
            hypothesis = DiagnosisHypothesis.DEVICE_DISTANCE_OR_SHADOWING
            raw_confidence = ConfidenceLevel.MEDIUM
            reasons.append(
                f"设备信号从基线 {baseline.baseline_rssi_dbm}dBm 显著突跌至 {last_rssi}dBm "
                "后断开，符合移动超距或物理遮挡。"
            )
        else:
            hypothesis = DiagnosisHypothesis.DEVICE_DISTANCE_OR_SHADOWING
            raw_confidence = ConfidenceLevel.LOW
            reasons.append(f"设备经历信号劣化后断开 (末次 RSSI: {last_rssi}dBm)。")

    elif pattern == ObservedPattern.SINGLE_DEVICE_ABRUPT_LOSS:
        dev_addr = next(iter(dev_addresses))
        last_rssi = next(
            (e.rssi_at_event_dbm for e in reversed(events) if e.rssi_at_event_dbm is not None), None
        )

        if last_rssi is None:
            hypothesis = DiagnosisHypothesis.INSUFFICIENT_EVIDENCE
            raw_confidence = ConfidenceLevel.INSUFFICIENT
            reasons.append("设备突发失联，但有效 RSSI 未采集，无法形成可靠假设。")
        elif last_rssi >= -65:
            # 信号良好却突发失联 -> 掉电或崩溃
            hypothesis = DiagnosisHypothesis.DEVICE_POWER_LOSS_OR_CRASH
            raw_confidence = ConfidenceLevel.LOW
            reasons.append(
                f"设备在良好信号 ({last_rssi}dBm) 下无前置衰减突发超时断开，"
                "疑似设备掉电、重启或固件崩溃。"
            )
        else:
            hypothesis = DiagnosisHypothesis.INSUFFICIENT_EVIDENCE
            raw_confidence = ConfidenceLevel.INSUFFICIENT
            reasons.append(f"设备弱信号 ({last_rssi}dBm) 下突发断开，无充分前置数据，不予妄猜。")

    else:
        hypothesis = DiagnosisHypothesis.INSUFFICIENT_EVIDENCE
        raw_confidence = ConfidenceLevel.INSUFFICIENT
        reasons.append("证据稀疏且不满足任何确定性模式。")

    # 6. 置信度上限刚性收敛 (HYPOTHESIS_MAX_CONFIDENCE)
    max_cap = HYPOTHESIS_MAX_CONFIDENCE.get(hypothesis, ConfidenceLevel.LOW)
    final_confidence = _cap_confidence(raw_confidence, max_cap)

    # 7. 组装设备级子发现 (Device Findings)
    device_findings: list[DeviceFinding] = []
    for d in dev_addresses:
        d_evs = [e for e in events if e.device_address == d]
        has_deg = any(e.event_type == "LINK_DEGRADED" for e in d_evs)
        d_pat = (
            ObservedPattern.SINGLE_DEVICE_GRADUAL_DEGRADATION
            if has_deg
            else ObservedPattern.SINGLE_DEVICE_ABRUPT_LOSS
        )
        d_hyp = (
            DiagnosisHypothesis.DEVICE_DISTANCE_OR_SHADOWING
            if has_deg
            else DiagnosisHypothesis.DEVICE_POWER_LOSS_OR_CRASH
        )
        device_findings.append(
            DeviceFinding(
                device_address=d,
                observed_pattern=d_pat,
                hypothesis=d_hyp,
                confidence=ConfidenceLevel.LOW,
                note=f"事件数: {len(d_evs)}",
            )
        )

    return CanonicalDiagnosis(
        observed_pattern=pattern,
        hypothesis=hypothesis,
        confidence=final_confidence,
        evidence_ids=evidence_ids,
        deterministic_reasons=reasons,
        device_findings=device_findings if len(dev_addresses) >= 2 else [],
    )


def _cap_confidence(current: ConfidenceLevel, cap: ConfidenceLevel) -> ConfidenceLevel:
    """按 HIGH > MEDIUM > LOW > INSUFFICIENT 施加置信度上限。"""
    ranks = {
        ConfidenceLevel.INSUFFICIENT: 0,
        ConfidenceLevel.LOW: 1,
        ConfidenceLevel.MEDIUM: 2,
        ConfidenceLevel.HIGH: 3,
    }
    if ranks[current] > ranks[cap]:
        return cap
    return current

"""B8 acceptance tests for the deterministic RiskWindow predictor (contract C).

Ten mandated cases + window-derivation proof:

  1. InsufficientDataProducesNoRiskJudgment
  2. DifferentDeviceBaselinesNeverMix
  3. SlowBaselineDriftIsDetectable          (needs baseline trajectory, not
                                              current-minus-current-baseline)
  4. RiskAndConfidenceAreIndependent         (HIGH risk + LOW confidence is legal)
  5. MissingMetricIsNotZero
  6. DriverContributionsNormalizeOverAvailableMetrics
  7. SameHistorySameCanonicalPrediction      (injected clock, bit-identical)
  8. ParameterVersionChangeInvalidatesReplayIdentity
  9. AbsoluteRssiAloneCannotDeclareFailure   (criterion is RF AND service impact)
 10. DevicePredictionDoesNotImplicitlyCreateSitePrediction
 +   WindowComesFromCriterionCrossingNotRiskLabel (24-72h is not an alias of HIGH)

All histories are injected; no DB, no network, no wall clock.
"""

from __future__ import annotations

from datetime import UTC, datetime

import pytest

from industrial_ops_agent.network_assurance.risk_window import (
    FAILURE_CRITERION_ID,
    MetricSample,
    RiskWindowConfig,
    model_version_for,
    predict,
)
from industrial_ops_agent.network_assurance.wireless_contracts import (
    DataSufficiency,
    InsufficientPrediction,
    RiskLevel,
    RiskWindow,
    SubjectType,
    TrendDirection,
    UnsupportedSubjectType,
    WindowEstimationStatus,
)

HOUR_MS = 3_600_000
FIXED_NOW = datetime(2026, 10, 4, 8, 0, tzinfo=UTC)
fixed_clock = lambda: FIXED_NOW  # noqa: E731 - test-local deterministic clock


def _series(values: list[float], *, start_ms: int = 0, step_hours: float = 24.0,
            prefix: str = "e", mature: bool = True) -> list[MetricSample]:
    return [
        MetricSample(
            ts_ms=start_ms + int(i * step_hours * HOUR_MS),
            value=value,
            evidence_id=f"{prefix}{i}",
            mature=mature,
        )
        for i, value in enumerate(values)
    ]


# 3. SlowBaselineDriftIsDetectable
def test_slow_baseline_drift_is_detectable() -> None:
    # 5 天内基线自 -60 缓慢漂移到 -70：每天只差几 dB，
    # current - current_baseline 恒为 -1/-2dB，只有 trajectory 能看见。
    baseline = _series([-60.0, -62.5, -65.0, -67.5, -70.0])
    result = predict(
        SubjectType.DEVICE, "dev-drift", {"baseline_rssi_dbm": baseline}, clock=fixed_clock
    )
    assert isinstance(result, RiskWindow)
    assert result.risk_level in (RiskLevel.MEDIUM, RiskLevel.HIGH)
    # 驱动必须来自 baseline 轨迹本身
    assert any(d.metric == "baseline_rssi_dbm" for d in result.drivers)
    assert any("漂移" in t.statement for t in result.trend_evidence)
    # 丢包组件缺失 → 有风险判断但时间不可估（三态之二）
    assert result.window_estimation_status is WindowEstimationStatus.UNAVAILABLE
    assert result.window_earliest_hours is None
    assert result.data_sufficiency is DataSufficiency.PARTIAL


# 1. InsufficientDataProducesNoRiskJudgment
def test_insufficient_data_produces_no_risk_judgment() -> None:
    # 空历史 / 样本太少
    tiny = _series([-60.0, -61.0])  # 2 < min_samples
    result = predict(
        SubjectType.DEVICE, "dev-empty", {"baseline_rssi_dbm": tiny}, clock=fixed_clock
    )
    assert isinstance(result, InsufficientPrediction)
    assert result.data_sufficiency is DataSufficiency.INSUFFICIENT
    assert result.missing_requirements
    # "证据不足"不是风险等级——结构上根本没有 risk_level 字段
    assert not hasattr(result, "risk_level")


# 2. DifferentDeviceBaselinesNeverMix
def test_different_device_baselines_never_mix() -> None:
    # A：健康锚点 -45，缓慢漂移 4dB → 应判退化
    device_a = _series([-45.0, -46.0, -47.0, -48.0, -49.0], prefix="a")
    # B：本来就在 -82 附近微幅起伏（锚点不同、无退化趋势，终值略低于锚点
    #    使它也有 trajectory 证据可断言锚点未被 A 污染）
    device_b = _series([-82.0, -81.5, -82.5, -82.0, -82.5], prefix="b")

    result_a = predict(SubjectType.DEVICE, "dev-a", {"baseline_rssi_dbm": device_a},
                       clock=fixed_clock)
    result_b = predict(SubjectType.DEVICE, "dev-b", {"baseline_rssi_dbm": device_b},
                       clock=fixed_clock)

    assert isinstance(result_a, RiskWindow) and isinstance(result_b, RiskWindow)
    assert result_a.subject_id == "dev-a"
    assert result_b.subject_id == "dev-b"
    # A 在退化，B 稳定：两个锚点（-45 vs -82）互不污染
    assert result_a.risk_level is RiskLevel.MEDIUM
    assert result_b.risk_level is RiskLevel.LOW
    assert "-45dBm" in " ".join(t.statement for t in result_a.trend_evidence)
    assert "-82dBm" in " ".join(t.statement for t in result_b.trend_evidence)


# 4. RiskAndConfidenceAreIndependent
def test_risk_and_confidence_are_independent() -> None:
    # 单指标持续恶化且幅度大 → 风险 HIGH；
    # 但其余指标缺失 → 数据不充分 → 置信度 LOW（合法组合）
    baseline = _series([-60.0, -62.5, -65.0, -67.5, -70.0])
    result = predict(
        SubjectType.DEVICE, "dev-single", {"baseline_rssi_dbm": baseline}, clock=fixed_clock
    )
    assert isinstance(result, RiskWindow)
    assert result.risk_level is RiskLevel.HIGH
    assert result.prediction_confidence is RiskLevel.LOW
    assert result.data_sufficiency is DataSufficiency.PARTIAL


# 5. MissingMetricIsNotZero
def test_missing_metric_is_not_zero() -> None:
    # baseline 退化 + jitter 正常在场，packet_loss 完全缺失
    baseline = _series([-60.0, -61.0, -62.0, -63.0, -64.0])
    jitter = _series([20.0, 20.0, 21.0, 20.0, 20.0], prefix="j")
    result = predict(
        SubjectType.DEVICE,
        "dev-partial",
        {"baseline_rssi_dbm": baseline, "median_jitter_ms": jitter},
        clock=fixed_clock,
    )
    assert isinstance(result, RiskWindow)
    driver_names = [d.metric for d in result.drivers]
    # 缺失指标绝不出现在驱动里（当 0 塞进分母会稀释贡献度）
    assert "packet_loss_rate" not in driver_names
    # 数据充分度如实反映缺失
    assert result.data_sufficiency is DataSufficiency.PARTIAL
    # 只有 baseline 在恶化 → 它独占 100% 贡献度
    assert [d.metric for d in result.drivers] == ["baseline_rssi_dbm"]
    assert result.drivers[0].contribution_pct == pytest.approx(100.0, abs=1.0)


# 6. DriverContributionsNormalizeOverAvailableMetrics
def test_driver_contributions_normalize_over_available_metrics() -> None:
    baseline = _series([-60.0, -61.0, -62.0, -63.0, -64.0])
    jitter = _series([20.0, 28.75, 37.5, 46.25, 55.0], prefix="j")
    loss = _series([0.001, 0.003, 0.005, 0.008, 0.011], prefix="l")
    result = predict(
        SubjectType.DEVICE,
        "dev-multi",
        {"baseline_rssi_dbm": baseline, "median_jitter_ms": jitter,
         "packet_loss_rate": loss},
        clock=fixed_clock,
    )
    assert isinstance(result, RiskWindow)
    assert len(result.drivers) >= 2
    total = sum(d.contribution_pct for d in result.drivers)
    assert 99.0 <= total <= 101.0, f"contribution total {total} != 100 ±1"
    # 每个驱动都有确定性算法需要的方向与证据
    for driver in result.drivers:
        assert driver.direction in (TrendDirection.DETERIORATING, TrendDirection.STABLE)
        assert driver.evidence_ids


# 7. SameHistorySameCanonicalPrediction
def test_same_history_same_canonical_prediction() -> None:
    baseline = _series([-60.0, -61.0, -62.5, -64.0, -66.0])
    jitter = _series([20.0, 26.0, 33.0, 41.0, 50.0], prefix="j")

    def build() -> dict[str, list[MetricSample]]:
        return {
            "baseline_rssi_dbm": list(baseline),
            "median_jitter_ms": list(jitter),
        }

    first = predict(SubjectType.DEVICE, "dev-replay", build(), clock=fixed_clock)
    second = predict(SubjectType.DEVICE, "dev-replay", build(), clock=fixed_clock)
    # 注入时钟 + 纯函数：同一历史逐字段相等（含 generated_at 与 model_version）
    assert first == second


# 8. ParameterVersionChangeInvalidatesReplayIdentity
def test_parameter_version_change_invalidates_replay_identity() -> None:
    default_cfg = RiskWindowConfig()
    tuned_cfg = RiskWindowConfig(min_velocity_norm_per_hour=0.002)
    assert model_version_for(default_cfg) != model_version_for(tuned_cfg)

    baseline = _series([-60.0, -62.0, -64.0, -66.0, -68.0])
    result_default = predict(
        SubjectType.DEVICE, "dev-fp", {"baseline_rssi_dbm": baseline},
        clock=fixed_clock, config=default_cfg,
    )
    result_tuned = predict(
        SubjectType.DEVICE, "dev-fp", {"baseline_rssi_dbm": baseline},
        clock=fixed_clock, config=tuned_cfg,
    )
    assert result_default.model_version != result_tuned.model_version
    assert result_default.model_version.startswith("riskwindow-v1+sha256:")


# 9. AbsoluteRssiAloneCannotDeclareFailure
def test_absolute_rssi_alone_cannot_declare_failure() -> None:
    # RF 组件已到判据终点（相对锚点突跌 15dB），
    # 但丢包组件平稳无趋势 → AND 判据不可估 → 不得宣布"即将失效"
    baseline = _series([-60.0, -63.75, -67.5, -71.25, -75.0])
    loss = _series([0.001, 0.001, 0.0012, 0.001, 0.001], prefix="l")
    result = predict(
        SubjectType.DEVICE,
        "dev-rf-only",
        {"baseline_rssi_dbm": baseline, "packet_loss_rate": loss},
        clock=fixed_clock,
    )
    assert isinstance(result, RiskWindow)
    assert result.risk_level is RiskLevel.HIGH  # 风险判断成立
    # 但窗口 = failure 判据的函数：AND 缺一项 → UNAVAILABLE，而非硬给 24-72h
    assert result.window_estimation_status is WindowEstimationStatus.UNAVAILABLE
    assert result.window_earliest_hours is None
    assert result.window_latest_hours is None
    assert result.failure_criterion_id == FAILURE_CRITERION_ID


# Window derivation proof: bucket comes from criterion crossing, not risk label
def test_window_comes_from_criterion_crossing_not_risk_label() -> None:
    # RF crossing ≈ 24h；丢包 crossing ≈ 96h；AND → max(24, 96) = 96h
    # → 分桶 (72, 168]，而不是 HIGH 惯例的 24–72h
    baseline = _series([-60.0, -63.0, -66.0, -69.0, -72.0])      # norm 0→0.8
    loss = _series([0.0015, 0.004875, 0.00825, 0.011625, 0.015], prefix="l")  # norm 0→0.5
    jitter = _series([20.0, 20.0, 21.0, 20.0, 20.0], prefix="j")  # 稳定但活跃

    result = predict(
        SubjectType.DEVICE,
        "dev-crossing",
        {"baseline_rssi_dbm": baseline, "packet_loss_rate": loss,
         "median_jitter_ms": jitter},
        clock=fixed_clock,
    )
    assert isinstance(result, RiskWindow)
    assert result.risk_level is RiskLevel.HIGH
    assert result.window_estimation_status is WindowEstimationStatus.AVAILABLE
    # crossing 96h 落在 (72, 168] 桶——若按 HIGH 硬映射会错成 24-72
    assert (result.window_earliest_hours, result.window_latest_hours) == (72, 168)


# 10. DevicePredictionDoesNotImplicitlyCreateSitePrediction
def test_device_prediction_does_not_implicitly_create_site_prediction() -> None:
    baseline = _series([-60.0, -62.0, -64.0, -66.0, -68.0])
    # 显式请求 SITE → 显式拒绝（枚举保留，实现 v1 不支持）
    with pytest.raises(UnsupportedSubjectType):
        predict(SubjectType.SITE, "site-1", {"baseline_rssi_dbm": baseline},
                clock=fixed_clock)
    # 不存在"把多台设备平均成站点风险"的暗门：DEVICE 请求只按 subject_id 出结果
    device_result = predict(
        SubjectType.DEVICE, "site-1", {"baseline_rssi_dbm": baseline}, clock=fixed_clock
    )
    assert isinstance(device_result, RiskWindow)
    assert device_result.subject_type is SubjectType.DEVICE

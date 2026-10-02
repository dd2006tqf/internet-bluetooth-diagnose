"""Unit tests for wireless predictive maintenance dynamic registry and RUL prediction."""

import pytest
from industrial_ops_agent.predictive_maintenance.wireless_rul import (
    DEFAULT_WIRELESS_METRICS_REGISTRY,
    DegradationDirection,
    MetricFeatureSpec,
    WirelessRulPredictor,
)


def test_perfect_health_index():
    predictor = WirelessRulPredictor()
    perfect_data = {
        "baseline_rssi_dbm": -60,
        "current_rssi_dbm": -60,
        "packet_loss_rate": 0.0,
        "jitter_ms": 5.0,
    }
    hi = predictor.compute_health_index(perfect_data)
    assert hi == 1.0


def test_failure_threshold_health_index_zero():
    predictor = WirelessRulPredictor()
    failed_data = {
        "baseline_rssi_dbm": -60,
        "current_rssi_dbm": -75,   # delta = 15dB -> max failure
        "packet_loss_rate": 0.03,  # 3% -> max failure
        "jitter_ms": 50.0,         # 50ms -> max failure
    }
    hi = predictor.compute_health_index(failed_data)
    assert hi == 0.0


def test_graceful_missing_metrics_handling():
    predictor = WirelessRulPredictor()
    # 某些传感器只上报了 RSSI，没有抖动和丢包
    sparse_data = {
        "baseline_rssi_dbm": -60,
        "current_rssi_dbm": -67.5, # 偏离 7.5dB -> 50% 退化
    }
    hi = predictor.compute_health_index(sparse_data)
    assert 0.49 <= hi <= 0.51


def test_extensible_new_metric_without_model_changes():
    # 模拟未来新增电池电压 (Battery Voltage, 越小越差)
    battery_spec = MetricFeatureSpec(
        name="battery_mv",
        direction=DegradationDirection.SMALLER_IS_WORSE,
        healthy_baseline=3300.0,
        failure_threshold=2400.0,
        default_weight=2.0,
        extractor=lambda d: float(d.get("battery_mv")) if d.get("battery_mv") is not None else None,
    )
    custom_specs = list(DEFAULT_WIRELESS_METRICS_REGISTRY) + [battery_spec]
    predictor = WirelessRulPredictor(custom_specs)

    data = {
        "baseline_rssi_dbm": -60,
        "current_rssi_dbm": -60,
        "packet_loss_rate": 0.0,
        "jitter_ms": 5.0,
        "battery_mv": 2850.0, # 处于 3300 和 2400 中点 -> 50% 退化
    }
    hi = predictor.compute_health_index(data)
    # 因为电池有退化，综合健康度从 1.0 平滑下降
    assert 0.75 <= hi <= 0.88


def test_rul_hours_estimation():
    predictor = WirelessRulPredictor()
    # 构造过去 4 小时逐渐恶化的时序数据
    history = [
        {"baseline_rssi_dbm": -60, "current_rssi_dbm": -60, "packet_loss_rate": 0.0},
        {"baseline_rssi_dbm": -60, "current_rssi_dbm": -62, "packet_loss_rate": 0.005},
        {"baseline_rssi_dbm": -60, "current_rssi_dbm": -65, "packet_loss_rate": 0.010},
        {"baseline_rssi_dbm": -60, "current_rssi_dbm": -67, "packet_loss_rate": 0.015},
    ]
    res = predictor.estimate_rul_hours(history, hours_per_point=1.0)
    assert res["status"] in {"HEALTHY", "DEGRADING"}
    assert res["predicted_rul_hours"] is not None
    assert res["predicted_rul_hours"] > 0

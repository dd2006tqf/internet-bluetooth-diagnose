"""Predictive Maintenance: Dynamic multi-metric feature registry and RUL model (Phase 4b).

Domain adaptation from bearing/motor wear to wireless link degradation:
- Decouples raw physical metrics from the core mathematical degradation model.
- Metrics are dynamically normalized into a [0.0 (Healthy) -> 1.0 (Failure)] vector.
- Gracefully handles missing metrics (heterogeneous sensor compatibility).
- Extensible via MetricFeatureSpec declarations without touching the core TTF estimator.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import StrEnum
from typing import Any, Callable


class DegradationDirection(StrEnum):
    """退化方向定义"""
    BIGGER_IS_WORSE = "BIGGER_IS_WORSE"    # 数值越大越差：丢包率、延迟抖动、重传数、掉基线差值
    SMALLER_IS_WORSE = "SMALLER_IS_WORSE"  # 数值越小越差：RSSI 绝对值、信噪比 SNR、电池电压


@dataclass(frozen=True)
class MetricFeatureSpec:
    """声明式指标元数据规范"""
    name: str
    direction: DegradationDirection
    healthy_baseline: float
    failure_threshold: float
    default_weight: float = 1.0
    extractor: Callable[[dict[str, Any]], float | None] | None = None


# ---------------------------------------------------------------------------
# 现存核心指标注册表（预留未来动态增改能力）
# ---------------------------------------------------------------------------
DEFAULT_WIRELESS_METRICS_REGISTRY: list[MetricFeatureSpec] = [
    MetricFeatureSpec(
        name="delta_rssi_db",
        direction=DegradationDirection.BIGGER_IS_WORSE,
        healthy_baseline=0.0,      # 紧贴健康基线
        failure_threshold=15.0,    # 继承板端 Phase 2 劣化的 -15dBm 门限
        default_weight=2.0,
        extractor=lambda d: max(0.0, float(d.get("baseline_rssi_dbm", -60) - d.get("current_rssi_dbm", -60)))
        if d.get("baseline_rssi_dbm") is not None and d.get("current_rssi_dbm") is not None
        else None,
    ),
    MetricFeatureSpec(
        name="packet_loss_rate",
        direction=DegradationDirection.BIGGER_IS_WORSE,
        healthy_baseline=0.0,
        failure_threshold=0.03,    # 继承板端 Reliability BAD 的 3% 门限
        default_weight=1.5,
        extractor=lambda d: float(d.get("packet_loss_rate")) if d.get("packet_loss_rate") is not None else None,
    ),
    MetricFeatureSpec(
        name="jitter_ms",
        direction=DegradationDirection.BIGGER_IS_WORSE,
        healthy_baseline=5.0,
        failure_threshold=50.0,    # 抖动发散上限
        default_weight=1.0,
        extractor=lambda d: float(d.get("jitter_ms")) if d.get("jitter_ms") is not None else None,
    ),
]


class WirelessRulPredictor:
    """无线通信链路剩余可用时间（RUL / TTF）估算引擎。"""

    def __init__(self, specs: list[MetricFeatureSpec] | None = None) -> None:
        self._specs = specs or DEFAULT_WIRELESS_METRICS_REGISTRY

    def compute_health_index(self, data: dict[str, Any]) -> float:
        """根据当前指标计算综合健康度指数 HI in [0.0, 1.0]。

        1.0 = 极其健康，0.0 = 达到失效门限。
        """
        deg_sum = 0.0
        weight_sum = 0.0

        for spec in self._specs:
            if spec.extractor is None:
                continue
            val = spec.extractor(data)
            if val is None:
                continue

            # 归一化无量纲退化度 [0.0, 1.0]
            if spec.direction == DegradationDirection.BIGGER_IS_WORSE:
                norm_deg = (val - spec.healthy_baseline) / max(1e-6, (spec.failure_threshold - spec.healthy_baseline))
            else:
                norm_deg = (spec.healthy_baseline - val) / max(1e-6, (spec.healthy_baseline - spec.failure_threshold))

            norm_deg = max(0.0, min(1.0, norm_deg))
            deg_sum += norm_deg * spec.default_weight
            weight_sum += spec.default_weight

        if weight_sum <= 0.0:
            return 1.0

        avg_deg = deg_sum / weight_sum
        return max(0.0, min(1.0, 1.0 - avg_deg))

    def estimate_rul_hours(self, history_points: list[dict[str, Any]], hours_per_point: float = 1.0) -> dict[str, Any]:
        """根据历史时序计算退化斜率，推算剩余可用小时数。"""
        if len(history_points) < 2:
            return {
                "health_index": 1.0,
                "predicted_rul_hours": None,
                "status": "INSUFFICIENT_DATA",
                "explanation": "数据点不足，至少需要2个历史时序点方可进行线性退化拟合。",
            }

        hi_series = [self.compute_health_index(p) for p in history_points]
        current_hi = hi_series[-1]

        # 计算近期平均退化速率 (ΔHI / Δt)
        total_delta = hi_series[0] - hi_series[-1]
        time_span = (len(hi_series) - 1) * hours_per_point

        if total_delta <= 0:
            return {
                "health_index": round(current_hi, 3),
                "predicted_rul_hours": None,
                "status": "STABLE",
                "explanation": "链路指标处于平稳或自我恢复状态，未检测到单调下行退化趋势。",
            }

        rate_per_hour = total_delta / max(1e-3, time_span)
        # 距离彻底归零 (HI = 0.0) 剩余时间
        rul_hours = current_hi / rate_per_hour

        return {
            "health_index": round(current_hi, 3),
            "predicted_rul_hours": round(rul_hours, 1),
            "status": "DEGRADING" if current_hi < 0.7 else "HEALTHY",
            "explanation": f"链路健康指数当前为 {round(current_hi, 2)}，正以每小时 {round(rate_per_hour, 4)} 的速率衰减，预计约 {round(rul_hours, 1)} 小时后可能达到断连失效门限。",
        }

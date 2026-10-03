"""Deterministic RiskWindow predictor (L3, contract C).

V1 pipeline — no ML, pure deterministic/statistical trend detection:

    MetricFeatureContract (声明式特征契约)
        → normalized trend features (deviation / velocity / persistence)
        → deterministic risk classification (risk & confidence 分开)
        → FailureCriterion-driven window estimation (组件 crossing → AND/OR 聚合)
        → explainable drivers (风险驱动贡献度, normalized over ACTIVE metrics only)

Hard rules enforced here (契约 C 七条纪律):
  - never emits "预计 X 小时后断连" — only bucketed window or UNAVAILABLE;
  - risk_level has no INSUFFICIENT — no judgment = InsufficientPrediction;
  - single feature reaching 1.0 ≠ failure — window follows the failure criterion
    (RF relative degradation AND service impact), component-wise crossing;
  - operational_baseline (Phase 2 adaptive) is NOT used for drift — L3 watches
    baseline_trajectory against a frozen prediction_reference anchor;
  - missing metrics are excluded from the contribution denominator (NULL ≠ 0);
  - every output-affecting parameter is fingerprinted into model_version.

Everything here is pure: no clock reads (injected), no DB access (histories
come from MetricHistoryProvider), no network. Same history + same clock →
bit-identical PredictionResult.
"""

from __future__ import annotations

import hashlib
import json
from collections.abc import Callable, Mapping, Sequence
from dataclasses import asdict, dataclass
from datetime import UTC, datetime
from typing import Final

from industrial_ops_agent.network_assurance.wireless_contracts import (
    DataSufficiency,
    InsufficientPrediction,
    PredictionResult,
    RiskDriver,
    RiskLevel,
    RiskWindow,
    SubjectType,
    TrendDirection,
    TrendEvidence,
    UnsupportedSubjectType,
    WindowEstimationStatus,
)

# ---------------------------------------------------------------------------
# Failure criterion & version constants
# ---------------------------------------------------------------------------

#: 失效判据 = 相对退化(RF) AND 业务影响(丢包)。判据实际驱动窗口估计，
#: 不是结果里的审计标签——组件 crossing 按 AND 取 max。
FAILURE_CRITERION_ID: Final[str] = "weaknet-rf-service-impact"
FAILURE_CRITERION_VERSION: Final[str] = "v1"
BASE_RISKWINDOW_MODEL_VERSION: Final[str] = "riskwindow-v1"

#: 与板端 Phase 2 对齐的物理常量（绝对值只作判据组件，绝不作全局 failure truth）
RF_FAIL_DROP_DB: Final[float] = 15.0  # 相对锚点突跌 15dB = RF 组件失效
PACKET_LOSS_FAIL_RATE: Final[float] = 0.03  # 板端 Reliability BAD = 3%
LOSS_EVIDENCE_UNIT_SCALE: Final[float] = 0.01  # 板端 evidence 以 % 上报 → ratio


# ---------------------------------------------------------------------------
# Feature contracts (声明式)
# ---------------------------------------------------------------------------


@dataclass(frozen=True, slots=True)
class MetricFeatureContract:
    """声明式特征契约（比"字段列表 Registry"更强：含 failure 语义与基线域）。

    baseline_domain:
      - ``prediction_reference``: normal = 冻结锚点（首个成熟样本），观测其轨迹漂移；
      - ``absolute``: normal = normal_value（物理常量，如丢包 0 / 抖动基线）。
    failure_threshold: 偏离量达到该值即判据组件失效（deviation 单位）。
    """

    name: str
    data_source: str
    source_kind: str  # "baseline_change_log" | "snapshot_evidence"
    source_metric: str  # snapshot_evidence 时的 evidence metric 名
    timestamp_semantics: str
    missing_strategy: str
    baseline_domain: str
    normal_value: float | None
    failure_threshold: float
    failure_semantics: str
    worsening: str  # "higher_is_worse" | "lower_is_worse"（deviation 恒 >= 0 为更差）
    applicable_subjects: tuple[str, ...]
    weight: float
    criterion_component: bool
    unit_scale: float = 1.0

    def summary(self) -> dict[str, object]:
        """参与 model_version 指纹的紧凑表示（不含 data_source 等不影响输出的字段）。"""
        return {
            "n": self.name,
            "bd": self.baseline_domain,
            "nv": self.normal_value,
            "ft": self.failure_threshold,
            "w": self.worsening,
            "wt": self.weight,
            "cc": self.criterion_component,
            "us": self.unit_scale,
        }


FEATURE_CONTRACTS: Final[tuple[MetricFeatureContract, ...]] = (
    MetricFeatureContract(
        name="baseline_rssi_dbm",
        data_source="network_device_baseline_history",
        source_kind="baseline_change_log",
        source_metric="baseline_rssi_dbm",
        timestamp_semantics="wall_ms(ingest-observed)",
        missing_strategy="exclude",
        baseline_domain="prediction_reference",
        normal_value=None,  # 锚点即 normal（冻结，不随 Phase 2 自适应基线漂移）
        failure_threshold=RF_FAIL_DROP_DB,
        failure_semantics="relative_degradation",
        worsening="lower_is_worse",
        applicable_subjects=("DEVICE",),
        weight=2.0,
        criterion_component=True,
    ),
    MetricFeatureContract(
        name="packet_loss_rate",
        data_source="network_snapshots.experience_json",
        source_kind="snapshot_evidence",
        source_metric="wifi_loss_rate",
        timestamp_semantics="wall_ms(snapshot.captured_at)",
        missing_strategy="exclude",
        baseline_domain="absolute",
        normal_value=0.0,
        failure_threshold=PACKET_LOSS_FAIL_RATE,
        failure_semantics="service_impact",
        worsening="higher_is_worse",
        applicable_subjects=("DEVICE",),
        weight=1.5,
        criterion_component=True,
        unit_scale=LOSS_EVIDENCE_UNIT_SCALE,  # evidence 4.2(%) → 0.042(ratio)
    ),
    MetricFeatureContract(
        name="median_jitter_ms",
        data_source="network_snapshots.experience_json",
        source_kind="snapshot_evidence",
        source_metric="median_jitter_ms",
        timestamp_semantics="wall_ms(snapshot.captured_at)",
        missing_strategy="exclude",
        baseline_domain="absolute",
        normal_value=20.0,
        failure_threshold=40.0,  # 20ms 基线 + 40ms 偏离 = 60ms 严重抖动
        failure_semantics="service_impact",
        worsening="higher_is_worse",
        applicable_subjects=("DEVICE",),
        weight=0.8,
        criterion_component=False,
    ),
)


# ---------------------------------------------------------------------------
# Inputs & config
# ---------------------------------------------------------------------------


@dataclass(frozen=True, slots=True)
class MetricSample:
    """一条历史样本（由 MetricHistoryProvider 提供或测试注入）。"""

    ts_ms: int
    value: float
    evidence_id: str = ""
    #: baseline change-log 的成熟标记（count>=10 且 STABLE）；
    #: 非 baseline 序列默认成熟。
    mature: bool = True


@dataclass(frozen=True, slots=True)
class RiskWindowConfig:
    """所有会改变输出的参数——全部进入 model_version 指纹。"""

    horizon_hours: int = 168
    min_samples: int = 4
    min_span_hours: int = 24
    persistence_min_steps: int = 2
    min_velocity_norm_per_hour: float = 0.001
    min_monotonic_fraction: float = 0.6
    high_norm_threshold: float = 0.5
    ref_velocity_norm_per_hour: float = 1.0 / 72.0
    window_bucket_edges_hours: tuple[float, ...] = (6.0, 24.0, 72.0, 168.0)
    #: 窗口起点种子的最长陈旧期（相对预测时刻）
    seed_staleness_hours: int = 336

    def fingerprint(self) -> str:
        payload = json.dumps(asdict(self), sort_keys=True, separators=(",", ":"))
        return hashlib.sha256(payload.encode()).hexdigest()[:12]


#: predict() 的默认配置单例（B008：默认值不做函数调用）
_DEFAULT_CONFIG: RiskWindowConfig = RiskWindowConfig()


def model_version_for(
    config: RiskWindowConfig,
    contracts: Sequence[MetricFeatureContract] = FEATURE_CONTRACTS,
) -> str:
    """任何影响输出的参数变化都会改变该指纹（纪律 7）。"""
    payload = json.dumps(
        {
            "cfg": config.fingerprint(),
            "crit": [FAILURE_CRITERION_ID, FAILURE_CRITERION_VERSION],
            "features": [c.summary() for c in contracts],
        },
        sort_keys=True,
        separators=(",", ":"),
    )
    digest = hashlib.sha256(payload.encode()).hexdigest()[:12]
    return f"{BASE_RISKWINDOW_MODEL_VERSION}+sha256:{digest}"


# ---------------------------------------------------------------------------
# Trend analysis (pure math)
# ---------------------------------------------------------------------------


@dataclass(frozen=True, slots=True)
class _MetricAnalysis:
    #: 生成该分析所用的配置（frozen）——避免模块级可变全局，
    #: 使分析对象自洽、可安全跨线程复用。
    config: RiskWindowConfig
    contract: MetricFeatureContract
    samples: tuple[MetricSample, ...]
    anchor_value: float | None  # prediction_reference 锚点（absolute 域为 normal）
    latest_deviation: float
    latest_norm: float
    velocity_norm_per_hour: float
    worsening_steps: int
    nonzero_deltas: int
    span_hours: float

    @property
    def monotonic_fraction(self) -> float:
        if self.nonzero_deltas <= 0:
            return 0.0
        return self.worsening_steps / self.nonzero_deltas

    @property
    def persistent(self) -> bool:
        return (
            self.worsening_steps >= self.config.persistence_min_steps
            and self.monotonic_fraction >= self.config.min_monotonic_fraction
        )


def _sorted_samples(samples: Sequence[MetricSample]) -> tuple[MetricSample, ...]:
    return tuple(sorted(samples, key=lambda s: (s.ts_ms, s.value)))


def _lsq_slope(xs: Sequence[float], ys: Sequence[float]) -> float:
    """最小二乘斜率（确定性；单点或全同时刻 → 0）。"""
    n = len(xs)
    if n < 2:
        return 0.0
    mean_x = sum(xs) / n
    mean_y = sum(ys) / n
    denom = sum((x - mean_x) ** 2 for x in xs)
    if denom <= 0.0:
        return 0.0
    numer = sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys, strict=True))
    return numer / denom


def _analyze(
    contract: MetricFeatureContract,
    samples: Sequence[MetricSample],
    config: RiskWindowConfig,
) -> _MetricAnalysis | str:
    """返回分析结果，或返回不满足资格的原因字符串（→ inactive / missing）。"""
    ordered = _sorted_samples(samples)
    if len(ordered) < config.min_samples:
        return f"{contract.name}: 有效样本不足 ({len(ordered)} < {config.min_samples})"
    t0 = ordered[0].ts_ms
    t_last = ordered[-1].ts_ms
    span_hours = (t_last - t0) / 3_600_000.0
    if span_hours < config.min_span_hours:
        return (
            f"{contract.name}: 历史跨度不足 "
            f"({span_hours:.1f}h < {config.min_span_hours}h)"
        )

    # --- 锚点与 deviation 序列 ---
    if contract.baseline_domain == "prediction_reference":
        # 资格锚点：首个成熟样本（Phase 2 STABLE 且 count 达标）。
        anchor_idx = next((i for i, s in enumerate(ordered) if s.mature), None)
        if anchor_idx is None:
            return f"{contract.name}: 无成熟 baseline 锚点（未完成 LEARNING）"
        anchor = ordered[anchor_idx]
        trajectory = ordered[anchor_idx:]
        anchor_value = float(anchor.value)
        deviations = [anchor_value - float(s.value) for s in trajectory]  # lower worse
    else:
        normal = contract.normal_value
        if normal is None:
            return f"{contract.name}: absolute 域缺少 normal_value"
        trajectory = ordered
        anchor_value = float(normal)
        if contract.worsening == "higher_is_worse":
            deviations = [float(s.value) - anchor_value for s in trajectory]
        else:
            deviations = [anchor_value - float(s.value) for s in trajectory]

    norms = [d / contract.failure_threshold for d in deviations]
    hours = [(s.ts_ms - trajectory[0].ts_ms) / 3_600_000.0 for s in trajectory]
    velocity = _lsq_slope(hours, norms)

    worsening_steps = 0
    nonzero_deltas = 0
    for prev, cur in zip(norms[:-1], norms[1:], strict=True):
        delta = cur - prev
        if abs(delta) < 1e-12:
            continue
        nonzero_deltas += 1
        if delta > 0:
            worsening_steps += 1

    return _MetricAnalysis(
        config=config,
        contract=contract,
        samples=tuple(trajectory),
        anchor_value=anchor_value,
        latest_deviation=deviations[-1],
        latest_norm=norms[-1],
        velocity_norm_per_hour=velocity,
        worsening_steps=worsening_steps,
        nonzero_deltas=nonzero_deltas,
        span_hours=(trajectory[-1].ts_ms - trajectory[0].ts_ms) / 3_600_000.0,
    )


# ---------------------------------------------------------------------------
# Window estimation (FailureCriterion-driven, B6)
# ---------------------------------------------------------------------------


@dataclass(frozen=True, slots=True)
class _WindowResult:
    status: WindowEstimationStatus
    earliest_hours: int | None
    latest_hours: int | None
    unavailable_reason: str = ""


def _bucket(crossing_hours: float, edges: tuple[float, ...]) -> tuple[int, int]:
    """crossing → 分桶 (earliest, latest]：46h → (24, 72]。"""
    prev = 0.0
    for edge in edges:
        if crossing_hours <= edge:
            return int(prev), int(edge)
        prev = edge
    # 超出最大边（调用方已拦截）
    return int(prev), int(edges[-1])


def _estimate_window(
    analyses: Mapping[str, _MetricAnalysis | str],
    config: RiskWindowConfig,
) -> _WindowResult:
    """按 failure_criterion 的必需组件分别外推 crossing，AND → max。"""
    crossings: list[float] = []
    for contract in FEATURE_CONTRACTS:
        if not contract.criterion_component:
            continue
        analysis = analyses.get(contract.name)
        if not isinstance(analysis, _MetricAnalysis):
            reason = analysis if isinstance(analysis, str) else f"{contract.name}: 无历史"
            return _WindowResult(
                WindowEstimationStatus.UNAVAILABLE, None, None,
                unavailable_reason=f"组件 {contract.name} 不可估: {reason}",
            )
        if analysis.latest_norm >= 1.0:
            crossings.append(0.0)  # 判据组件当前已满足
            continue
        slope = analysis.velocity_norm_per_hour
        if slope < config.min_velocity_norm_per_hour or slope <= 0:
            return _WindowResult(
                WindowEstimationStatus.UNAVAILABLE, None, None,
                unavailable_reason=f"组件 {contract.name} 退化速度不可靠外推 "
                                   f"(velocity={slope:.6f}/h)",
            )
        if analysis.monotonic_fraction < config.min_monotonic_fraction:
            return _WindowResult(
                WindowEstimationStatus.UNAVAILABLE, None, None,
                unavailable_reason=f"组件 {contract.name} 轨迹非单调 "
                                   f"(fraction={analysis.monotonic_fraction:.2f})",
            )
        crossings.append((1.0 - analysis.latest_norm) / slope)

    if not crossings:
        return _WindowResult(
            WindowEstimationStatus.UNAVAILABLE, None, None,
            unavailable_reason="failure criterion 无可用组件",
        )

    # AND 判据：全部组件都满足才算失效 → 取最晚 crossing
    crossing = max(crossings)
    if crossing > config.horizon_hours:
        return _WindowResult(
            WindowEstimationStatus.UNAVAILABLE, None, None,
            unavailable_reason=f"criterion crossing {crossing:.1f}h 超出预测视野 "
                               f"{config.horizon_hours}h",
        )
    earliest, latest = _bucket(max(0.0, crossing), config.window_bucket_edges_hours)
    return _WindowResult(WindowEstimationStatus.AVAILABLE, earliest, latest)


# ---------------------------------------------------------------------------
# Prediction entry points
# ---------------------------------------------------------------------------


def _insufficient(
    subject_id: str,
    missing: list[str],
    available: list[str],
    clock: Callable[[], datetime],
    config: RiskWindowConfig,
) -> InsufficientPrediction:
    return InsufficientPrediction(
        subject_id=subject_id,
        missing_requirements=missing,
        available_evidence_ids=available,
        model_version=model_version_for(config),
        generated_at=clock(),
    )


def predict(
    subject_type: SubjectType,
    subject_id: str,
    histories: Mapping[str, Sequence[MetricSample]],
    *,
    clock: Callable[[], datetime] = lambda: datetime.now(UTC),
    config: RiskWindowConfig = _DEFAULT_CONFIG,
    contracts: Sequence[MetricFeatureContract] = FEATURE_CONTRACTS,
) -> PredictionResult:
    """L3 预测总入口。SITE 在 v1 显式不支持（契约保留枚举但不冒充实现）。"""
    if subject_type is not SubjectType.DEVICE:
        raise UnsupportedSubjectType(subject_type)

    return _predict_device(
        subject_id, histories, clock=clock, config=config, contracts=contracts
    )


def _predict_device(
    subject_id: str,
    histories: Mapping[str, Sequence[MetricSample]],
    *,
    clock: Callable[[], datetime],
    config: RiskWindowConfig,
    contracts: Sequence[MetricFeatureContract],
) -> PredictionResult:
    analyses: dict[str, _MetricAnalysis | str] = {}
    missing: list[str] = []
    available_evidence: list[str] = []
    for contract in contracts:
        samples = histories.get(contract.name, ())
        result = _analyze(contract, samples, config)
        analyses[contract.name] = result
        if isinstance(result, _MetricAnalysis):
            for sample in result.samples:
                if sample.evidence_id:
                    available_evidence.append(sample.evidence_id)
        else:
            missing.append(result)

    if not any(isinstance(v, _MetricAnalysis) for v in analyses.values()):
        # 三态之一：没有任何可分析的指标 → 系统没有产生风险判断
        if not missing:
            missing = ["所有声明指标均无历史数据"]
        return _insufficient(subject_id, missing, available_evidence, clock, config)

    active = {k: v for k, v in analyses.items() if isinstance(v, _MetricAnalysis)}
    data_sufficiency = (
        DataSufficiency.SUFFICIENT if len(active) == len(contracts) else DataSufficiency.PARTIAL
    )

    # --- 趋势判定（每个 active 指标）---
    deteriorating: list[_MetricAnalysis] = []
    persistent_analyses: list[_MetricAnalysis] = []
    for analysis in active.values():
        trending = (
            analysis.velocity_norm_per_hour >= config.min_velocity_norm_per_hour
            and analysis.latest_norm > 0.05
        )
        if trending and analysis.persistent:
            persistent_analyses.append(analysis)
            deteriorating.append(analysis)

    max_persistent_norm = max(
        (a.latest_norm for a in persistent_analyses), default=0.0
    )

    # --- 风险分级（与置信度独立）---
    if persistent_analyses and (
        len(deteriorating) >= 2 or max_persistent_norm >= config.high_norm_threshold
    ):
        risk = RiskLevel.HIGH
    elif deteriorating:
        risk = RiskLevel.MEDIUM
    else:
        risk = RiskLevel.LOW

    # --- 置信度（独立表；PARTIAL 永远到不了 HIGH/MEDIUM）---
    span_ok = all(a.span_hours >= config.horizon_hours / 2 for a in active.values())
    if data_sufficiency is DataSufficiency.SUFFICIENT and len(deteriorating) >= 2 and span_ok:
        confidence = RiskLevel.HIGH
    elif data_sufficiency is DataSufficiency.SUFFICIENT and len(deteriorating) >= 1:
        confidence = RiskLevel.MEDIUM
    else:
        confidence = RiskLevel.LOW

    # --- 驱动贡献度（仅 active 指标进入分母；缺失 ≠ 0）---
    drivers: list[RiskDriver] = []
    raw_by_metric: dict[str, float] = {}
    for name, analysis in active.items():
        trend_strength = min(
            1.0, max(0.0, analysis.velocity_norm_per_hour) / config.ref_velocity_norm_per_hour
        )
        raw = analysis.contract.weight * max(0.0, analysis.latest_norm) * trend_strength
        raw_by_metric[name] = raw
    total_raw = sum(raw_by_metric.values())
    if total_raw > 0:
        for name, analysis in active.items():
            raw = raw_by_metric[name]
            if raw <= 0:
                continue
            direction = (
                TrendDirection.DETERIORATING
                if analysis.velocity_norm_per_hour >= config.min_velocity_norm_per_hour
                else TrendDirection.STABLE
            )
            evidence_ids: list[str] = []
            for sample in analysis.samples:
                if sample.evidence_id and sample.evidence_id not in evidence_ids:
                    evidence_ids.append(sample.evidence_id)
            drivers.append(
                RiskDriver(
                    metric=name,
                    contribution_pct=round(raw / total_raw * 100.0, 1),
                    direction=direction,
                    evidence_ids=evidence_ids,
                )
            )
        drivers.sort(key=lambda d: (-d.contribution_pct, d.metric))

    # --- 趋势证据 ---
    trend_evidence: list[TrendEvidence] = []
    for analysis in active.values():
        if analysis.latest_norm <= 0:
            continue
        name = analysis.contract.name
        evidence_ids: list[str] = []
        for sample in analysis.samples:
            if sample.evidence_id and sample.evidence_id not in evidence_ids:
                evidence_ids.append(sample.evidence_id)
        if analysis.contract.baseline_domain == "prediction_reference":
            statement = (
                f"baseline 自锚点 {analysis.anchor_value:.0f}dBm 漂移 "
                f"{analysis.latest_deviation:.1f}dB（{analysis.span_hours:.0f}h，"
                f"样本 {len(analysis.samples)}）"
            )
        else:
            statement = (
                f"{name} 偏离正常值 {analysis.latest_deviation:.4g}"
                f"（{analysis.span_hours:.0f}h，样本 {len(analysis.samples)}）"
            )
        trend_evidence.append(
            TrendEvidence(metric=name, statement=statement, evidence_ids=evidence_ids)
        )

    # --- 窗口估计（FailureCriterion 驱动）---
    window = _estimate_window(analyses, config)

    return RiskWindow(
        subject_type=SubjectType.DEVICE,
        subject_id=subject_id,
        risk_level=risk,
        window_estimation_status=window.status,
        window_earliest_hours=window.earliest_hours,
        window_latest_hours=window.latest_hours,
        forecast_horizon_hours=config.horizon_hours,
        prediction_confidence=confidence,
        data_sufficiency=data_sufficiency,
        drivers=drivers,
        trend_evidence=trend_evidence,
        failure_criterion_id=FAILURE_CRITERION_ID,
        failure_criterion_version=FAILURE_CRITERION_VERSION,
        model_version=model_version_for(config, contracts),
        generated_at=clock(),
    )

"""Contracts and data models for wireless causal diagnosis (Phase 4b).

Strictly enforces separation between:
1. CanonicalDiagnosis: Physical facts & determined hypotheses (ground truth)
2. DiagnosisPresentation: Human-oriented report, citations, and suggestions (presentation only)
"""

from __future__ import annotations

from datetime import datetime
from enum import StrEnum
from typing import Any, Literal, Self

from pydantic import BaseModel, ConfigDict, Field, model_validator


class _ClosedModel(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)


class ObservedPattern(StrEnum):
    """物理层真实发生的事实特征（确定性识别，不含猜测）"""

    SUB_SECOND_SIMULTANEOUS_DISCONNECT = "SUB_SECOND_SIMULTANEOUS_DISCONNECT"  # 亚秒级多设备同断
    MULTI_DEVICE_CONCURRENT_ANOMALY = "MULTI_DEVICE_CONCURRENT_ANOMALY"  # 窗口内多设备协同断连/劣化
    SINGLE_DEVICE_GRADUAL_DEGRADATION = "SINGLE_DEVICE_GRADUAL_DEGRADATION"  # 单设备持续劣化后断开
    SINGLE_DEVICE_ABRUPT_LOSS = "SINGLE_DEVICE_ABRUPT_LOSS"  # 单设备良好信号下突发失联
    EXPLICIT_TERMINATION_PATTERN = "EXPLICIT_TERMINATION_PATTERN"  # 纯事实命名：全为显式主动终止
    SPARSE_UNCLASSIFIED_EVENTS = "SPARSE_UNCLASSIFIED_EVENTS"  # 零星孤立事件


class DiagnosisHypothesis(StrEnum):
    """受约束的推断解释（面向运维的可解释根因）"""

    LOCAL_ADAPTER_OR_HOST_STALL = (
        "LOCAL_ADAPTER_OR_HOST_STALL"  # 本机网关适配器/总线假死 (max=MEDIUM)
    )
    COEXISTENCE_RF_INTERFERENCE = (
        "COEXISTENCE_RF_INTERFERENCE"  # 2.4GHz Wi-Fi 同频竞争干扰 (max=HIGH)
    )
    AREA_RF_DEGRADATION_OR_OBSTACLE = (
        "AREA_RF_DEGRADATION_OR_OBSTACLE"  # 区域无线遮挡/环境噪声 (max=HIGH)
    )
    DEVICE_DISTANCE_OR_SHADOWING = (
        "DEVICE_DISTANCE_OR_SHADOWING"  # 单设备距离超限或移动遮挡 (max=MEDIUM)
    )
    DEVICE_POWER_LOSS_OR_CRASH = (
        "DEVICE_POWER_LOSS_OR_CRASH"  # 单设备掉电、重启或固件崩溃 (max=LOW)
    )
    NORMAL_USER_ACTIVITY = "NORMAL_USER_ACTIVITY"  # 正常下班关机/主动操作 (max=HIGH)
    INSUFFICIENT_EVIDENCE = "INSUFFICIENT_EVIDENCE"  # 证据不足，拒绝胡猜


class ConfidenceLevel(StrEnum):
    """离散证据强度等级（杜绝虚假浮点精确度）"""

    HIGH = "HIGH"  # 多源证据一致支撑，零主要反证
    MEDIUM = "MEDIUM"  # 核心模式成立，但缺少环境交叉支撑
    LOW = "LOW"  # 仅单一路径特征弱支持
    INSUFFICIENT = "INSUFFICIENT"  # 证据残缺，无法形成可靠解释


#: 各假说的证据置信度上限门槛（置信度上限规则）
HYPOTHESIS_MAX_CONFIDENCE: dict[DiagnosisHypothesis, ConfidenceLevel] = {
    DiagnosisHypothesis.COEXISTENCE_RF_INTERFERENCE: ConfidenceLevel.HIGH,  # 蓝牙+Wi-Fi双事实源支撑
    # 排除共存后的多设备协同衰退
    DiagnosisHypothesis.AREA_RF_DEGRADATION_OR_OBSTACLE: ConfidenceLevel.HIGH,
    # 无 host/controller 独立证据前，封顶 MEDIUM
    DiagnosisHypothesis.LOCAL_ADAPTER_OR_HOST_STALL: ConfidenceLevel.MEDIUM,
    DiagnosisHypothesis.NORMAL_USER_ACTIVITY: ConfidenceLevel.HIGH,  # 显式主动断开
    # 仅靠无线单源无法区分姿态/距离
    DiagnosisHypothesis.DEVICE_DISTANCE_OR_SHADOWING: ConfidenceLevel.MEDIUM,
    # 现有无线指标无法区分掉电/崩掉/移出，上限封顶 LOW
    DiagnosisHypothesis.DEVICE_POWER_LOSS_OR_CRASH: ConfidenceLevel.LOW,
    DiagnosisHypothesis.INSUFFICIENT_EVIDENCE: ConfidenceLevel.INSUFFICIENT,
}


class DeviceFinding(_ClosedModel):
    """单设备在事故中的表现子结论"""

    device_address: str
    observed_pattern: ObservedPattern
    hypothesis: DiagnosisHypothesis
    confidence: ConfidenceLevel
    note: str = ""


class CanonicalDiagnosis(_ClosedModel):
    """【真值】系统确立的物理事实与推断假说，下游系统只能读取此处的字段"""

    observed_pattern: ObservedPattern
    hypothesis: DiagnosisHypothesis
    confidence: ConfidenceLevel
    evidence_ids: list[str] = Field(default_factory=list)
    deterministic_reasons: list[str] = Field(default_factory=list)
    device_findings: list[DeviceFinding] = Field(default_factory=list)


class StructuredFinding(_ClosedModel):
    """【W6约束】结构化事实断言，必须绑定 Evidence ID 集合"""

    text: str
    evidence_ids: list[str] = Field(min_length=1)


class EvidenceCitation(_ClosedModel):
    """证据引述节点"""

    step: str
    claim: str
    evidence_refs: list[str] = Field(default_factory=list)


class DiagnosisPresentation(_ClosedModel):
    """【呈现】面向运维人员阅读的自然语言阐述与操作建议，绝非系统认定的真值"""

    llm_model_name: str | None = None
    llm_used: bool
    diagnosis_report: str
    structured_findings: list[StructuredFinding] = Field(default_factory=list)
    evidence_citations: list[EvidenceCitation] = Field(default_factory=list)
    recommendations: list[str] = Field(default_factory=list)


class WirelessDiagnosisResponse(_ClosedModel):
    """对外返回的完整诊断视图"""

    diagnosis_id: str
    incident_id: str
    diagnosis_version: str
    rules_version: str
    prompt_version: str
    guardrail_version: str
    created_at: datetime
    canonical: CanonicalDiagnosis
    presentation: DiagnosisPresentation
    guardrail_status: str
    guardrail_findings: list[dict[str, Any]] = Field(default_factory=list)


# ---------------------------------------------------------------------------
# 证据包与内部视图模型（用于解耦 DB ORM 和 规则计算）
# ---------------------------------------------------------------------------


class WirelessEventView(_ClosedModel):
    event_id: str
    ts_ms: int
    site_id: str
    gateway_id: str
    protocol: str
    device_address: str
    address_type: str
    hci_index: int
    event_type: str
    rssi_at_event_dbm: int | None = None
    raw_reason_code: int = 0
    reason: str
    source: str
    source_detail: str = ""
    details_json: str = ""


class BaselineView(_ClosedModel):
    # 身份字段带默认值：兼容 4b 期只按 device_address 构造的既有测试；
    # 上行链（Phase 4a）必须填全，云端据此落复合主键，杜绝跨网关/跨控制器合并。
    site_id: str = ""
    gateway_id: str = ""
    hci_index: int = 0
    protocol: str = "BLUETOOTH"
    address_type: str = "UNKNOWN"
    device_address: str
    baseline_rssi_dbm: int | None = None
    min_seen_rssi_dbm: int | None = None
    max_seen_rssi_dbm: int | None = None
    baseline_sample_count: int = 0
    state: str = "LEARNING"


class IncidentView(_ClosedModel):
    incident_id: str
    site_id: str
    gateway_id: str
    started_at_ms: int
    last_event_ms: int
    resolved_at_ms: int | None = None
    affected_devices: int
    state: str
    suspected_cause: str | None = None
    #: 构成该事故的 device_events.event_id 清板端 site_incident_events 回链；
    #: 默认空列表 = 旧载荷/未知，装载时回退到时间窗选事件。
    evidence_event_ids: list[str] = Field(default_factory=list)


class EnvironmentWindowView(_ClosedModel):
    available: bool = False
    link_type: str = "UNKNOWN"
    wifi_anomaly: bool = False
    coexistence_warning: bool = False
    #: 窗口覆盖的事件时间范围（板端上行用；4b 期构造的既有测试缺省为 0）。
    from_ms: int = 0
    to_ms: int = 0
    snapshots: list[dict[str, Any]] = Field(default_factory=list)


class IncidentEvidenceBundle(_ClosedModel):
    """送入因果规则引擎的完整证据包（不可变、纯数据）"""

    incident: IncidentView
    qualifying_events: list[WirelessEventView]
    window_events: list[WirelessEventView]
    baselines: dict[str, BaselineView]
    environment: EnvironmentWindowView


# ---------------------------------------------------------------------------
# L3 Prediction：PredictionResult 联合类型
#
# 三态语义（契约 C）：
#   风险无法判断            → InsufficientPrediction（系统没有产生风险判断）
#   风险可判断但时间不可估   → RiskWindow(window_estimation_status=UNAVAILABLE)
#   风险与时间均可估         → RiskWindow(window_estimation_status=AVAILABLE)
#
# "证据不足"不是第四种风险等级——risk_level 只有 LOW/MEDIUM/HIGH。
# ---------------------------------------------------------------------------


class SubjectType(StrEnum):
    """预测主体类型。v1 只实现 DEVICE；SITE 保留枚举但显式不支持。"""

    DEVICE = "DEVICE"
    SITE = "SITE"


class RiskLevel(StrEnum):
    LOW = "LOW"
    MEDIUM = "MEDIUM"
    HIGH = "HIGH"


class WindowEstimationStatus(StrEnum):
    """窗口是否可估。UNAVAILABLE 时 earliest/latest 为 null，但风险判断仍成立。"""

    AVAILABLE = "AVAILABLE"
    UNAVAILABLE = "UNAVAILABLE"


class DataSufficiency(StrEnum):
    INSUFFICIENT = "INSUFFICIENT"
    PARTIAL = "PARTIAL"
    SUFFICIENT = "SUFFICIENT"


class TrendDirection(StrEnum):
    DETERIORATING = "DETERIORATING"
    STABLE = "STABLE"


class RiskDriver(_ClosedModel):
    """单个风险驱动因子。

    ``contribution_pct`` 的产品文案一律是"风险驱动贡献度"，**不是"原因占比"**：
    它只表示该指标对预测模型输出的贡献，不表示现实故障的因果配比。
    """

    metric: str
    contribution_pct: float = Field(ge=0.0, le=100.0)
    direction: TrendDirection
    evidence_ids: list[str] = Field(default_factory=list)


class TrendEvidence(_ClosedModel):
    """事实性趋势证据（如"基线在 72h 内自 -60 漂移到 -64"）。"""

    metric: str
    statement: str
    evidence_ids: list[str] = Field(default_factory=list)


class RiskWindow(_ClosedModel):
    """风险可判断时产出。窗口可估与否由 window_estimation_status 表达。"""

    subject_type: SubjectType = SubjectType.DEVICE
    subject_id: str

    risk_level: RiskLevel

    window_estimation_status: WindowEstimationStatus
    window_earliest_hours: int | None = Field(default=None, ge=0)
    window_latest_hours: int | None = Field(default=None, ge=0)
    forecast_horizon_hours: int = Field(default=168, gt=0)

    prediction_confidence: RiskLevel
    data_sufficiency: DataSufficiency

    drivers: list[RiskDriver] = Field(default_factory=list)
    trend_evidence: list[TrendEvidence] = Field(default_factory=list)

    failure_criterion_id: str
    failure_criterion_version: str
    model_version: str
    generated_at: datetime


class InsufficientPrediction(_ClosedModel):
    """风险无法判断时产出——系统没有产生风险判断，而不是"风险未知"。"""

    subject_type: SubjectType = SubjectType.DEVICE
    subject_id: str

    data_sufficiency: DataSufficiency = DataSufficiency.INSUFFICIENT
    missing_requirements: list[str] = Field(default_factory=list)
    available_evidence_ids: list[str] = Field(default_factory=list)

    model_version: str
    generated_at: datetime


PredictionResult = RiskWindow | InsufficientPrediction


class UnsupportedSubjectType(RuntimeError):
    """v1 只支持 DEVICE 级预测；SITE 级需要独立的聚合与失效语义。"""

    def __init__(self, subject_type: SubjectType) -> None:
        self.subject_type = subject_type
        super().__init__(f"prediction for subject_type={subject_type} is not supported in v1")


# ---------------------------------------------------------------------------
# Phase 4a：板端 → 云端无线事实上行契约（network.edge.wireless-events.v1）
# ---------------------------------------------------------------------------

WIRELESS_EVENT_UPLINK_SCHEMA_VERSION = "network.edge.wireless-events.v1"

#: 单批上限：事件是稀疏事实（断连/劣化），正常一批远小于此值；
#: 上限只为给签名报文一个确定的体积边界（服务端另有 1MB 原始体上限）。
MAX_UPLINK_EVENTS_PER_BATCH = 1000
MAX_UPLINK_INCIDENTS_PER_BATCH = 200
MAX_UPLINK_BASELINES_PER_BATCH = 2000


class WirelessEventUplinkBatch(_ClosedModel):
    """签名信封：单网关一次批量上行的无线事实（事件/事故/基线/环境窗口）。

    与遥测信封 ``network.edge.telemetry.v1`` 分域（决策 D1）：
    这里的幂等键是 ``event_id`` / ``incident_id`` / 复合基线键，
    不走 ``(network_epoch, sequence_id)``——两者失败重试与演化语义不同
    （不可变事实 vs 单调快照），混进一个信封会把两种幂等规则耦合死。
    ``watermark_ms`` 只是板端游标的回显，服务端不依赖它做去重。
    """

    schema_version: Literal["network.edge.wireless-events.v1"] = (
        WIRELESS_EVENT_UPLINK_SCHEMA_VERSION
    )
    device_id: str = Field(min_length=1, max_length=128)
    watermark_ms: int = Field(ge=0)
    events: list[WirelessEventView] = Field(default_factory=list)
    incidents: list[IncidentView] = Field(default_factory=list)
    baselines: list[BaselineView] = Field(default_factory=list)
    env_window: EnvironmentWindowView | None = None
    #: 网关自述的动作目录指纹（见 docs/网关动作目录版本契约.md）。可选：
    #: 未上报保持 ``None``，Policy 回落既有"无漂移事实"语义——缺数据不等于漂移，
    #: 若把缺失当漂移会让所有旧固件与排练环境被全线阻断。
    catalog_version: str | None = Field(default=None, min_length=1, max_length=64)

    @model_validator(mode="after")
    def _enforce_batch_limits(self) -> Self:
        """给签名报文一个确定的体积边界，并保证载荷不空。

        依赖已装 pydantic v2 的 model_validator，形态与 contracts.py
        既有的 ``_require_single_device_and_ascending_sequence`` 一致。
        """
        if not (self.events or self.incidents or self.baselines or self.env_window):
            raise ValueError("an uplink batch must carry at least one group")
        if len(self.events) > MAX_UPLINK_EVENTS_PER_BATCH:
            raise ValueError("too many events in one signed batch")
        if len(self.incidents) > MAX_UPLINK_INCIDENTS_PER_BATCH:
            raise ValueError("too many incidents in one signed batch")
        if len(self.baselines) > MAX_UPLINK_BASELINES_PER_BATCH:
            raise ValueError("too many baselines in one signed batch")
        return self


class WirelessGroupCounts(_ClosedModel):
    """一组事实的接受/重复计数（重放安全的可观测出口）。"""

    events: int = 0
    incidents: int = 0
    baselines: int = 0


class WirelessEventIngestResult(_ClosedModel):
    accepted: WirelessGroupCounts
    duplicates: WirelessGroupCounts


def wireless_report_schema() -> dict[str, Any]:
    """返回供 ModelGateway 调用的 JSON Schema 严格契约"""
    return {
        "type": "object",
        "properties": {
            "diagnosis_report": {"type": "string"},
            "structured_findings": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "text": {"type": "string"},
                        "evidence_ids": {
                            "type": "array",
                            "items": {"type": "string"},
                            "minItems": 1,
                        },
                    },
                    "required": ["text", "evidence_ids"],
                    "additionalProperties": False,
                },
            },
            "evidence_citations": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "step": {"type": "string"},
                        "claim": {"type": "string"},
                        "evidence_refs": {"type": "array", "items": {"type": "string"}},
                    },
                    "required": ["step", "claim", "evidence_refs"],
                    "additionalProperties": False,
                },
            },
            "recommendations": {"type": "array", "items": {"type": "string"}},
        },
        "required": [
            "diagnosis_report",
            "structured_findings",
            "evidence_citations",
            "recommendations",
        ],
        "additionalProperties": False,
    }

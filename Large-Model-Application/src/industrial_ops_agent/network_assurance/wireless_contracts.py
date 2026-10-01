"""Contracts and data models for wireless causal diagnosis (Phase 4b).

Strictly enforces separation between:
1. CanonicalDiagnosis: Physical facts & determined hypotheses (ground truth)
2. DiagnosisPresentation: Human-oriented report, citations, and suggestions (presentation only)
"""

from __future__ import annotations

from datetime import datetime
from enum import StrEnum
from typing import Any
from pydantic import BaseModel, ConfigDict, Field


class _ClosedModel(BaseModel):
    model_config = ConfigDict(extra="forbid", frozen=True)


class ObservedPattern(StrEnum):
    """物理层真实发生的事实特征（确定性识别，不含猜测）"""
    SUB_SECOND_SIMULTANEOUS_DISCONNECT = "SUB_SECOND_SIMULTANEOUS_DISCONNECT"  # 亚秒级多设备同断
    MULTI_DEVICE_CONCURRENT_ANOMALY    = "MULTI_DEVICE_CONCURRENT_ANOMALY"     # 窗口内多设备协同断连/劣化
    SINGLE_DEVICE_GRADUAL_DEGRADATION  = "SINGLE_DEVICE_GRADUAL_DEGRADATION"   # 单设备持续劣化后断开
    SINGLE_DEVICE_ABRUPT_LOSS          = "SINGLE_DEVICE_ABRUPT_LOSS"           # 单设备良好信号下突发失联
    EXPLICIT_TERMINATION_PATTERN       = "EXPLICIT_TERMINATION_PATTERN"        # 纯事实命名：全为显式主动终止
    SPARSE_UNCLASSIFIED_EVENTS         = "SPARSE_UNCLASSIFIED_EVENTS"          # 零星孤立事件


class DiagnosisHypothesis(StrEnum):
    """受约束的推断解释（面向运维的可解释根因）"""
    LOCAL_ADAPTER_OR_HOST_STALL        = "LOCAL_ADAPTER_OR_HOST_STALL"         # 本机网关适配器/总线假死 (max=MEDIUM)
    COEXISTENCE_RF_INTERFERENCE        = "COEXISTENCE_RF_INTERFERENCE"         # 2.4GHz Wi-Fi 同频竞争干扰 (max=HIGH)
    AREA_RF_DEGRADATION_OR_OBSTACLE    = "AREA_RF_DEGRADATION_OR_OBSTACLE"     # 区域无线遮挡/环境噪声 (max=HIGH)
    DEVICE_DISTANCE_OR_SHADOWING       = "DEVICE_DISTANCE_OR_SHADOWING"        # 单设备距离超限或移动遮挡 (max=MEDIUM)
    DEVICE_POWER_LOSS_OR_CRASH         = "DEVICE_POWER_LOSS_OR_CRASH"          # 单设备掉电、重启或固件崩溃 (max=LOW)
    NORMAL_USER_ACTIVITY               = "NORMAL_USER_ACTIVITY"                # 正常下班关机/主动操作 (max=HIGH)
    INSUFFICIENT_EVIDENCE              = "INSUFFICIENT_EVIDENCE"               # 证据不足，拒绝胡猜


class ConfidenceLevel(StrEnum):
    """离散证据强度等级（杜绝虚假浮点精确度）"""
    HIGH         = "HIGH"         # 多源证据一致支撑，零主要反证
    MEDIUM       = "MEDIUM"       # 核心模式成立，但缺少环境交叉支撑
    LOW          = "LOW"          # 仅单一路径特征弱支持
    INSUFFICIENT = "INSUFFICIENT" # 证据残缺，无法形成可靠解释


#: 各假说的证据置信度上限门槛（置信度上限规则）
HYPOTHESIS_MAX_CONFIDENCE: dict[DiagnosisHypothesis, ConfidenceLevel] = {
    DiagnosisHypothesis.COEXISTENCE_RF_INTERFERENCE: ConfidenceLevel.HIGH,     # 蓝牙+Wi-Fi双事实源支撑
    DiagnosisHypothesis.AREA_RF_DEGRADATION_OR_OBSTACLE: ConfidenceLevel.HIGH, # 排除共存后的多设备协同衰退
    DiagnosisHypothesis.LOCAL_ADAPTER_OR_HOST_STALL: ConfidenceLevel.MEDIUM,   # 无host/controller独立证据前，封顶 MEDIUM
    DiagnosisHypothesis.NORMAL_USER_ACTIVITY: ConfidenceLevel.HIGH,            # 显式主动断开
    DiagnosisHypothesis.DEVICE_DISTANCE_OR_SHADOWING: ConfidenceLevel.MEDIUM,  # 仅靠无线单源无法区分姿态/距离
    DiagnosisHypothesis.DEVICE_POWER_LOSS_OR_CRASH: ConfidenceLevel.LOW,       # 现有无线指标无法区分掉电/崩掉/移出，上限封顶 LOW
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


class EnvironmentWindowView(_ClosedModel):
    available: bool = False
    link_type: str = "UNKNOWN"
    wifi_anomaly: bool = False
    coexistence_warning: bool = False
    snapshots: list[dict[str, Any]] = Field(default_factory=list)


class IncidentEvidenceBundle(_ClosedModel):
    """送入因果规则引擎的完整证据包（不可变、纯数据）"""
    incident: IncidentView
    qualifying_events: list[WirelessEventView]
    window_events: list[WirelessEventView]
    baselines: dict[str, BaselineView]
    environment: EnvironmentWindowView


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
                            "minItems": 1
                        }
                    },
                    "required": ["text", "evidence_ids"],
                    "additionalProperties": False
                }
            },
            "evidence_citations": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "step": {"type": "string"},
                        "claim": {"type": "string"},
                        "evidence_refs": {
                            "type": "array",
                            "items": {"type": "string"}
                        }
                    },
                    "required": ["step", "claim", "evidence_refs"],
                    "additionalProperties": False
                }
            },
            "recommendations": {
                "type": "array",
                "items": {"type": "string"}
            }
        },
        "required": [
            "diagnosis_report",
            "structured_findings",
            "evidence_citations",
            "recommendations"
        ],
        "additionalProperties": False
    }

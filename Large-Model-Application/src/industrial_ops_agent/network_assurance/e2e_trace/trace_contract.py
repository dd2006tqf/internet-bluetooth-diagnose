"""⑥ Trace contract —— 端到端审计链的报告格式（verification/audit support）。

**这不是产品领域模型**，而是路线图 ⑥ 的测试产物契约：场景 runner 执行
真实 service 路径并捕获 TraceReport；trace_verifier 读 TraceReport + DB
产出 `TRACE OK / BREAK / PARTIAL`；reporter CLI 打印审计树。

三个不相交的集合（不可混淆）：

- ``nodes``           —— **真实存在、可指认**的对象（Incident/Diagnosis/...）；
- ``edge_checks``     —— 关系断言（"该存在的边存在"、"不该存在的边不存在"）；
- ``operation_checks``—— 动作断言（"尝试执行后系统实际做了什么"）。

``prediction_snapshots`` 单列：L3 预测不落库，只有运行时捕获的 snapshot
才能让 trace 完整；reporter 的 --incident-id 模式因拿不到它只能输出
``TRACE PARTIAL``，绝不假装完整。
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import asdict, dataclass, field
from datetime import UTC, datetime
from typing import Any, Literal
from uuid import uuid4

TRACE_CONTRACT_VERSION = "e2e-trace.v1"

NodeKind = Literal[
    "INCIDENT",
    "DIAGNOSIS",
    "PREDICTION",
    "COUNCIL",
    "PROPOSAL",
    "ACTION_DECISION",
    "APPROVAL",
    "APPROVAL_DECISION",
    "PENDING_ACTION",
    "ACTION_OUTCOME",
    "MANUAL_RUNBOOK",
]

Expectation = Literal["PRESENT", "ABSENT"]
Actual = Literal["PRESENT", "ABSENT"]
OperationResult = Literal["ACCEPTED", "REJECTED"]
TraceResult = Literal["OK", "BREAK", "PARTIAL"]

Scenario = Literal[
    "REMOTE_HAPPY",
    "MANUAL_RUNBOOK",
    "POLICY_BLOCKED",
    "APPROVAL_INVALID",
    "ISOLATION",
]


@dataclass(frozen=True, slots=True)
class TraceNode:
    """一个**真实存在**的链上对象。不存在的对象不允许建节点（见 edge_checks）。"""

    kind: NodeKind
    id: str
    digest_or_version: str | None = None
    status: str | None = None
    ts: str | None = None
    extra: dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        return {k: v for k, v in asdict(self).items() if v not in (None, {}, [])}


@dataclass(frozen=True, slots=True)
class TraceEdgeCheck:
    """关系不变量：from → to 该存在还是该不存在，以及实际是否如此。"""

    from_kind: NodeKind
    to_kind: NodeKind
    expectation: Expectation
    actual: Actual
    reason: str = ""

    @property
    def ok(self) -> bool:
        return self.expectation == self.actual

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(frozen=True, slots=True)
class TraceOperationCheck:
    """动作不变量：对系统发起一次操作，期望/实际结果。"""

    operation: str  # "decide" | "execute" | "runbook" | "list_proposals" | ...
    subject_id: str  # 操作对象（proposal_id / approval_id / ...）
    expectation: OperationResult
    actual: OperationResult
    error_code: str | None = None
    detail: str = ""

    @property
    def ok(self) -> bool:
        return self.expectation == self.actual

    def to_dict(self) -> dict[str, Any]:
        return {k: v for k, v in asdict(self).items() if v not in (None, "")}


@dataclass(frozen=True, slots=True)
class TraceReport:
    """一次场景运行的完整审计快照（可 JSON 序列化）。"""

    scenario: Scenario
    incident_id: str
    nodes: list[TraceNode]
    edge_checks: list[TraceEdgeCheck]
    operation_checks: list[TraceOperationCheck]
    prediction_snapshots: list[dict[str, Any]] = field(default_factory=list)
    trace_contract_version: str = TRACE_CONTRACT_VERSION
    trace_run_id: str = field(default_factory=lambda: f"trace-{uuid4().hex[:16]}")
    started_at: str = field(default_factory=lambda: datetime.now(UTC).isoformat())
    completed_at: str | None = None

    def to_dict(self) -> dict[str, Any]:
        return {
            "trace_contract_version": self.trace_contract_version,
            "trace_run_id": self.trace_run_id,
            "scenario": self.scenario,
            "incident_id": self.incident_id,
            "started_at": self.started_at,
            "completed_at": self.completed_at,
            "nodes": [n.to_dict() for n in self.nodes],
            "edge_checks": [e.to_dict() for e in self.edge_checks],
            "operation_checks": [o.to_dict() for o in self.operation_checks],
            "prediction_snapshots": self.prediction_snapshots,
        }

    def to_json(self) -> str:
        return json.dumps(self.to_dict(), ensure_ascii=False, indent=2, sort_keys=True)

    @classmethod
    def from_json(cls, raw: str) -> TraceReport:
        doc = json.loads(raw)
        return cls(
            trace_contract_version=doc["trace_contract_version"],
            trace_run_id=doc["trace_run_id"],
            scenario=doc["scenario"],
            incident_id=doc["incident_id"],
            started_at=doc["started_at"],
            completed_at=doc.get("completed_at"),
            nodes=[TraceNode(**n) for n in doc.get("nodes", [])],
            edge_checks=[TraceEdgeCheck(**e) for e in doc.get("edge_checks", [])],
            operation_checks=[
                TraceOperationCheck(**o) for o in doc.get("operation_checks", [])
            ],
            prediction_snapshots=doc.get("prediction_snapshots", []),
        )

    def digest(self) -> str:
        return "sha256:" + hashlib.sha256(self.to_json().encode()).hexdigest()


def node(report: TraceReport, kind: NodeKind) -> TraceNode | None:
    """按 kind 取第一个节点（多同 kind 场景用 extra 区分后再查）。"""
    for n in report.nodes:
        if n.kind == kind:
            return n
    return None


def edge(report: TraceReport, from_kind: NodeKind, to_kind: NodeKind) -> TraceEdgeCheck | None:
    for e in report.edge_checks:
        if e.from_kind == from_kind and e.to_kind == to_kind:
            return e
    return None

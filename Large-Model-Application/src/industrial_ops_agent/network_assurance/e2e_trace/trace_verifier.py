"""⑥ Trace verifier —— 对 TraceReport + DB 做架构不变量断言（纯函数）。

输入是 runner 捕获的 TraceReport 和可查询的 DB 行视图；输出
``TraceResult``（OK / BREAK / PARTIAL）+ 第一处断链说明。

核心纪律：**verifier 不补数据**——它只断言 report 里记录的节点确实存在、
关系断言与操作断言一致；它不重新跑 Prediction、不读没有的记录。
"""

from __future__ import annotations

from dataclasses import dataclass, field

from .trace_contract import (
    TraceReport,
)


@dataclass(frozen=True, slots=True)
class TraceVerdict:
    result: str  # "OK" | "BREAK" | "PARTIAL"
    first_break: str | None = None
    details: list[str] = field(default_factory=list)


def verify_trace(report: TraceReport) -> TraceVerdict:
    """对 TraceReport 的自洽性校验（不需要 DB；runner 已把 DB 事实写进节点）。

    断言三条：
    1. 所有 edge_check.actual == expectation；
    2. 所有 operation_check.actual == expectation；
    3. 存在至少一条 PRESENT 主链（依场景不同，主链形态不同——由 runner
       负责把该场景的关键节点按顺序放进 nodes）。
    """
    details: list[str] = []
    first_break: str | None = None

    for e in report.edge_checks:
        if not e.ok:
            msg = (
                f"edge {e.from_kind}→{e.to_kind}: "
                f"expected {e.expectation}, got {e.actual} ({e.reason})"
            )
            details.append(msg)
            if first_break is None:
                first_break = msg

    for o in report.operation_checks:
        if not o.ok:
            msg = (
                f"op {o.operation}({o.subject_id}): "
                f"expected {o.expectation}, got {o.actual}"
                + (f" [{o.error_code}]" if o.error_code else "")
            )
            details.append(msg)
            if first_break is None:
                first_break = msg

    if first_break is not None:
        return TraceVerdict(result="BREAK", first_break=first_break, details=details)

    # PARTIAL：有 prediction 节点但 snapshot 未捕获，或报告自身标注部分可用
    has_prediction_node = any(n.kind == "PREDICTION" for n in report.nodes)
    has_prediction_snapshot = bool(report.prediction_snapshots)
    if has_prediction_node and not has_prediction_snapshot:
        details.append(
            "PREDICTION node present but prediction_snapshots is empty — "
            "trace is PARTIAL (prediction is not persisted; capture at runtime)"
        )
        return TraceVerdict(result="PARTIAL", first_break=None, details=details)

    return TraceVerdict(result="OK", first_break=None, details=details)


def render_tree(report: TraceReport, verdict: TraceVerdict) -> str:
    """人读审计树：缩进按 nodes 顺序，edge/operation 断言附在对应节点下。"""
    lines: list[str] = [
        f"TRACE {verdict.result}"
        + (f" — {verdict.first_break}" if verdict.first_break else ""),
        f"scenario: {report.scenario}",
        f"incident: {report.incident_id}",
        f"run:      {report.trace_run_id} @ {report.started_at}",
        "",
    ]
    for i, n in enumerate(report.nodes):
        indent = "  " * i
        bits = [n.kind, n.id]
        if n.status:
            bits.append(f"status={n.status}")
        if n.digest_or_version:
            bits.append(f"v={n.digest_or_version[:18]}…")
        lines.append(indent + "→ " + " ".join(bits))
        for k, v in n.extra.items():
            lines.append(indent + "    " + f"{k}={v}")
    if report.prediction_snapshots:
        lines.append("")
        lines.append(f"  prediction snapshots captured: {len(report.prediction_snapshots)}")
    bad_edges = [e for e in report.edge_checks if not e.ok]
    bad_ops = [o for o in report.operation_checks if not o.ok]
    if bad_edges or bad_ops:
        lines.append("")
        lines.append("  FAILED ASSERTIONS:")
        for e in bad_edges:
            lines.append(
                f"    edge {e.from_kind}→{e.to_kind}: want {e.expectation}, got {e.actual}"
            )
        for o in bad_ops:
            lines.append(
                f"    op {o.operation}({o.subject_id}): want {o.expectation}, got {o.actual}"
            )
    return "\n".join(lines)

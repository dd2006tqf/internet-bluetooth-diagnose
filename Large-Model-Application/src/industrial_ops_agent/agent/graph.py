"""Structured LangGraph diagnosis that cannot grant permissions or mutate business state."""

from __future__ import annotations

from typing import Any, TypedDict

from langgraph.graph import END, START, StateGraph

from industrial_ops_agent.agent.models import AgentBudget, DiagnosisReport
from industrial_ops_agent.knowledge.models import RetrievedEvidence


class _GraphState(TypedDict):
    query: str
    evidence: list[dict[str, Any]]
    max_steps: int
    steps: int
    report: dict[str, Any] | None


class DiagnosisGraph:
    VERSION = "diagnosis-graph-v1"

    def __init__(self) -> None:
        graph = StateGraph(_GraphState)
        graph.add_node("assess_evidence", _assess_evidence)
        graph.add_edge(START, "assess_evidence")
        graph.add_edge("assess_evidence", END)
        self._compiled = graph.compile()

    def run(
        self,
        *,
        query: str,
        evidence: list[RetrievedEvidence],
        budget: AgentBudget,
    ) -> tuple[DiagnosisReport, dict[str, Any]]:
        if budget.max_steps < 1:
            report = DiagnosisReport(
                status="NEEDS_INFORMATION",
                conclusion=None,
                citations=(),
                contradictions=(),
                missing_information=("Agent step budget is exhausted.",),
                next_checks=("Request an expert-owned diagnostic run.",),
                confidence=0.0,
                stop_reason="step_budget_exhausted",
            )
            return report, {"steps": 0, "stop_reason": report.stop_reason}
        state = self._compiled.invoke(
            {
                "query": query,
                "evidence": [_evidence_state(item) for item in evidence],
                "max_steps": budget.max_steps,
                "steps": 0,
                "report": None,
            }
        )
        report_data = state["report"]
        if report_data is None:
            raise RuntimeError("diagnosis graph completed without a report")
        report = DiagnosisReport(
            status=str(report_data["status"]),
            conclusion=report_data["conclusion"],
            citations=tuple(report_data["citations"]),
            contradictions=tuple(report_data["contradictions"]),
            missing_information=tuple(report_data["missing_information"]),
            next_checks=tuple(report_data["next_checks"]),
            confidence=float(report_data["confidence"]),
            stop_reason=str(report_data["stop_reason"]),
        )
        return report, dict(state)


def _assess_evidence(state: _GraphState) -> dict[str, Any]:
    evidence = state["evidence"]
    if not evidence:
        report = DiagnosisReport(
            status="NEEDS_INFORMATION",
            conclusion=None,
            citations=(),
            contradictions=(),
            missing_information=(
                "No authorized, active knowledge applies to this equipment model and symptom.",
            ),
            next_checks=("Confirm the equipment model and upload current alarm evidence.",),
            confidence=0.0,
            stop_reason="insufficient_authorized_evidence",
        )
    else:
        top = evidence[0]
        report = DiagnosisReport(
            status="COMPLETED",
            conclusion=str(top["content"]),
            citations=tuple(item["citation"] for item in evidence[:3]),
            contradictions=(),
            missing_information=(),
            next_checks=(
                "Inspect the inlet filter and verify differential pressure before repair.",
            ),
            confidence=min(0.92, 0.68 + 0.06 * len(evidence)),
            stop_reason="evidence_sufficient",
        )
    return {"steps": state["steps"] + 1, "report": report.as_dict()}


def _evidence_state(evidence: RetrievedEvidence) -> dict[str, Any]:
    return {
        "content": evidence.content,
        "citation": evidence.citation_payload(),
    }

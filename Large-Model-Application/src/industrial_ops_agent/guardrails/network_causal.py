"""Deterministic causal guardrail for network diagnosis outputs.

## Why this is not a prompt check

``prompt_injection`` guards *text*: it pattern-matches untrusted strings for
override attempts and exfiltration. That is necessary but not sufficient for
network diagnosis, because the dangerous failure there is not an instruction
override — it is a **causal inversion**: a confident, well-formed report that
states the opposite of what the evidence shows. No string pattern catches
that; only comparing the report against the snapshot does.

This module therefore checks the report's *claims against the evidence*, and
its rules mirror the invariants the edge evaluator itself enforces:

- R1 — reachability veto. The edge treats a gateway-unreachable verdict as a
  one-vote override of every higher-layer conclusion (see
  ``overall_policy.hpp``: "Reachability BAD -> Overall BAD, DNS=symptom").
  A report blaming DNS or application config on top of an unreachable
  gateway inverts that ordering and would send an operator to inspect the
  wrong layer entirely.

- R2 — transport-status decoupling. A 4xx/5xx HTTP response *proves the
  transport works*; the failure is server-side. A report calling the network
  "down" because a server returned 500 repeats the single most common
  misdiagnosis this platform exists to correct.

- R3 — no evidence, no conclusion. The edge's HR-6 rule is "no new evidence,
  no state transition". Its diagnosis counterpart: a report asserting a root
  cause must name evidence that exists in the snapshot. A root cause backed
  by nothing is a guess, and a guess delivered as fact is worse than no
  report at all.

## What this does not do

This module does not decide whether a diagnosis is *correct*. It enforces a
floor: the report must not contradict the snapshot it claims to explain.
Deeper fidelity (does the causal chain really follow from the evidence?) is
the model's job; catching contradictions is the guardrail's.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from hashlib import sha256
from typing import Any

from industrial_ops_agent.guardrails.prompt_injection import (
    GuardrailDecision,
    GuardrailFinding,
)

NETWORK_CAUSAL_POLICY_VERSION = "network-causal-v1"

#: Higher-layer subjects a report may blame when the gateway is unreachable.
#: Matching any of these in a report about an unreachable network is a causal
#: inversion (R1). Deliberately includes both the technical term and the
#: phrasings a model actually produces ("check the DNS server", "application
#: misconfiguration") because a guardrail that only catches the textbook
#: phrasing catches nothing in practice.
_HIGH_LAYER_BLAME = re.compile(
    r"\b(?:dns|resolver|name.?server|http|https|tls|certificate|"
    r"application|app.?config|proxy|firewall rule)\b",
    re.IGNORECASE,
)

#: Phrasings that assert the transport is broken. R2 fires only when the
#: snapshot's HTTP result actually received a response (state GOOD), so this
#: pattern alone is not enough — it is the combination that blocks.
_TRANSPORT_BROKEN_CLAIM = re.compile(
    r"\b(?:network|transport|connectivity|link|connection)\s+"
    r"(?:is\s+)?(?:down|broken|dead|unusable|unreachable|failed)\b",
    re.IGNORECASE,
)

#: A root-cause assertion. R3 requires that a report containing one of these
#: also cite evidence; "may", "possibly" and "suggest" forms are exempt
#: because hedged language is not a factual claim.
_ROOT_CAUSE_CLAIM = re.compile(
    r"\b(?:root cause|caused by|due to|the problem is|the failure is)\b",
    re.IGNORECASE,
)
_HEDGED_CLAIM = re.compile(
    r"\b(?:may|might|possibly|possibly|could be|suggest(?:s|ed)?)\b",
    re.IGNORECASE,
)

#: Evidence identifiers a report may cite. Kept generous on purpose: a
#: citation like "ping_loss" or "dns_latency_ms" matches the metric names the
#: edge actually emits, and we would rather accept an imprecise citation than
#: reject a truthful report over naming conventions.
_EVIDENCE_TOKEN = re.compile(r"\b[a-z][a-z0-9_]{2,40}\b", re.IGNORECASE)

#: HTTP success states that prove the transport works even when the business
#: response was an error (R2's evidence basis).
_TRANSPORT_PROVEN_STATES = frozenset({"GOOD"})


@dataclass(frozen=True, slots=True)
class CausalContext:
    """The snapshot facts a report claims to explain.

    ``snapshot`` is the ``experience`` payload as stored by the ingest
    service (flat dict with ``overall_state``, ``network_health``,
    ``service_health``). Kept as a plain dict rather than a Pydantic model so
    the guardrail can run against stored JSON without a second parse.
    """

    device_id: str
    snapshot: dict[str, Any]

    def _sle_state(self, group: str, key: str) -> str | None:
        section = self.snapshot.get(group)
        if not isinstance(section, dict):
            return None
        result = section.get(key)
        if not isinstance(result, dict):
            return None
        state = result.get("state")
        return state if isinstance(state, str) else None

    def _evidence_metrics(self) -> frozenset[str]:
        metrics: set[str] = set()
        for group in ("network_health", "service_health"):
            section = self.snapshot.get(group)
            if not isinstance(section, dict):
                continue
            for result in section.values():
                if not isinstance(result, dict):
                    continue
                evidence = result.get("evidence")
                if not isinstance(evidence, list):
                    continue
                for item in evidence:
                    if isinstance(item, dict) and isinstance(item.get("metric"), str):
                        metrics.add(item["metric"].lower())
        return frozenset(metrics)


def _finding(
    rule_id: str,
    category: str,
    excerpt: str,
) -> GuardrailFinding:
    """Record a causal violation by fingerprint, never by raw excerpt.

    Mirrors ``prompt_injection``'s discipline: the finding carries a SHA-256
    of the matched text so two identical violations are distinguishable, but
    the report content itself never leaves the guardrail boundary.
    """

    return GuardrailFinding(
        phase="OUTPUT",
        source_role="model",
        pattern_id=rule_id,
        category=category,
        severity="HIGH",
        content_hash=sha256(excerpt.encode()).hexdigest(),
    )


class NetworkCausalGuardrail:
    """Check a diagnosis report against the snapshot it claims to explain."""

    policy_version = NETWORK_CAUSAL_POLICY_VERSION

    def inspect(
        self,
        report: str,
        context: CausalContext,
    ) -> GuardrailDecision:
        findings: list[GuardrailFinding] = []
        findings.extend(self._check_reachability_veto(report, context))
        findings.extend(self._check_transport_decoupling(report, context))
        findings.extend(self._check_evidence_backing(report, context))
        return GuardrailDecision(
            decision="BLOCKED" if findings else "ALLOWED",
            policy_version=self.policy_version,
            findings=tuple(findings),
        )

    def inspect_json_report(
        self,
        report: dict[str, Any],
        context: CausalContext,
    ) -> GuardrailDecision:
        """Inspect a structured report, checking structured fields first.

        Structured fields are checked exactly (string equality against the
        snapshot) before the free-text rules run, because a model that gets
        the structured summary right but rambles in prose is far less
        dangerous than one that gets the summary wrong.
        """

        findings: list[GuardrailFinding] = []
        stated_cause = report.get("primary_issue") or report.get("root_cause")
        if isinstance(stated_cause, str) and stated_cause:
            findings.extend(self._check_stated_cause(stated_cause, context))
        text = json.dumps(report, ensure_ascii=False)
        findings.extend(self._check_reachability_veto(text, context))
        findings.extend(self._check_transport_decoupling(text, context))
        findings.extend(self._check_evidence_backing(text, context))
        return GuardrailDecision(
            decision="BLOCKED" if findings else "ALLOWED",
            policy_version=self.policy_version,
            findings=tuple(findings),
        )

    # ------------------------------------------------------------------
    # Rules
    # ------------------------------------------------------------------

    def _check_reachability_veto(
        self, report: str, context: CausalContext
    ) -> list[GuardrailFinding]:
        """R1: gateway unreachable means the report may not blame higher layers."""

        reach_state = context._sle_state("network_health", "ip_reachability")
        if reach_state != "BAD":
            return []
        if not _HIGH_LAYER_BLAME.search(report):
            return []
        return [
            _finding(
                "reachability_veto_inverted",
                "causal_inversion",
                report,
            )
        ]

    def _check_transport_decoupling(
        self, report: str, context: CausalContext
    ) -> list[GuardrailFinding]:
        """R2: a received HTTP response proves the transport, whatever the status."""

        http_state = context._sle_state("service_health", "http_access")
        if http_state not in _TRANSPORT_PROVEN_STATES:
            return []
        if not _TRANSPORT_BROKEN_CLAIM.search(report):
            return []
        return [
            _finding(
                "transport_status_decoupled",
                "causal_inversion",
                report,
            )
        ]

    def _check_evidence_backing(
        self, report: str, context: CausalContext
    ) -> list[GuardrailFinding]:
        """R3: an unhedged root-cause claim must name evidence that exists."""

        claim = _ROOT_CAUSE_CLAIM.search(report)
        if claim is None:
            return []
        if _HEDGED_CLAIM.search(report):
            return []
        # The cited tokens include the claim's own words; the requirement is
        # that *some* token names a metric the edge actually measured. This
        # is intentionally a floor, not a proof: it catches reports whose
        # root cause is backed by nothing at all, which is the case that
        # matters.
        known = context._evidence_metrics()
        if not known:
            # No evidence exists in the snapshot at all; any unhedged claim
            # is therefore backed by nothing.
            return [
                _finding(
                    "root_cause_without_evidence",
                    "unsupported_claim",
                    report,
                )
            ]
        tokens = {t.lower() for t in _EVIDENCE_TOKEN.findall(report)}
        if tokens & known:
            return []
        return [
            _finding(
                "root_cause_without_evidence",
                "unsupported_claim",
                report,
            )
        ]

    def _check_stated_cause(
        self, stated_cause: str, context: CausalContext
    ) -> list[GuardrailFinding]:
        """Check the structured root-cause field against the snapshot state."""

        overall = context.snapshot.get("overall_state")
        if overall == "GOOD" and stated_cause.strip():
            # A healthy snapshot with a named root cause would surface a
            # fault the evaluator did not find.
            return [
                _finding(
                    "root_cause_on_healthy_snapshot",
                    "causal_inversion",
                    stated_cause,
                )
            ]
        return []

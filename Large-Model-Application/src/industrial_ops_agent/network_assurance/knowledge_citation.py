"""Retrieve prior cases as *background* for an LLM explanation.

Why this is deliberately narrow
-------------------------------
The diagnosis pipeline has a hard boundary: the canonical conclusion comes
from deterministic rules over this incident's own evidence, and the model may
only explain it. Retrieval therefore feeds the model's *context*, never its
*authority*:

  - retrieved text lands in a separate ``knowledge_context`` payload key,
    never in ``evidence_catalog`` — so ``valid_evidence_ids`` stays exactly the
    incident's own events, and the W6 guardrail still rejects any attempt to
    cite a knowledge document as evidence for this incident;
  - no identity → no retrieval (the GET path and every existing caller keep
    their zero-side-effect behaviour);
  - any failure — no published release, ACL mismatch, backend error — returns
    an empty list. A diagnosis must never fail because the knowledge base was
    unreachable.

Relevance comes from the platform's own retrieval: sparse tokens plus a
deterministic embedding, rank-fused, ACL-filtered by tenant, subject and role,
restricted to the currently active published release.
"""

from __future__ import annotations

import logging
from dataclasses import dataclass
from datetime import UTC, datetime
from typing import Any

from sqlalchemy import select

from industrial_ops_agent.auth.identity import IdentityContext
from industrial_ops_agent.knowledge.models import RetrievalQuery
from industrial_ops_agent.knowledge.retrieval import HybridRetriever
from industrial_ops_agent.network_assurance.wireless_contracts import CanonicalDiagnosis
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import IndexReleaseRecord
from industrial_ops_agent.persistence.tenant import TenantContext

logger = logging.getLogger("industrial_ops_agent.network_assurance.knowledge_citation")

#: At most this many references are injected — the prompt is explanation
#: context, not a literature review.
MAX_REFERENCE_ITEMS = 3

#: Per-reference content bound, so one oversized case cannot dominate the
#: model's context window.
MAX_REFERENCE_CHARS = 1_000

#: An active release is per-name; several may exist (one per knowledge domain).
#: Bounding the scan keeps one diagnosis from walking the whole release table.
_MAX_ACTIVE_RELEASES = 5

#: The ``model_code`` the asset bridge assigns to gateways. Case documents are
#: published unscoped (empty ``device_models``) so they match any query; using
#: this sentinel instead of a pump model keeps *other* scoped documents out of
#: a wireless diagnosis.
GATEWAY_DEVICE_MODEL = "weaknet-gateway"


@dataclass(frozen=True, slots=True)
class ReferenceKnowledge:
    """One retrieved prior case, ready for the model payload."""

    title: str
    content: str
    document_id: str

    def as_payload(self) -> dict[str, str]:
        return {"title": self.title, "content": self.content, "document_id": self.document_id}


def _query_text(canonical: CanonicalDiagnosis) -> str:
    """The deterministic retrieval query.

    Built from the enum *values* (they appear verbatim inside case documents),
    so a hit means "we have seen this pattern before" rather than a fuzzy
    semantic guess the guardrail could not audit.
    """

    return f"{canonical.observed_pattern} {canonical.hypothesis}"


def retrieve_reference_knowledge(
    database: Database,
    identity: IdentityContext,
    canonical: CanonicalDiagnosis,
    *,
    limit: int = MAX_REFERENCE_ITEMS,
) -> list[ReferenceKnowledge]:
    """Best-effort retrieval of prior cases. Never raises."""

    context: TenantContext = identity.tenant_context
    try:
        with database.transaction(context) as session:
            release_ids = list(
                session.scalars(
                    select(IndexReleaseRecord.release_id)
                    .where(
                        IndexReleaseRecord.tenant_id == identity.tenant_id,
                        IndexReleaseRecord.status == "PUBLISHED",
                        IndexReleaseRecord.is_active.is_(True),
                    )
                    .limit(_MAX_ACTIVE_RELEASES)
                )
            )
        if not release_ids:
            return []

        roles = frozenset(role.value for role in identity.roles)
        query_text = _query_text(canonical)
        retriever = HybridRetriever(database)
        now = datetime.now(UTC)

        merged: dict[str, Any] = {}
        for release_id in release_ids:
            result = retriever.search(
                RetrievalQuery(
                    tenant_id=identity.tenant_id,
                    subject_id=identity.subject_id,
                    roles=roles,
                    release_id=release_id,
                    device_family=None,
                    device_model=GATEWAY_DEVICE_MODEL,
                    query=query_text,
                    as_of=now,
                    limit=limit,
                )
            )
            for item in result:
                existing = merged.get(item.chunk_id)
                if existing is None or item.fused_score > existing.fused_score:
                    merged[item.chunk_id] = item

        ordered = sorted(
            merged.values(), key=lambda item: (-item.fused_score, item.chunk_id)
        )[:limit]
        return [
            ReferenceKnowledge(
                title=item.title,
                content=item.content[:MAX_REFERENCE_CHARS],
                document_id=item.document_id,
            )
            for item in ordered
        ]
    except Exception as exc:  # noqa: BLE001 - 检索是增强，绝不阻断诊断
        logger.warning("knowledge retrieval skipped: %s", exc, exc_info=True)
        return []

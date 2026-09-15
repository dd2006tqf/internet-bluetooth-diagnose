"""Append-only Agent event store used by workers and the SSE transport."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import UTC, datetime
from typing import Any
from uuid import uuid4

from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import AgentEventRecord
from industrial_ops_agent.persistence.repositories import AgentEventRepository, AgentRunRepository
from industrial_ops_agent.persistence.tenant import TenantContext


class AgentRunNotFound(Exception):
    """The run is absent from the trusted tenant context."""


class AgentRunWriteStopped(Exception):
    """A cancellation request prevents new Agent nodes from publishing events."""


@dataclass(frozen=True, slots=True)
class AgentEvent:
    event_id: str
    agent_run_id: str
    sequence: int
    event_type: str
    payload: dict[str, Any]
    visibility: str
    occurred_at: datetime


class AgentEventStore:
    def __init__(self, database: Database) -> None:
        self._database = database

    def append(
        self,
        context: TenantContext,
        agent_run_id: str,
        *,
        event_type: str,
        payload: dict[str, Any],
        visibility: str = "authorized",
        agent_status: str | None = None,
    ) -> AgentEvent:
        now = datetime.now(UTC)
        with self._database.transaction(context) as session:
            runs = AgentRunRepository(session, context)
            run = runs.get(agent_run_id)
            if run is None:
                raise AgentRunNotFound
            if run.status in {"CANCEL_REQUESTED", "CANCELLED"}:
                raise AgentRunWriteStopped
            sequence = run.last_sequence + 1
            record = AgentEventRecord(
                event_id=f"agent-event-{uuid4().hex}",
                tenant_id=context.tenant_id,
                agent_run_id=agent_run_id,
                sequence=sequence,
                event_type=event_type,
                payload=payload,
                visibility=visibility,
                occurred_at=now,
                created_at=now,
                updated_at=now,
            )
            AgentEventRepository(session, context).add(record)
            run.last_sequence = sequence
            run.version += 1
            run.updated_at = now
            if agent_status is not None:
                run.status = agent_status
            return _event(record)

    def list_after(
        self,
        context: TenantContext,
        agent_run_id: str,
        sequence: int,
    ) -> tuple[list[AgentEvent], str]:
        with self._database.transaction(context) as session:
            run = AgentRunRepository(session, context).get(agent_run_id)
            if run is None:
                raise AgentRunNotFound
            records = AgentEventRepository(session, context).list_after(agent_run_id, sequence)
            return [_event(record) for record in records], run.status


def _event(record: AgentEventRecord) -> AgentEvent:
    occurred_at = record.occurred_at
    if occurred_at.tzinfo is None:
        occurred_at = occurred_at.replace(tzinfo=UTC)
    return AgentEvent(
        event_id=record.event_id,
        agent_run_id=record.agent_run_id,
        sequence=record.sequence,
        event_type=record.event_type,
        payload=record.payload,
        visibility=record.visibility,
        occurred_at=occurred_at,
    )

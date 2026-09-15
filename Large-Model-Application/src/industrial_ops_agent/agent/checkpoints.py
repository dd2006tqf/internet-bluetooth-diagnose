"""Persistent graph checkpoints independent from Temporal workflow history."""

from __future__ import annotations

import json
from datetime import UTC, datetime
from hashlib import sha256
from typing import Any
from uuid import uuid4

from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import AgentCheckpointRecord
from industrial_ops_agent.persistence.repositories import AgentCheckpointRepository
from industrial_ops_agent.persistence.tenant import TenantContext


class AgentCheckpointStore:
    def __init__(self, database: Database) -> None:
        self._database = database

    def save(
        self,
        context: TenantContext,
        agent_run_id: str,
        *,
        sequence: int,
        graph_version: str,
        state: dict[str, Any],
    ) -> None:
        canonical = json.dumps(state, ensure_ascii=False, separators=(",", ":"), sort_keys=True)
        now = datetime.now(UTC)
        with self._database.transaction(context) as session:
            AgentCheckpointRepository(session, context).add(
                AgentCheckpointRecord(
                    checkpoint_id=f"checkpoint-{uuid4().hex}",
                    tenant_id=context.tenant_id,
                    agent_run_id=agent_run_id,
                    sequence=sequence,
                    graph_version=graph_version,
                    state=state,
                    state_checksum=sha256(canonical.encode()).hexdigest(),
                    created_at=now,
                    updated_at=now,
                )
            )

    def latest(self, context: TenantContext, agent_run_id: str) -> dict[str, Any] | None:
        with self._database.transaction(context) as session:
            record = AgentCheckpointRepository(session, context).latest(agent_run_id)
            return None if record is None else record.state

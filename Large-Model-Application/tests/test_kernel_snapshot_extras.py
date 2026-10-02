"""Deep kernel snapshots (process top-N, skb drop attribution) reach the copilot
as *appendix* evidence, never as a verdict input.

The edge emits these inside ``env_window.snapshots[]`` (an untyped JSON array).
That looseness is why the contract verifier now pins the accepted ``kind``
values: a renamed kind on the C++ side would otherwise be silently ingested and
silently ignored.

What these tests pin:

  1. recognised kinds are extracted; unknown kinds are dropped, not passed through;
  2. the copilot appendix renders process/drop details when present and is
     omitted entirely when absent (no fabricated "kernel looks fine");
  3. the appendix is text only — it never enters the causal chain or the
     guardrail context, so "conclusions come from SLEs" still holds.
"""

from __future__ import annotations

from datetime import UTC, datetime
from typing import Any

import pytest
from sqlalchemy import create_engine
from sqlalchemy.orm import Session
from sqlalchemy.pool import StaticPool

from industrial_ops_agent.network_assurance.service import NetworkAssuranceService
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import (
    Base,
    NetworkEnvWindowRecord,
    TenantRecord,
)
from industrial_ops_agent.persistence.tenant import TenantContext

TENANT = "tenant-alpha"
ASSET = "radxa-cubie-a7a"


@pytest.fixture
def test_db() -> Database:
    engine = create_engine(
        "sqlite:///:memory:",
        connect_args={"check_same_thread": False},
        poolclass=StaticPool,
    )
    Base.metadata.create_all(engine)
    db = Database.from_engine(engine)
    with Session(db.engine) as session:  # type: ignore[attr-defined]
        session.add(
            TenantRecord(
                id=TENANT,
                status="active",
                display_name="kernel snapshot tenant",
                version=1,
                created_at=datetime.now(UTC),
                updated_at=datetime.now(UTC),
            )
        )
        session.commit()
    return db


@pytest.fixture
def tenant_context() -> TenantContext:
    return TenantContext(tenant_id=TENANT, subject_id="operator-1")


def _window(
    db: Database, snapshots: list[dict[str, Any]], *, to_ms: int = 1_700_000_010_000
) -> None:
    with Session(db.engine) as session:  # type: ignore[attr-defined]
        session.add(
            NetworkEnvWindowRecord(
                window_id=f"nenv-{to_ms}",
                tenant_id=TENANT,
                asset_id=ASSET,
                from_ms=0,
                to_ms=to_ms,
                available=True,
                link_type="WIFI_2_4G",
                wifi_anomaly=False,
                coexistence_warning=False,
                snapshots_json=snapshots,
            )
        )
        session.commit()


PROCESS_SNAPSHOT = {
    "kind": "process_top",
    "ts_ms": 1_700_000_010_000,
    "top_processes": [
        {
            "pid": 1234,
            "comm": "iperf3",
            "tx_bytes": 80_000_000,
            "tx_packets": 60_000,
            "retrans_count": 42,
        },
    ],
}

DROP_SNAPSHOT = {
    "kind": "skb_drop_hist",
    "ts_ms": 1_700_000_010_000,
    "total_drops": 1024,
    "top_reasons": [
        {
            "reason_code": 6,
            "reason_name": "NETFILTER_DROP",
            "protocol": "TCP/IPv4",
            "count": 1024,
            "description": "被 netfilter 规则丢弃",
            "last_timestamp_ns": 1,
        },
    ],
}


def test_extracts_recognised_kinds(test_db: Database, tenant_context: TenantContext):
    _window(test_db, [PROCESS_SNAPSHOT, DROP_SNAPSHOT])
    extras = NetworkAssuranceService(test_db).get_kernel_snapshot_extras(tenant_context, ASSET)

    assert extras["process_top"][0]["comm"] == "iperf3"
    assert extras["process_top"][0]["retrans_count"] == 42
    assert extras["skb_drop_hist"]["total_drops"] == 1024
    assert extras["skb_drop_hist"]["top_reasons"][0]["reason_name"] == "NETFILTER_DROP"


def test_unknown_kind_is_dropped_not_passed_through(
    test_db: Database, tenant_context: TenantContext
):
    """板端写错 kind 时忽略；透传会让前端拿到无法解释的结构。"""

    _window(test_db, [{"kind": "process_xyz", "top_processes": [{"pid": 1}]}])
    extras = NetworkAssuranceService(test_db).get_kernel_snapshot_extras(tenant_context, ASSET)
    assert extras == {}


def test_no_window_and_empty_snapshots_yield_empty(
    test_db: Database, tenant_context: TenantContext
):
    service = NetworkAssuranceService(test_db)
    assert service.get_kernel_snapshot_extras(tenant_context, ASSET) == {}

    _window(test_db, [])
    assert service.get_kernel_snapshot_extras(tenant_context, ASSET) == {}


def test_latest_window_wins(test_db: Database, tenant_context: TenantContext):
    _window(test_db, [PROCESS_SNAPSHOT], to_ms=1_700_000_010_000)
    _window(
        test_db,
        [{"kind": "process_top", "top_processes": [{"pid": 9, "comm": "newer"}]}],
        to_ms=1_700_000_020_000,
    )

    extras = NetworkAssuranceService(test_db).get_kernel_snapshot_extras(tenant_context, ASSET)
    assert extras["process_top"][0]["comm"] == "newer"


# ---------------------------------------------------------------------------
# Copilot 呈现层
# ---------------------------------------------------------------------------


class _StubAssurance:
    def __init__(self, extras: dict[str, Any]) -> None:
        self._extras = extras

    def get_kernel_snapshot_extras(self, context: TenantContext, asset_id: str) -> dict:
        return self._extras


def _copilot(extras: dict[str, Any]):
    from industrial_ops_agent.network_assurance.copilot import NetworkCopilotService

    return NetworkCopilotService(None, _StubAssurance(extras))  # type: ignore[arg-type]


def test_kernel_observation_renders_process_and_drop(tenant_context: TenantContext):
    copilot = _copilot(
        {
            "process_top": PROCESS_SNAPSHOT["top_processes"],
            "skb_drop_hist": {
                "total_drops": 1024,
                "top_reasons": DROP_SNAPSHOT["top_reasons"],
            },
        }
    )
    text = copilot.kernel_observation(tenant_context, ASSET)
    assert text is not None
    assert "iperf3" in text and "pid=1234" in text and "重传 42 次" in text
    assert "NETFILTER_DROP" in text and "1024 次" in text
    # 明示这是佐证材料，不改变结论
    assert "不改变上面的结论" in text


def test_kernel_observation_absent_when_no_data(tenant_context: TenantContext):
    assert _copilot({}).kernel_observation(tenant_context, ASSET) is None

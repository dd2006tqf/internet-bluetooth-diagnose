"""Unit tests for the Phase 4a wireless-fact uplink (service level).

The edge treats replay as a normal path (its watermark only advances after
an acknowledgement), so every test below exercises both the first-ingest and
the identical-replay behaviour the board's exporter depends on:

  1. first batch registers the gateway asset and accepts every group;
  2. identical replay counts duplicates without duplicating rows;
  3. incident lifecycle advance (OPEN -> RESOLVED) counts accepted;
  4. baseline profile upsert accounting;
  5. env window refresh keeps a single row;
  6. cross-tenant device claim fails closed (409 semantics upstream).
"""

from __future__ import annotations

from datetime import UTC, datetime

import pytest
from pydantic import ValidationError
from sqlalchemy import create_engine, select
from sqlalchemy.orm import Session
from sqlalchemy.pool import StaticPool

from industrial_ops_agent.domain import as_utc
from industrial_ops_agent.network_assurance.service import (
    NetworkAssuranceConflict,
    NetworkAssuranceService,
)
from industrial_ops_agent.network_assurance.wireless_contracts import (
    BaselineView,
    EnvironmentWindowView,
    IncidentView,
    WirelessEventUplinkBatch,
    WirelessEventView,
)
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import (
    Base,
    NetworkAssetRecord,
    NetworkDeviceBaselineHistoryRecord,
    NetworkDeviceBaselineRecord,
    NetworkEnvWindowRecord,
    NetworkGatewayCatalogVersionRecord,
    NetworkSiteIncidentRecord,
    NetworkWirelessEventRecord,
)
from industrial_ops_agent.persistence.tenant import TenantContext

DEVICE = "radxa-cubie-a7a"
KEY_ID = "weaknet-edge-telemetry-v1"
#: 与 _incident() 的默认 incident_id 一致
INCIDENT = "site-1_1700000001000"


@pytest.fixture
def test_db() -> Database:
    engine = create_engine(
        "sqlite:///:memory:",
        connect_args={"check_same_thread": False},
        poolclass=StaticPool,
    )
    Base.metadata.create_all(engine)
    return Database.from_engine(engine)


@pytest.fixture
def tenant_context() -> TenantContext:
    return TenantContext(tenant_id="tenant-alpha", subject_id="edge-automation")


def _event(event_id: str = "evt-1", ts_ms: int = 1_700_000_001_000) -> WirelessEventView:
    return WirelessEventView(
        event_id=event_id,
        ts_ms=ts_ms,
        site_id="site-1",
        gateway_id="gw-1",
        protocol="BLUETOOTH",
        device_address="AA:01",
        address_type="LE_RANDOM",
        hci_index=0,
        event_type="LINK_DISCONNECTED",
        rssi_at_event_dbm=-70,
        reason="CONNECTION_TIMEOUT",
        source="KERNEL_MGMT",
    )


def _incident(
    incident_id: str = "site-1_1700000001000",
    *,
    state: str = "OPEN",
    resolved_at_ms: int | None = None,
    last_event_ms: int = 1_700_000_001_500,
) -> IncidentView:
    return IncidentView(
        incident_id=incident_id,
        site_id="site-1",
        gateway_id="gw-1",
        started_at_ms=1_700_000_001_000,
        last_event_ms=last_event_ms,
        resolved_at_ms=resolved_at_ms,
        affected_devices=2,
        state=state,
        evidence_event_ids=["evt-1"],
    )


def _baseline(
    device_address: str = "AA:01", *, count: int = 12, state: str = "STABLE"
) -> BaselineView:
    return BaselineView(
        site_id="site-1",
        gateway_id="gw-1",
        hci_index=0,
        protocol="BLUETOOTH",
        address_type="LE_RANDOM",
        device_address=device_address,
        baseline_rssi_dbm=-65,
        min_seen_rssi_dbm=-78,
        max_seen_rssi_dbm=-59,
        baseline_sample_count=count,
        state=state,
    )


def _env(*, wifi_anomaly: bool = True) -> EnvironmentWindowView:
    return EnvironmentWindowView(
        available=True,
        link_type="WIFI_2_4G",
        wifi_anomaly=wifi_anomaly,
        coexistence_warning=False,
        from_ms=1_700_000_000_000,
        to_ms=1_700_000_060_000,
        snapshots=[{"ts_ms": 1_700_000_000_000, "rssi_dbm": -66}],
    )


def _batch(**overrides) -> WirelessEventUplinkBatch:
    payload: dict[str, object] = {
        "device_id": DEVICE,
        "watermark_ms": 1_700_000_001_500,
        "events": [_event()],
        "incidents": [_incident()],
        "baselines": [_baseline()],
        "env_window": _env(),
    }
    payload.update(overrides)
    return WirelessEventUplinkBatch.model_validate(payload)


def _count_rows(db: Database, model) -> int:
    with Session(db.engine) as session:  # type: ignore[attr-defined]
        return len(session.query(model).all())


def test_first_batch_accepts_and_registers_asset(test_db: Database, tenant_context: TenantContext):
    service = NetworkAssuranceService(database=test_db)
    result = service.ingest_wireless_batch(tenant_context, _batch(), key_id=KEY_ID)

    assert result.accepted.events == 1
    assert result.accepted.incidents == 1
    assert result.accepted.baselines == 1
    assert result.duplicates.events == 0
    assert result.duplicates.incidents == 0
    assert result.duplicates.baselines == 0

    with Session(test_db.engine) as session:  # type: ignore[attr-defined]
        asset = session.get(NetworkAssetRecord, DEVICE)
        assert asset is not None
        assert asset.tenant_id == "tenant-alpha"
        assert asset.signing_key_id == KEY_ID

        event = session.get(NetworkWirelessEventRecord, "evt-1")
        assert event is not None
        assert event.asset_id == DEVICE
        assert event.rssi_at_event_dbm == -70
        assert event.raw_reason_code == 0

        incident = session.get(NetworkSiteIncidentRecord, "site-1_1700000001000")
        assert incident is not None
        assert incident.state == "OPEN"
        assert incident.evidence_event_ids_json == ["evt-1"]


def test_identical_replay_counts_duplicates_without_duplicate_rows(
    test_db: Database, tenant_context: TenantContext
):
    service = NetworkAssuranceService(database=test_db)
    service.ingest_wireless_batch(tenant_context, _batch(), key_id=KEY_ID)
    replay = service.ingest_wireless_batch(tenant_context, _batch(), key_id=KEY_ID)

    # 事件是不可变事实：重放全部计为 duplicate，行数不增
    assert replay.accepted.events == 0
    assert replay.duplicates.events == 1
    # 载荷未变化的事故与基线同样计为 duplicate
    assert replay.accepted.incidents == 0
    assert replay.duplicates.incidents == 1
    assert replay.accepted.baselines == 0
    assert replay.duplicates.baselines == 1

    assert _count_rows(test_db, NetworkWirelessEventRecord) == 1
    assert _count_rows(test_db, NetworkSiteIncidentRecord) == 1
    assert _count_rows(test_db, NetworkDeviceBaselineRecord) == 1
    assert _count_rows(test_db, NetworkEnvWindowRecord) == 1


def test_incident_lifecycle_advance_counts_accepted_and_persists_state(
    test_db: Database, tenant_context: TenantContext
):
    service = NetworkAssuranceService(database=test_db)
    service.ingest_wireless_batch(tenant_context, _batch(), key_id=KEY_ID)

    resolved = _batch(
        incidents=[_incident(state="RESOLVED", resolved_at_ms=1_700_000_061_000)],
        events=[],
        baselines=[],
        env_window=None,
    )
    advance = service.ingest_wireless_batch(tenant_context, resolved, key_id=KEY_ID)
    assert advance.accepted.incidents == 1
    assert advance.duplicates.incidents == 0

    with Session(test_db.engine) as session:  # type: ignore[attr-defined]
        row = session.get(NetworkSiteIncidentRecord, "site-1_1700000001000")
        assert row is not None
        assert row.state == "RESOLVED"
        assert row.resolved_at_ms == 1_700_000_061_000

    # 再次重放同一 RESOLVED 载荷 → 全部 duplicate
    again = service.ingest_wireless_batch(tenant_context, resolved, key_id=KEY_ID)
    assert again.accepted.incidents == 0
    assert again.duplicates.incidents == 1


def test_incident_window_never_rewinds_on_late_replay(
    test_db: Database, tenant_context: TenantContext
):
    service = NetworkAssuranceService(database=test_db)
    service.ingest_wireless_batch(tenant_context, _batch(), key_id=KEY_ID)

    late = _incident(last_event_ms=1_700_000_001_100)
    # 迟到的旧载荷：last_event_ms 早于已存窗口 —— 时间右边界不得回退
    service.ingest_wireless_batch(
        tenant_context,
        _batch(incidents=[late], events=[], baselines=[], env_window=None),
        key_id=KEY_ID,
    )

    with Session(test_db.engine) as session:  # type: ignore[attr-defined]
        row = session.get(NetworkSiteIncidentRecord, "site-1_1700000001000")
        assert row is not None
        assert row.last_event_ms == 1_700_000_001_500


def test_baseline_profile_upsert_counts_change_as_accepted(
    test_db: Database, tenant_context: TenantContext
):
    service = NetworkAssuranceService(database=test_db)
    service.ingest_wireless_batch(tenant_context, _batch(), key_id=KEY_ID)

    updated = _batch(
        baselines=[_baseline(count=40, state="DEGRADED")], events=[], incidents=[], env_window=None
    )
    result = service.ingest_wireless_batch(tenant_context, updated, key_id=KEY_ID)
    assert result.accepted.baselines == 1
    assert result.duplicates.baselines == 0

    with Session(test_db.engine) as session:  # type: ignore[attr-defined]
        rows = session.query(NetworkDeviceBaselineRecord).all()
        assert len(rows) == 1
        assert rows[0].baseline_sample_count == 40
        assert rows[0].state == "DEGRADED"


def test_env_window_refresh_keeps_single_row(test_db: Database, tenant_context: TenantContext):
    service = NetworkAssuranceService(database=test_db)
    service.ingest_wireless_batch(tenant_context, _batch(), key_id=KEY_ID)
    service.ingest_wireless_batch(
        tenant_context,
        _batch(events=[], incidents=[], baselines=[], env_window=_env(wifi_anomaly=False)),
        key_id=KEY_ID,
    )

    with Session(test_db.engine) as session:  # type: ignore[attr-defined]
        rows = session.query(NetworkEnvWindowRecord).all()
        assert len(rows) == 1
        assert rows[0].wifi_anomaly is False


def test_cross_tenant_device_claim_fails_closed(test_db: Database, tenant_context: TenantContext):
    service = NetworkAssuranceService(database=test_db)
    service.ingest_wireless_batch(tenant_context, _batch(), key_id=KEY_ID)

    other_tenant = TenantContext(tenant_id="tenant-beta", subject_id="edge-automation")
    with pytest.raises(NetworkAssuranceConflict):
        service.ingest_wireless_batch(other_tenant, _batch(), key_id=KEY_ID)

    # 冲突方不得写入任何行
    assert _count_rows(test_db, NetworkWirelessEventRecord) == 1


def test_empty_and_oversized_batches_are_rejected_by_contract():
    with pytest.raises(ValidationError):
        WirelessEventUplinkBatch.model_validate({"device_id": DEVICE, "watermark_ms": 1})

    with pytest.raises(ValidationError):
        WirelessEventUplinkBatch.model_validate(
            {
                "device_id": DEVICE,
                "watermark_ms": 1,
                "events": [_event(f"evt-{i}") for i in range(1001)],
            }
        )


class _RecordingKnowledgeBridge:
    """Records create_case calls so the wiring test observes the real trigger."""

    def __init__(self) -> None:
        self.calls: list[str] = []

    def create_case(self, context, incident_id: str):  # noqa: ANN001, ANN201
        self.calls.append(incident_id)
        return f"doc-{incident_id}"


def test_resolved_incident_triggers_knowledge_archival(
    test_db: Database, tenant_context: TenantContext
):
    bridge = _RecordingKnowledgeBridge()
    service = NetworkAssuranceService(database=test_db, knowledge_case_bridge=bridge)

    # OPEN 不触发：故事还没讲完，入库的案例会是半截的
    service.ingest_wireless_batch(tenant_context, _batch(), key_id=KEY_ID)
    assert bridge.calls == []

    resolved = _batch(
        incidents=[_incident(state="RESOLVED")], events=[], baselines=[], env_window=None
    )
    service.ingest_wireless_batch(tenant_context, resolved, key_id=KEY_ID)
    assert bridge.calls == [INCIDENT]


def test_replayed_resolved_incident_reaches_an_idempotent_bridge(
    test_db: Database, tenant_context: TenantContext
):
    """重放会再次进入 create_case——调用方必须按 incident_id 幂等。"""

    bridge = _RecordingKnowledgeBridge()
    service = NetworkAssuranceService(database=test_db, knowledge_case_bridge=bridge)
    resolved = _batch(
        incidents=[_incident(state="RESOLVED")], events=[], baselines=[], env_window=None
    )
    service.ingest_wireless_batch(tenant_context, resolved, key_id=KEY_ID)
    service.ingest_wireless_batch(tenant_context, resolved, key_id=KEY_ID)

    assert bridge.calls == [INCIDENT, INCIDENT]


def test_archival_failure_never_breaks_ingest(test_db: Database, tenant_context: TenantContext):
    class _ExplodingBridge:
        def create_case(self, context, incident_id: str):  # noqa: ANN001, ANN201
            raise RuntimeError("knowledge backend down")

    service = NetworkAssuranceService(
        database=test_db, knowledge_case_bridge=_ExplodingBridge()
    )
    resolved = _batch(
        incidents=[_incident(state="RESOLVED")], events=[], baselines=[], env_window=None
    )
    result = service.ingest_wireless_batch(tenant_context, resolved, key_id=KEY_ID)
    assert result.accepted.incidents == 1  # 事实照常入库


def test_two_gateways_on_one_site_coexist(test_db: Database, tenant_context: TenantContext):
    """多网关共存：同 site 的两台网关各自的事故/事件落成独立行（schema 天然支持）。"""

    gw1 = WirelessEventView(
        event_id="gw1-e1",
        ts_ms=1_700_000_001_000,
        site_id="shop-floor-A",
        gateway_id="gw-1",
        protocol="BLUETOOTH",
        device_address="AA:01",
        address_type="LE_RANDOM",
        hci_index=0,
        event_type="LINK_DISCONNECTED",
        reason="CONNECTION_TIMEOUT",
        source="KERNEL_MGMT",
    )
    gw2 = gw1.model_copy(update={"event_id": "gw2-e1", "gateway_id": "gw-2"})

    service = NetworkAssuranceService(database=test_db)
    service.ingest_wireless_batch(
        tenant_context,
        WirelessEventUplinkBatch.model_validate(
            {
                "device_id": DEVICE,
                "watermark_ms": 1_700_000_001_500,
                "events": [gw1.model_dump(), gw2.model_dump()],
                "incidents": [
                    {
                        "incident_id": "sitinc_shop-floor-A_gw-1_1700000001000",
                        "site_id": "shop-floor-A",
                        "gateway_id": "gw-1",
                        "started_at_ms": 1_700_000_001_000,
                        "last_event_ms": 1_700_000_001_500,
                        "affected_devices": 2,
                        "state": "OPEN",
                        "evidence_event_ids": ["gw1-e1"],
                    },
                    {
                        "incident_id": "sitinc_shop-floor-A_gw-2_1700000001000",
                        "site_id": "shop-floor-A",
                        "gateway_id": "gw-2",
                        "started_at_ms": 1_700_000_001_000,
                        "last_event_ms": 1_700_000_001_500,
                        "affected_devices": 2,
                        "state": "OPEN",
                        "evidence_event_ids": ["gw2-e1"],
                    },
                ],
            }
        ),
        key_id=KEY_ID,
    )

    with Session(test_db.engine) as session:  # type: ignore[attr-defined]
        incidents = list(session.scalars(select(NetworkSiteIncidentRecord)).all())
        events = list(session.scalars(select(NetworkWirelessEventRecord)).all())

    # 同一 site 下两台网关：两行事故、两条事件，互不覆盖
    assert len(incidents) == 2
    assert {i.incident_id for i in incidents} == {
        "sitinc_shop-floor-A_gw-1_1700000001000",
        "sitinc_shop-floor-A_gw-2_1700000001000",
    }
    assert {i.site_id for i in incidents} == {"shop-floor-A"}
    assert {i.gateway_id for i in incidents} == {"gw-1", "gw-2"}
    assert len(events) == 2


def test_baseline_change_appends_history_and_replay_is_idempotent(
    test_db: Database, tenant_context: TenantContext
):
    """append-on-change：L3 baseline trajectory 的唯一来源（B3a）。

    - 首次观测与每次变化各追加一行（UPSERT 当前态会压掉漂移轨迹）；
    - 内容寻址主键 + change-detect 双重幂等：重放不产生重复历史行。
    """
    from datetime import UTC, datetime

    service = NetworkAssuranceService(database=test_db)
    t1 = datetime(2026, 10, 4, 8, 0, tzinfo=UTC)

    # 1) 首次观测 → 轨迹第 0 点
    service.ingest_wireless_batch(tenant_context, _batch(), key_id=KEY_ID, received_at=t1)
    # 2) 完全相同的重放 → 不追加
    service.ingest_wireless_batch(tenant_context, _batch(), key_id=KEY_ID, received_at=t1)

    with Session(test_db.engine) as session:  # type: ignore[attr-defined]
        rows = session.query(NetworkDeviceBaselineHistoryRecord).all()
        assert len(rows) == 1
        first = rows[0]
        assert first.baseline_rssi_dbm == -65
        assert first.state == "STABLE"
        assert first.device_address == "AA:01"

    # 3) baseline 值变化 → 追加第 2 行（不覆盖历史）
    t2 = datetime(2026, 10, 4, 9, 0, tzinfo=UTC)
    drifted = _batch(
        baselines=[_baseline(state="STABLE")], events=[], incidents=[], env_window=None
    )
    drifted.baselines[0] = drifted.baselines[0].model_copy(
        update={"baseline_rssi_dbm": -67}
    )
    service.ingest_wireless_batch(tenant_context, drifted, key_id=KEY_ID, received_at=t2)
    # 4) 变化批次的重放 → 内容寻址主键去重，仍只有 2 行
    service.ingest_wireless_batch(tenant_context, drifted, key_id=KEY_ID, received_at=t2)

    with Session(test_db.engine) as session:  # type: ignore[attr-defined]
        rows = (
            session.query(NetworkDeviceBaselineHistoryRecord)
            .order_by(NetworkDeviceBaselineHistoryRecord.observed_at_ms)
            .all()
        )
        assert len(rows) == 2
        assert [r.baseline_rssi_dbm for r in rows] == [-65, -67]
        assert rows[0].observed_at_ms == int(t1.timestamp() * 1000)
        assert rows[1].observed_at_ms == int(t2.timestamp() * 1000)

        # 当前态行仍是 UPSERT 语义：只有一行、值为最新
        current = session.query(NetworkDeviceBaselineRecord).all()
        assert len(current) == 1
        assert current[0].baseline_rssi_dbm == -67


# ---------------------------------------------------------------------------
# 网关自述动作目录版本（漂移检测输入源，docs/网关动作目录版本契约.md）
# ---------------------------------------------------------------------------


def _catalog_rows(db: Database) -> list:
    with Session(db.engine) as session:  # type: ignore[attr-defined]
        return list(session.query(NetworkGatewayCatalogVersionRecord).all())


def test_catalog_version_is_recorded_when_reported(
    test_db: Database, tenant_context: TenantContext
) -> None:
    """设备自述版本 → 落库为事实（每设备一行）。"""

    service = NetworkAssuranceService(test_db)
    service.ingest_wireless_batch(
        tenant_context, _batch(catalog_version="bdb093ac5310"), key_id=KEY_ID
    )

    rows = _catalog_rows(test_db)
    assert len(rows) == 1
    assert rows[0].device_id == DEVICE
    assert rows[0].tenant_id == tenant_context.tenant_id
    assert rows[0].catalog_version == "bdb093ac5310"
    assert rows[0].reported_at is not None


def test_catalog_version_absent_records_nothing(
    test_db: Database, tenant_context: TenantContext
) -> None:
    """未上报 → 不写任何行。缺失不等于漂移，不得被记成一条漂移事实。"""

    service = NetworkAssuranceService(test_db)
    assert _batch().catalog_version is None
    service.ingest_wireless_batch(tenant_context, _batch(), key_id=KEY_ID)

    assert _catalog_rows(test_db) == []


def test_catalog_version_replay_keeps_single_row(
    test_db: Database, tenant_context: TenantContext
) -> None:
    """同值重放只推进 reported_at，不产生第二行（幂等重放不是新事实）。"""

    service = NetworkAssuranceService(test_db)
    batch = _batch(catalog_version="bdb093ac5310")
    t1 = datetime(2026, 10, 8, 9, 0, tzinfo=UTC)
    t2 = datetime(2026, 10, 8, 9, 5, tzinfo=UTC)
    service.ingest_wireless_batch(tenant_context, batch, key_id=KEY_ID, received_at=t1)
    service.ingest_wireless_batch(tenant_context, batch, key_id=KEY_ID, received_at=t2)

    rows = _catalog_rows(test_db)
    assert len(rows) == 1
    assert rows[0].catalog_version == "bdb093ac5310"
    # SQLite 剥离时区；用仓库既有 as_utc 归一后再比（PG 上是 tz-aware）
    assert as_utc(rows[0].reported_at) == t2


def test_catalog_version_change_updates_in_place(
    test_db: Database, tenant_context: TenantContext
) -> None:
    """设备换目录（升级/降级）→ 覆盖式更新为最新事实，仍是一行。"""

    service = NetworkAssuranceService(test_db)
    service.ingest_wireless_batch(
        tenant_context, _batch(catalog_version="bdb093ac5310"), key_id=KEY_ID
    )
    service.ingest_wireless_batch(
        tenant_context, _batch(catalog_version="deadbeef0000"), key_id=KEY_ID
    )

    rows = _catalog_rows(test_db)
    assert len(rows) == 1
    assert rows[0].catalog_version == "deadbeef0000"


def test_catalog_version_rejects_oversized_value() -> None:
    """契约边界：1–64 字符；超长由 closed model 直接拒收。"""

    with pytest.raises(ValidationError):
        _batch(catalog_version="x" * 65)
    with pytest.raises(ValidationError):
        _batch(catalog_version="")

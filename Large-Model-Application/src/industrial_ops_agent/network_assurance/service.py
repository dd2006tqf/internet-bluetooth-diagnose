"""Ingestion and query logic for WeakNet edge telemetry.

## Layering

This module owns all database access for network assurance. The API layer is
responsible only for authenticating the caller and translating HTTP into
these calls, so that every rule below is testable without an HTTP client.

## Idempotency

A device that does not receive an acknowledgement replays its ring buffer.
Duplicates are therefore *expected*, not exceptional, and are counted rather
than treated as errors — a device retrying after a lost response is behaving
correctly.

The unique constraint on
``(tenant_id, asset_id, network_epoch, sequence_id)`` is the actual
correctness guarantee, since two concurrent submissions of the same snapshot
must not both insert. This module still pre-checks for the common
single-writer case so that ``duplicate`` counts are accurate, but it does not
rely on that check.

## Watermarks

The high-water mark is tracked per ``(device, network_epoch)``, not per
device. ``sequence_id`` restarts at 1 after a reconnect, so a device-level
watermark would classify a legitimate post-reconnect snapshot as stale and
silently discard the first report of a new network epoch — precisely the
report most likely to explain an outage.

## Clock skew

``captured_at`` is device-supplied and therefore untrusted. A device with a
wrong clock (or none, having just booted) will send timestamps in the past or
future. Such a snapshot is still accepted — the observation is real — but it
is stored with the device's own claimed time, and ``received_at`` is recorded
alongside it so an operator can see the discrepancy. Rejecting the snapshot
would lose evidence over a metadata defect.

The one thing clock skew must not do is corrupt the watermark. Watermarks
compare ``sequence_id`` within an epoch, never timestamps across epochs.
"""

from __future__ import annotations

import hashlib
import logging
from dataclasses import dataclass
from datetime import UTC, datetime, timedelta
from typing import Any
from uuid import uuid4

from sqlalchemy import func, select
from sqlalchemy.orm import Session

from industrial_ops_agent.domain import as_utc
from industrial_ops_agent.network_assurance.contracts import (
    NetworkActionResults,
    NetworkAssetDetail,
    NetworkAssetSummary,
    NetworkDeviceTelemetry,
    NetworkTelemetryBatch,
    NetworkTimelinePoint,
    SiteGatewayItem,
    SiteSummary,
)
from industrial_ops_agent.network_assurance.wireless_contracts import (
    BaselineView,
    EnvironmentWindowView,
    WirelessEventIngestResult,
    WirelessEventUplinkBatch,
    WirelessGroupCounts,
)
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import (
    AssetRecord,
    NetworkAssetRecord,
    NetworkDeviceBaselineHistoryRecord,
    NetworkDeviceBaselineRecord,
    NetworkEnvWindowRecord,
    NetworkPendingActionRecord,
    NetworkSiteIncidentRecord,
    NetworkSnapshotRecord,
    NetworkWirelessEventRecord,
)
from industrial_ops_agent.persistence.tenant import TenantContext, validate_boundary_identifier

LOGGER = logging.getLogger("industrial_ops_agent.network_assurance.service")

#: Devices are marked OFFLINE once this long passes with no successful
#: upload. Generation cadence is 10s, so this tolerates several consecutive
#: failures — including the device being bounced by a genuine outage —
#: before it is reported as unreachable. Reporting OFFLINE too eagerly would
#: make the dashboard unusable on a lossy link, which is the normal case
#: this platform exists to diagnose.
DEFAULT_OFFLINE_AFTER_SECONDS = 45

#: A snapshot older than this is stored but not counted as evidence that the
#: device is currently reachable; the heartbeat is what proves liveness.
_MAX_TIMELINE_POINTS = 500

_TIMELINE_WINDOWS: dict[str, timedelta] = {
    "15m": timedelta(minutes=15),
    "1h": timedelta(hours=1),
    "6h": timedelta(hours=6),
    "24h": timedelta(hours=24),
}

#: Health states that warrant surfacing in the asset list rather than being
#: smoothed over. GOOD and UNKNOWN are deliberately absent: UNKNOWN means
#: "not enough evidence", which is not the same as "fine", but it also is not
#: a degradation to alert on.
_DEGRADED_STATES = frozenset({"DEGRADED", "BAD"})

#: Prefix marking a persistence subject as a device rather than a person.
#: Joined with `.` because `TenantContext` accepts only
#: ``[A-Za-z0-9._-]`` in boundary identifiers.
_EDGE_SUBJECT_PREFIX = "edge."

#: 事故状态里"正在发生"的那两个；只有它们才值得开草稿。
#: RESOLVED 不补开——事故已经结束，再开一张草稿只会制造噪音。
_ACTIVE_INCIDENT_STATES = frozenset({"OPEN", "ONGOING"})


def edge_subject_id(device_id: str) -> str:
    """Build the persistence subject that represents one edge device.

    `TenantContext.subject_id` is validated against the platform identifier
    charset, so the device id is re-checked here rather than assumed. A colon
    separator (the obvious choice) is rejected by that charset, which is how
    this helper came to exist.
    """

    subject = f"{_EDGE_SUBJECT_PREFIX}{device_id}"
    return validate_boundary_identifier(subject, field="subject_id")


class NetworkAssuranceNotFound(RuntimeError):
    """The requested device is not registered for this tenant."""


class NetworkAssuranceConflict(RuntimeError):
    """A submitted batch contradicts the current device state."""


@dataclass(frozen=True, slots=True)
class IngestOutcome:
    """Per-snapshot result of an ingest attempt."""

    sequence_id: int
    accepted: bool
    duplicate: bool


@dataclass(frozen=True, slots=True)
class IngestResult:
    """Aggregate result of one batch submission."""

    device_id: str
    accepted: int
    duplicates: int
    outcomes: tuple[IngestOutcome, ...]
    #: Actions the device should apply before its next upload.
    pending_actions: tuple[dict[str, Any], ...]


def _connection_status(
    *,
    overall_state: str,
    last_heartbeat_at: datetime | None,
    now: datetime,
    offline_after_seconds: int,
) -> str:
    """Derive the operator-facing connectivity of a device.

    Deliberately two-stage. A missed heartbeat means the device is not
    currently talking to us, which is a stronger and more certain statement
    than any health verdict: whatever it last reported may no longer hold.
    So OFFLINE outranks WEAK_NET rather than being merged with it.
    """

    if last_heartbeat_at is None:
        return "OFFLINE"
    if now - as_utc(last_heartbeat_at) > timedelta(seconds=offline_after_seconds):
        return "OFFLINE"
    return "WEAK_NET" if overall_state in _DEGRADED_STATES else "ONLINE"


class NetworkAssuranceService:
    """Persist edge telemetry and answer operator queries about devices."""

    def __init__(
        self,
        database: Database,
        *,
        offline_after_seconds: int = DEFAULT_OFFLINE_AFTER_SECONDS,
        auto_incident_draft_enabled: bool = True,
        incident_draft_service_factory: Any = None,
        knowledge_case_bridge: Any = None,
    ) -> None:
        if offline_after_seconds <= 0:
            raise ValueError("offline_after_seconds must be positive")
        self._database = database
        self._offline_after_seconds = offline_after_seconds
        #: S7：收到上行的区域事故时自动开一张**工单草稿**（见 §五）。
        #: 默认开；关掉后仅落事实、不进草稿队列（保留纯观测部署形态）。
        self._auto_incident_draft_enabled = auto_incident_draft_enabled
        #: 工厂注入点：草稿创建走平台既有 IncidentDraftService（含审计与
        #: 权限判定）。未注入时静默跳过——集成测试与纯遥测部署不需要它。
        self._incident_draft_service_factory = incident_draft_service_factory
        #: S8 收录侧：事故结案时把确定性诊断归档为待审知识案例。
        #: 未注入即跳过（集成测试 / 纯遥测部署不需要它）。
        self._knowledge_case_bridge = knowledge_case_bridge

    @property
    def offline_after_seconds(self) -> int:
        return self._offline_after_seconds

    # ------------------------------------------------------------------
    # Ingest
    # ------------------------------------------------------------------

    def ingest_batch(
        self,
        context: TenantContext,
        batch: NetworkTelemetryBatch,
        *,
        key_id: str,
        received_at: datetime | None = None,
    ) -> IngestResult:
        """Store a verified batch, tolerating replay.

        The caller must have verified the signature over the exact bytes that
        produced ``batch``. Nothing here re-validates the signature, because
        re-parsing could accept a body the signature did not cover.
        """

        now = as_utc(received_at or datetime.now(UTC))
        device_id = batch.snapshots[0].device_id

        with self._database.transaction(context) as session:
            asset = self._upsert_asset(session, context, batch, key_id=key_id, now=now)

            outcomes: list[IngestOutcome] = []
            accepted = 0
            duplicates = 0
            for telemetry in batch.snapshots:
                if self._already_stored(session, context, asset.asset_id, telemetry):
                    duplicates += 1
                    outcomes.append(
                        IngestOutcome(
                            sequence_id=telemetry.sequence_id,
                            accepted=False,
                            duplicate=True,
                        )
                    )
                    continue
                self._append_snapshot(session, context, asset.asset_id, telemetry, now=now)
                accepted += 1
                outcomes.append(
                    IngestOutcome(
                        sequence_id=telemetry.sequence_id,
                        accepted=True,
                        duplicate=False,
                    )
                )

            self._advance_asset_headline(session, asset, batch, now=now)
            pending = self._claim_pending_actions(session, context, asset.asset_id, now=now)

        return IngestResult(
            device_id=device_id,
            accepted=accepted,
            duplicates=duplicates,
            outcomes=tuple(outcomes),
            pending_actions=pending,
        )

    # ------------------------------------------------------------------
    # Wireless fact uplink (Phase 4a: events / incidents / baselines / env)
    # ------------------------------------------------------------------

    def ingest_wireless_batch(
        self,
        context: TenantContext,
        batch: WirelessEventUplinkBatch,
        *,
        key_id: str,
        received_at: datetime | None = None,
    ) -> WirelessEventIngestResult:
        """Store a verified wireless-fact batch, tolerating replay.

        Per-group idempotency mirrors the edge tables' own semantics:

        - events     -> ``event_id``: immutable fact, insert-or-duplicate;
        - incidents  -> ``incident_id``: lifecycle row; a payload that advances
          state (OPEN -> RESOLVED) counts as accepted, an identical replay
          counts as duplicate;
        - baselines  -> natural-key hash: profile upsert with the same
          accepted/duplicate accounting;
        - env window -> refreshed silently (diagnostic context, not a counted
          fact group).

        The signature over the exact bytes was verified by the route; nothing
        here re-parses the payload (same rule as :meth:`ingest_batch`).
        ``received_at`` exists for testability and audit symmetry with the
        telemetry path.
        """

        # 事件自带 ts_ms；received_at 现在服务 baseline 历史的观测时刻
        # （append-on-change 的 observed_at 需要一个可测试、可审计的时间源）。
        observed_at = as_utc(received_at or datetime.now(UTC))
        with self._database.transaction(context) as session:
            asset = self._ensure_uplink_asset(
                session, context, device_id=batch.device_id, key_id=key_id
            )

            accepted_events = 0
            duplicate_events = 0
            for view in batch.events:
                existing = session.get(NetworkWirelessEventRecord, view.event_id)
                if existing is not None:
                    self._require_same_tenant(context, existing.tenant_id, view.event_id)
                    duplicate_events += 1
                    continue
                session.add(
                    NetworkWirelessEventRecord(
                        event_id=view.event_id,
                        tenant_id=context.tenant_id,
                        asset_id=asset.asset_id,
                        site_id=view.site_id,
                        gateway_id=view.gateway_id,
                        protocol=view.protocol,
                        device_address=view.device_address,
                        address_type=view.address_type,
                        hci_index=view.hci_index,
                        event_type=view.event_type,
                        ts_ms=view.ts_ms,
                        rssi_at_event_dbm=view.rssi_at_event_dbm,
                        raw_reason_code=view.raw_reason_code,
                        reason=view.reason,
                        source=view.source,
                        source_detail=view.source_detail,
                        details_json=view.details_json,
                    )
                )
                accepted_events += 1

            accepted_incidents = 0
            duplicate_incidents = 0
            opened_incident_ids: list[str] = []
            resolved_incident_ids: list[str] = []
            for view in batch.incidents:
                existing = session.get(NetworkSiteIncidentRecord, view.incident_id)
                if existing is None:
                    session.add(
                        NetworkSiteIncidentRecord(
                            incident_id=view.incident_id,
                            tenant_id=context.tenant_id,
                            asset_id=asset.asset_id,
                            site_id=view.site_id,
                            gateway_id=view.gateway_id,
                            started_at_ms=view.started_at_ms,
                            last_event_ms=view.last_event_ms,
                            resolved_at_ms=view.resolved_at_ms,
                            affected_devices=view.affected_devices,
                            state=view.state,
                            suspected_cause=view.suspected_cause,
                            evidence_event_ids_json=list(view.evidence_event_ids),
                        )
                    )
                    accepted_incidents += 1
                    if view.state in _ACTIVE_INCIDENT_STATES:
                        opened_incident_ids.append(view.incident_id)
                    elif view.state == "RESOLVED":
                        # 按状态入队，不判"是否首次"：上游游标在确认送达前不
                        # 前移，同一 RESOLVED 载荷会重放，而 create_case 按
                        # incident_id 幂等。注意这提供的是**请求级**重试（整批
                        # 失败时板端重发）；create_case 自身失败只记日志——
                        # 归档是下游增强，不值得让事实上行反复失败重试。
                        resolved_incident_ids.append(view.incident_id)
                    continue
                self._require_same_tenant(context, existing.tenant_id, view.incident_id)
                changed = (
                    existing.state != view.state
                    or existing.resolved_at_ms != view.resolved_at_ms
                    or existing.last_event_ms != view.last_event_ms
                    or existing.affected_devices != view.affected_devices
                    or existing.suspected_cause != view.suspected_cause
                )
                if view.state == "RESOLVED":
                    # 与插入分支同理：按状态触发、按 id 幂等，重放即重试。
                    resolved_incident_ids.append(view.incident_id)
                if changed:
                    # 时间窗与板端一致：last_event 只右移、started 只左移，
                    # 乱序/迟到的重放不得把事故窗口拉回去。
                    existing.state = view.state
                    existing.resolved_at_ms = view.resolved_at_ms
                    existing.last_event_ms = max(existing.last_event_ms, view.last_event_ms)
                    existing.started_at_ms = min(existing.started_at_ms, view.started_at_ms)
                    existing.affected_devices = view.affected_devices
                    existing.suspected_cause = view.suspected_cause
                    if view.evidence_event_ids:
                        existing.evidence_event_ids_json = list(view.evidence_event_ids)
                    accepted_incidents += 1
                else:
                    duplicate_incidents += 1

            accepted_baselines = 0
            duplicate_baselines = 0
            for view in batch.baselines:
                baseline_id = self._wireless_baseline_id(context, asset.asset_id, view)
                existing = session.get(NetworkDeviceBaselineRecord, baseline_id)
                if existing is None:
                    session.add(
                        NetworkDeviceBaselineRecord(
                            baseline_id=baseline_id,
                            tenant_id=context.tenant_id,
                            asset_id=asset.asset_id,
                            site_id=view.site_id,
                            gateway_id=view.gateway_id,
                            hci_index=view.hci_index,
                            protocol=view.protocol,
                            address_type=view.address_type,
                            device_address=view.device_address,
                            baseline_rssi_dbm=view.baseline_rssi_dbm,
                            min_seen_rssi_dbm=view.min_seen_rssi_dbm,
                            max_seen_rssi_dbm=view.max_seen_rssi_dbm,
                            baseline_sample_count=view.baseline_sample_count,
                            state=view.state,
                        )
                    )
                    # 首次观测即轨迹第 0 点（L1 合法演进：追加，不覆盖）
                    self._append_baseline_history(
                        session, context, baseline_id, asset.asset_id, view, observed_at
                    )
                    accepted_baselines += 1
                    continue
                self._require_same_tenant(context, existing.tenant_id, baseline_id)
                changed = (
                    existing.baseline_rssi_dbm != view.baseline_rssi_dbm
                    or existing.min_seen_rssi_dbm != view.min_seen_rssi_dbm
                    or existing.max_seen_rssi_dbm != view.max_seen_rssi_dbm
                    or existing.baseline_sample_count != view.baseline_sample_count
                    or existing.state != view.state
                )
                if changed:
                    # 变化即追加（append-on-change）：UPSERT 当前态会把
                    # -60 → -61 → -62 的漂移压成最后一行，慢性退化信号丢失；
                    # 历史行让 L3 的 baseline_trajectory 可观测。
                    self._append_baseline_history(
                        session, context, baseline_id, asset.asset_id, view, observed_at
                    )
                    existing.baseline_rssi_dbm = view.baseline_rssi_dbm
                    existing.min_seen_rssi_dbm = view.min_seen_rssi_dbm
                    existing.max_seen_rssi_dbm = view.max_seen_rssi_dbm
                    existing.baseline_sample_count = view.baseline_sample_count
                    existing.state = view.state
                    accepted_baselines += 1
                else:
                    duplicate_baselines += 1

            if batch.env_window is not None:
                self._upsert_env_window(session, context, asset.asset_id, batch.env_window)

        # 事务提交后再开草稿：草稿创建走平台既有服务（含审计与权限判定），
        # 自带事务；在 ingest 事务内调用会形成跨服务的锁嵌套。
        for incident_id in opened_incident_ids:
            self._open_incident_draft(context, incident_id, device_id=batch.device_id)

        # S8 收录侧：结案事故归档为知识案例草稿（同样在 ingest 事务之外）。
        # "绝不冒泡"在这里兜底而不是只托付给桥内部：归档是下游增强，调用方
        # 必须保证它不会把已经提交的事实上行变成 500。
        if self._knowledge_case_bridge is not None:
            for incident_id in resolved_incident_ids:
                try:
                    self._knowledge_case_bridge.create_case(context, incident_id)
                except Exception as exc:  # noqa: BLE001 - 下游增强不许拖垮入库
                    LOGGER.warning(
                        "knowledge archival failed for %s: %s",
                        incident_id,
                        exc,
                        exc_info=True,
                    )

        return WirelessEventIngestResult(
            accepted=WirelessGroupCounts(
                events=accepted_events,
                incidents=accepted_incidents,
                baselines=accepted_baselines,
            ),
            duplicates=WirelessGroupCounts(
                events=duplicate_events,
                incidents=duplicate_incidents,
                baselines=duplicate_baselines,
            ),
        )

    def _ensure_uplink_asset(
        self,
        session: Session,
        context: TenantContext,
        *,
        device_id: str,
        key_id: str,
    ) -> NetworkAssetRecord:
        """Resolve (or implicitly register) the gateway asset for an uplink.

        Same implicit-registration rule as telemetry: a device that presents a
        valid signature over a well-formed payload is by definition part of
        this deployment. No snapshot/heartbeat is touched here — wireless
        facts must not fake telemetry liveness.
        """

        record = session.get(NetworkAssetRecord, device_id)
        if record is not None and record.tenant_id != context.tenant_id:
            raise NetworkAssuranceConflict("device is registered to another tenant")
        if record is None:
            record = NetworkAssetRecord(
                asset_id=device_id,
                tenant_id=context.tenant_id,
                version=1,
            )
            session.add(record)
        record.signing_key_id = key_id
        return record

    def _open_incident_draft(
        self,
        context: TenantContext,
        incident_id: str,
        *,
        device_id: str,
    ) -> None:
        """为新开的区域事故自动建一张**工单草稿**（S7）。

        为什么止步于草稿，而不是直接开单：
        平台的正式 incident 需要 ``evidence.status == CONFIRMED`` 且至少一个
        CLEAN 媒体对象（``application/incidents.py:1433``），工单还需要已批准的
        proposal。平台没有"系统主体已确认证据"的通道，自动开单就必须伪造证据或
        绕过证据门禁——两者都会摧毁这套系统的可信度，因此不做。

        草稿是**提议**而非状态变更：它进入运营方的队列，由人补证据并确认，
        之后平台原有的提交/审批/派工链路照常运转。这就是本阶段能给的自动化边界。

        幂等：以确定性 idempotency_key 调用平台服务；重复上行同一事故不会产生
        第二张草稿（服务端按 key 返回既有草稿，created=False）。

        失败绝不抛给调用方：草稿是诊断事实的下游增强，不能被它拖垮上游入库。
        """

        if not self._auto_incident_draft_enabled or self._incident_draft_service_factory is None:
            return

        try:
            service, identity = self._incident_draft_service_factory(context)
            service.create(
                identity,
                asset_id=device_id,
                description=(
                    f"区域无线异常自动草稿：网关 {device_id} 上报区域事故 "
                    f"{incident_id}（多台设备同时段异常）。"
                    "请在补充现场证据后确认提交。"
                ),
                idempotency_key=f"weaknet-incident-draft:{incident_id}",
                request_id=f"weaknet-auto-draft:{incident_id}",
            )
        except Exception as exc:  # noqa: BLE001 - 草稿失败不得影响事实入库
            LOGGER.warning(
                "auto incident draft failed for %s: %s", incident_id, exc, exc_info=True
            )

    @staticmethod
    def _require_same_tenant(context: TenantContext, row_tenant: str, row_id: str) -> None:
        if row_tenant != context.tenant_id:
            raise NetworkAssuranceConflict(f"row {row_id} is registered to another tenant")

    @staticmethod
    def _wireless_baseline_id(context: TenantContext, asset_id: str, view: BaselineView) -> str:
        seed = (
            f"{context.tenant_id}|{asset_id}|{view.site_id}|{view.gateway_id}"
            f"|{view.hci_index}|{view.protocol}|{view.address_type}|{view.device_address}"
        )
        return "nbli-" + hashlib.sha1(seed.encode()).hexdigest()[:32]

    @staticmethod
    def _append_baseline_history(
        session: Any,
        context: TenantContext,
        baseline_id: str,
        asset_id: str,
        view: BaselineView,
        observed_at: datetime,
    ) -> None:
        """append-on-change 记录 baseline 轨迹（L3 的 prediction_reference 来源）。

        - 主键内容寻址 (tenant|baseline|观测毫秒|值|状态)：同一变化重放命中主键即跳过，
          与 ingest 的 change-detect 双重幂等；
        - 只在**首次观测**与**变化**时追加（由调用点保证），历史行永不更新；
        - 观测时刻取 ingest 时间（板端 baseline 上行契约不携带时间戳）。
        """
        observed_at_ms = int(observed_at.timestamp() * 1000)
        seed = (
            f"{context.tenant_id}|{baseline_id}|{observed_at_ms}"
            f"|{view.baseline_rssi_dbm}|{view.state}"
        )
        history_id = "nblh-" + hashlib.sha1(seed.encode()).hexdigest()[:32]
        if session.get(NetworkDeviceBaselineHistoryRecord, history_id) is not None:
            return
        session.add(
            NetworkDeviceBaselineHistoryRecord(
                history_id=history_id,
                tenant_id=context.tenant_id,
                baseline_id=baseline_id,
                asset_id=asset_id,
                device_address=view.device_address,
                observed_at_ms=observed_at_ms,
                baseline_rssi_dbm=view.baseline_rssi_dbm,
                baseline_sample_count=view.baseline_sample_count,
                state=view.state,
            )
        )

    @staticmethod
    def _wireless_window_id(
        context: TenantContext, asset_id: str, view: EnvironmentWindowView
    ) -> str:
        """环境窗口的确定性键：**(tenant, asset, 批次最小事实时间)**。

        窗口的 `to_ms` 与板端上行批次绑定——同一批事实无论重放多少次、无论
        携带的深度内核快照内容如何变化，都是同一行（内容 UPSERT 覆盖）。
        用 `from_ms`（板端恒为 0）+ `to_ms` 的组合而非墙钟，保证"同一批事实
        只占一行"这一幂等语义在两端一致。

        板端把 `to_ms` 设为该批事件/事故的最新时间；纯基线批次没有稀疏事实，
        `to_ms` 为 0 —— 此时窗口不含内核快照（快照只在有事件时携带），
        因此不会与"真实事实窗口"混淆。
        """

        seed = f"{context.tenant_id}|{asset_id}|{view.to_ms}"
        return "nenv-" + hashlib.sha1(seed.encode()).hexdigest()[:32]

    def _upsert_env_window(
        self,
        session: Session,
        context: TenantContext,
        asset_id: str,
        view: EnvironmentWindowView,
    ) -> None:
        window_id = self._wireless_window_id(context, asset_id, view)
        existing = session.get(NetworkEnvWindowRecord, window_id)
        if existing is None:
            session.add(
                NetworkEnvWindowRecord(
                    window_id=window_id,
                    tenant_id=context.tenant_id,
                    asset_id=asset_id,
                    from_ms=view.from_ms,
                    to_ms=view.to_ms,
                    available=view.available,
                    link_type=view.link_type,
                    wifi_anomaly=view.wifi_anomaly,
                    coexistence_warning=view.coexistence_warning,
                    snapshots_json=list(view.snapshots),
                )
            )
            return
        self._require_same_tenant(context, existing.tenant_id, window_id)
        existing.available = view.available
        existing.link_type = view.link_type
        existing.wifi_anomaly = view.wifi_anomaly
        existing.coexistence_warning = view.coexistence_warning
        existing.snapshots_json = list(view.snapshots)

    def record_action_results(
        self,
        context: TenantContext,
        results: NetworkActionResults,
        *,
        received_at: datetime | None = None,
    ) -> int:
        """Mark queued actions as applied, rejected, or rolled back.

        Verification invariants:
          - Tenant scoping: ``record.tenant_id == context.tenant_id`` (implicit)
          - Asset ownership: ``record.asset_id == results.device_id``
          - One-time token binding: if the stored record has a non-empty
            ``claim_token``, incoming ``outcome.claim_token`` must match.
            Empty ``outcome.claim_token`` is tolerated only if the stored record
            also has none (v1 compatibility window).
          - Terminal states: once APPLIED, REJECTED, or ROLLBACK, a record is
            never re-updated.
        """

        now = as_utc(received_at or datetime.now(UTC))
        updated = 0
        with self._database.transaction(context) as session:
            for outcome in results.results:
                record = session.get(NetworkPendingActionRecord, outcome.action_id)
                if record is None or record.asset_id != results.device_id:
                    continue
                if record.status in ("APPLIED", "REJECTED", "ROLLBACK"):
                    continue

                # Anti-replay / claim token matching:
                # If the action was claimed with a token, the device MUST echo
                # it back. An empty token on a token-claimed action is rejected.
                if record.claim_token and outcome.claim_token != record.claim_token:
                    continue

                record.status = outcome.status
                record.result_detail = outcome.detail[:1024]
                record.completed_at = now
                record.version += 1
                updated += 1
        return updated

    # ------------------------------------------------------------------
    # Queries
    # ------------------------------------------------------------------

    def list_assets(self, context: TenantContext) -> list[NetworkAssetSummary]:
        now = datetime.now(UTC)
        with self._database.transaction(context) as session:
            records = session.execute(
                select(NetworkAssetRecord)
                .where(NetworkAssetRecord.tenant_id == context.tenant_id)
                .order_by(NetworkAssetRecord.asset_id)
            ).scalars()
            return [self._to_summary(record, now=now) for record in records]

    def list_sites(self, context: TenantContext) -> list[SiteSummary]:
        """每个现场有哪几台网关（M2 现场聚合的查询出口）。

        现场/网关身份只随事实携带——``network_assets`` 行没有现场列，所以三张
        事实表（baselines / events / incidents）取并集：任何一张都可能是一台
        网关的首次出现（例如基线尚未建立但已有事件）。``site_id`` 为空的历史
        缺省行不是一个现场，不呈现为匿名分组。展示字段从 ``network_assets``
        富化；未注册的网关如实出现（少报现场网关数比多报更危险），富化字段为
        None。与诊断侧按事件时刻取窗口不同，这里回答的是"当前库存"。
        """
        now = datetime.now(UTC)
        with self._database.transaction(context) as session:
            pairs: set[tuple[str, str]] = set()
            for model in (
                NetworkDeviceBaselineRecord,
                NetworkWirelessEventRecord,
                NetworkSiteIncidentRecord,
            ):
                rows = session.execute(
                    select(model.site_id, model.gateway_id)
                    .where(
                        model.tenant_id == context.tenant_id,
                        model.site_id != "",
                        model.gateway_id != "",
                    )
                    .distinct()
                ).all()
                pairs.update((site_id, gateway_id) for site_id, gateway_id in rows)

            if not pairs:
                return []

            gateway_ids = {gateway_id for _, gateway_id in pairs}
            assets = {
                record.asset_id: record
                for record in session.scalars(
                    select(NetworkAssetRecord).where(
                        NetworkAssetRecord.tenant_id == context.tenant_id,
                        NetworkAssetRecord.asset_id.in_(gateway_ids),
                    )
                ).all()
            }

            by_site: dict[str, list[SiteGatewayItem]] = {}
            for site_id, gateway_id in sorted(pairs):
                asset = assets.get(gateway_id)
                by_site.setdefault(site_id, []).append(
                    SiteGatewayItem(
                        gateway_id=gateway_id,
                        display_name=asset.display_name if asset else None,
                        connection_status=_connection_status(
                            overall_state=asset.overall_state,
                            last_heartbeat_at=asset.last_heartbeat_at,
                            now=now,
                            offline_after_seconds=self._offline_after_seconds,
                        )
                        if asset
                        else None,
                        last_heartbeat_at=as_utc(asset.last_heartbeat_at)
                        if asset and asset.last_heartbeat_at
                        else None,
                    )
                )
            return [
                SiteSummary(site_id=site_id, gateways=gateways)
                for site_id, gateways in sorted(by_site.items())
            ]

    def get_asset(self, context: TenantContext, asset_id: str) -> NetworkAssetDetail:
        now = datetime.now(UTC)
        with self._database.transaction(context) as session:
            record = session.get(NetworkAssetRecord, asset_id)
            if record is None or record.tenant_id != context.tenant_id:
                raise NetworkAssuranceNotFound(asset_id)
            return NetworkAssetDetail(
                summary=self._to_summary(record, now=now),
                hardware_arch=record.hardware_arch,
                os_kernel=record.os_kernel,
                mac_address=record.mac_address,
                gateway_ip=record.gateway_ip,
                dns_servers=list(record.dns_servers_json or []),
                ap_ssid=record.ap_ssid,
                ap_bssid=record.ap_bssid,
                rtt_interval_seconds=record.rtt_interval_seconds,
                rtt_probe_targets=list(record.rtt_probe_targets_json or []),
                latest_experience=record.latest_snapshot_json,
            )

    def get_kernel_snapshot_extras(
        self, context: TenantContext, asset_id: str
    ) -> dict[str, Any]:
        """最近一批上行的深度内核快照（进程画像 / skb_drop 归因）。

        这是**呈现层证据**，不是判定输入：云端规则引擎与 W1~W6 因果护栏都不读
        它。Copilot 用它把"网络拥塞/丢包"解释到具体进程与内核 drop 原因，
        但结论仍只能来自边缘的 SLE 判定。

        按最近一次 UPSERT（``updated_at``）取窗口，``to_ms`` 作平手裁决。
        不能按 ``to_ms`` 排序：板端把窗口时间轴锚定在稀疏事实上，安静期每轮
        把当前快照刷进同一个 ``to_ms=0`` 行，而事实窗口停留在事件时刻——按
        ``to_ms`` 排序会把展示固定在历史事实窗口（实测落后近 20 小时）。
        无窗口或无快照条目时返回空字典，调用方据此静默省略该段
        （绝不伪造"内核观测正常"）。
        """

        with self._database.transaction(context) as session:
            record = session.scalars(
                select(NetworkEnvWindowRecord)
                .where(
                    NetworkEnvWindowRecord.tenant_id == context.tenant_id,
                    NetworkEnvWindowRecord.asset_id == asset_id,
                )
                .order_by(
                    NetworkEnvWindowRecord.updated_at.desc(),
                    NetworkEnvWindowRecord.to_ms.desc(),
                )
                .limit(1)
            ).first()
            if record is None or not record.snapshots_json:
                return {}

            extras: dict[str, Any] = {}
            for item in record.snapshots_json:
                if not isinstance(item, dict):
                    continue
                kind = item.get("kind")
                # 只认已评审的两种 kind；板端写错值时忽略而不是透传，
                # 否则前端会拿到无法解释的结构。
                if kind == "process_top" and isinstance(item.get("top_processes"), list):
                    extras["process_top"] = item["top_processes"]
                elif kind == "skb_drop_hist":
                    extras["skb_drop_hist"] = {
                        "total_drops": item.get("total_drops", 0),
                        "top_reasons": item.get("top_reasons", []),
                    }
            return extras

    def timeline(
        self,
        context: TenantContext,
        asset_id: str,
        *,
        window: str = "15m",
    ) -> list[NetworkTimelinePoint]:
        """Return assessments for one device, oldest first.

        Ordering is by device-claimed ``captured_at`` because that is what a
        human reads a timeline by, but the window is applied against
        ``received_at`` — a device with a badly wrong clock would otherwise
        be able to pull arbitrary history into, or hide its recent activity
        from, the operator's view.
        """

        span = _TIMELINE_WINDOWS.get(window)
        if span is None:
            raise ValueError(f"unsupported timeline window: {window}")

        cutoff = datetime.now(UTC) - span
        with self._database.transaction(context) as session:
            asset = session.get(NetworkAssetRecord, asset_id)
            if asset is None or asset.tenant_id != context.tenant_id:
                raise NetworkAssuranceNotFound(asset_id)
            records = (
                session.execute(
                    select(NetworkSnapshotRecord)
                    .where(
                        NetworkSnapshotRecord.tenant_id == context.tenant_id,
                        NetworkSnapshotRecord.asset_id == asset_id,
                        NetworkSnapshotRecord.received_at >= cutoff,
                    )
                    .order_by(NetworkSnapshotRecord.captured_at.desc())
                    .limit(_MAX_TIMELINE_POINTS)
                )
                .scalars()
                .all()
            )

        return [
            NetworkTimelinePoint(
                sequence_id=record.sequence_id,
                network_epoch=record.network_epoch,
                config_generation=record.config_generation,
                captured_at=as_utc(record.captured_at),
                overall_state=record.overall_state,
                display_score=record.display_score,
                primary_issue=record.primary_issue,
            )
            for record in reversed(records)
        ]

    def queue_action(
        self,
        context: TenantContext,
        asset_id: str,
        *,
        config_key: str,
        config_value: str,
        issued_by: str,
        approved_by: str | None = None,
    ) -> str:
        """Queue a configuration change for delivery on the device's next upload.

        Each queued action gets:
          - A cryptographic anti-replay ``nonce``
          - A monotonic per-device ``generation`` (independent of config_generation)
          - Tenant scoping (enforced by TenantScopedMixin)
        """

        now = datetime.now(UTC)
        action_id = f"nact-{uuid4().hex}"
        nonce = uuid4().hex
        with self._database.transaction(context) as session:
            asset = session.get(NetworkAssetRecord, asset_id)
            if asset is None or asset.tenant_id != context.tenant_id:
                raise NetworkAssuranceNotFound(asset_id)

            # 动作 generation：每个 asset 维护自己的单调递增序列，与快照失效解耦。
            max_gen = session.execute(
                select(func.coalesce(func.max(NetworkPendingActionRecord.generation), 0)).where(
                    NetworkPendingActionRecord.tenant_id == context.tenant_id,
                    NetworkPendingActionRecord.asset_id == asset_id,
                )
            ).scalar_one()
            next_generation = int(max_gen) + 1

            session.add(
                NetworkPendingActionRecord(
                    action_id=action_id,
                    tenant_id=context.tenant_id,
                    asset_id=asset_id,
                    generation=next_generation,
                    nonce=nonce,
                    config_key=config_key,
                    config_value=config_value,
                    status="QUEUED",
                    issued_by=issued_by[:128],
                    approved_by=approved_by[:128] if approved_by else None,
                    issued_at=now,
                    version=1,
                )
            )
        return action_id

    # ------------------------------------------------------------------
    # Internals
    # ------------------------------------------------------------------

    def _upsert_asset(
        self,
        session: Session,
        context: TenantContext,
        batch: NetworkTelemetryBatch,
        *,
        key_id: str,
        now: datetime,
    ) -> NetworkAssetRecord:
        """Create the asset on first contact, then refresh its descriptors.

        Registration is implicit — a device that presents valid credentials
        and a well-formed payload is, by definition, a device this deployment
        has provisioned. Requiring an out-of-band registration step would add
        an operational dependency between provisioning a key and seeing the
        device in the list.
        """

        first = batch.snapshots[0]
        asset_id = first.device_id
        record = session.get(NetworkAssetRecord, asset_id)
        if record is not None and record.tenant_id != context.tenant_id:
            # Same device id claimed by two tenants. Refusing is the only safe
            # answer: the alternative is silently re-homing a device, which
            # would move its history across a tenant boundary.
            raise NetworkAssuranceConflict("device is registered to another tenant")

        if record is None:
            record = NetworkAssetRecord(
                asset_id=asset_id,
                tenant_id=context.tenant_id,
                version=1,
            )
            session.add(record)

        snapshot = first.snapshot
        record.display_name = first.display_name or record.display_name
        record.location = first.location or record.location
        record.hardware_arch = snapshot.hardware_arch or record.hardware_arch
        record.os_kernel = snapshot.os_kernel or record.os_kernel
        record.active_iface = snapshot.interface or record.active_iface
        record.link_type = snapshot.link_type
        record.mac_address = snapshot.mac_address or record.mac_address
        record.ip_address = snapshot.ip_address or record.ip_address
        record.gateway_ip = snapshot.gateway_ip or record.gateway_ip
        record.dns_servers_json = list(snapshot.dns_servers)
        record.ap_ssid = snapshot.ap_ssid or record.ap_ssid
        record.ap_bssid = snapshot.ap_bssid or record.ap_bssid
        record.signing_key_id = key_id

        # Heartbeat advances only on acceptance, and only forward. A replayed
        # batch must not rewind it, or an operator would see a device that is
        # actively reporting appear to stop.
        newest = max(batch.snapshots, key=lambda item: item.sequence_id)
        self._touch_heartbeat(record, newest, now=now)

        # S6 资产桥：网关在售后资产域（assets）取得同一身份。
        # 没有这一步，平台 incidents.asset_id 的外键无从满足，区域事故就
        # 永远开不出工单（见集成设计 §四）。桥在网关注册/心跳成功后同事务内
        # 建立，保证"出现在资产池"与"被本平台观测"两件事同时为真。
        self._bridge_gateway_asset(session, context, record, now=now)
        return record

    @staticmethod
    def _bridge_gateway_asset(
        session: Session,
        context: TenantContext,
        record: NetworkAssetRecord,
        *,
        now: datetime,
    ) -> AssetRecord:
        """把网关镜像为 assets 表的资产行（幂等 UPSERT）。

        身份约定：
        - ``source_system="weaknet"``、``source_record_id=<network asset_id>``
          构成 UPSERT 键（唯一约束 uq_asset_source），device_id 变化不会造出重复行；
        - ``asset_id`` 沿用 device_id —— 事故/工单链路直接引用它可读性最好；
        - 现场被观测的无线设备**不**入资产池（决策 D2）：它们是被观测对象，
          不是被维护对象，只有网关本身是平台的服务对象。

        幂等：已有行只刷新描述字段并推进 version，绝不覆盖运营方在平台侧
        补充的 serial_number / lifecycle_status（那是人的输入，不是遥测能覆盖的）。
        """

        source_system = "weaknet"
        source_record_id = record.asset_id

        existing = session.execute(
            select(AssetRecord).where(
                AssetRecord.tenant_id == context.tenant_id,
                AssetRecord.source_system == source_system,
                AssetRecord.source_record_id == source_record_id,
            )
        ).scalar_one_or_none()

        display_name = record.display_name or record.asset_id
        if existing is None:
            asset = AssetRecord(
                asset_id=record.asset_id,
                tenant_id=context.tenant_id,
                source_system=source_system,
                source_record_id=source_record_id,
                as_of=now,
                model_code="weaknet-gateway",
                display_name=display_name,
                lifecycle_status="IN_SERVICE",
                version=1,
            )
            session.add(asset)
            return asset

        changed = (
            existing.display_name != display_name
            or not (existing.model_code or "").strip()
        )
        if changed:
            existing.display_name = display_name
            existing.model_code = existing.model_code or "weaknet-gateway"
            existing.as_of = now
            existing.version = (existing.version or 1) + 1
        return existing

    def _touch_heartbeat(
        self,
        record: NetworkAssetRecord,
        telemetry: NetworkDeviceTelemetry,
        *,
        now: datetime,
    ) -> None:
        current_epoch = record.network_epoch
        current_sequence = record.latest_sequence_id
        if (
            current_epoch is not None
            and current_sequence is not None
            and telemetry.network_epoch == current_epoch
            and telemetry.sequence_id <= current_sequence
        ):
            return
        record.network_epoch = telemetry.network_epoch
        record.latest_sequence_id = telemetry.sequence_id
        record.last_heartbeat_at = now

    def _already_stored(
        self,
        session: Session,
        context: TenantContext,
        asset_id: str,
        telemetry: NetworkDeviceTelemetry,
    ) -> bool:
        existing = session.execute(
            select(NetworkSnapshotRecord.snapshot_id).where(
                NetworkSnapshotRecord.tenant_id == context.tenant_id,
                NetworkSnapshotRecord.asset_id == asset_id,
                NetworkSnapshotRecord.network_epoch == telemetry.network_epoch,
                NetworkSnapshotRecord.sequence_id == telemetry.sequence_id,
            )
        ).first()
        return existing is not None

    def _append_snapshot(
        self,
        session: Session,
        context: TenantContext,
        asset_id: str,
        telemetry: NetworkDeviceTelemetry,
        *,
        now: datetime,
    ) -> None:
        snapshot = telemetry.snapshot
        session.add(
            NetworkSnapshotRecord(
                snapshot_id=f"nsnap-{uuid4().hex}",
                tenant_id=context.tenant_id,
                asset_id=asset_id,
                sequence_id=telemetry.sequence_id,
                network_epoch=telemetry.network_epoch,
                config_generation=telemetry.config_generation,
                captured_at=telemetry.captured_at,
                received_at=now,
                overall_state=snapshot.overall_state,
                display_score=snapshot.display_score,
                primary_issue=snapshot.primary_issue,
                experience_json=self._experience_payload(telemetry),
            )
        )

    def _advance_asset_headline(
        self,
        session: Session,
        asset: NetworkAssetRecord,
        batch: NetworkTelemetryBatch,
        *,
        now: datetime,
    ) -> None:
        """Point the asset at its newest snapshot by sequence within the epoch.

        Selecting by ``sequence_id`` rather than by timestamp means a device
        with a skewed clock still shows its genuinely most recent verdict,
        rather than whichever row happens to carry the largest wall time.
        """

        newest = max(batch.snapshots, key=lambda item: item.sequence_id)
        if not self._is_current_headline(asset, newest):
            return
        snapshot = newest.snapshot
        asset.overall_state = snapshot.overall_state
        asset.primary_issue = snapshot.primary_issue
        asset.display_score = snapshot.display_score
        asset.config_generation = newest.config_generation
        asset.rtt_interval_seconds = newest.rtt_interval_seconds
        asset.rtt_probe_targets_json = list(newest.rtt_probe_targets)
        asset.latest_snapshot_json = self._experience_payload(newest)
        asset.connection_status = _connection_status(
            overall_state=snapshot.overall_state,
            last_heartbeat_at=asset.last_heartbeat_at,
            now=now,
            offline_after_seconds=self._offline_after_seconds,
        )
        asset.version += 1

    @staticmethod
    def _is_current_headline(asset: NetworkAssetRecord, telemetry: NetworkDeviceTelemetry) -> bool:
        if asset.network_epoch is None or asset.latest_sequence_id is None:
            return True
        if telemetry.network_epoch != asset.network_epoch:
            # A new epoch always wins: it represents the link as it is now.
            return True
        return telemetry.sequence_id >= asset.latest_sequence_id

    @staticmethod
    def _experience_payload(telemetry: NetworkDeviceTelemetry) -> dict[str, Any]:
        payload = telemetry.snapshot.model_dump(mode="json")
        payload["device_id"] = telemetry.device_id
        payload["sequence_id"] = telemetry.sequence_id
        payload["network_epoch"] = telemetry.network_epoch
        payload["config_generation"] = telemetry.config_generation
        payload["captured_at"] = telemetry.captured_at.isoformat()
        return payload

    def _claim_pending_actions(
        self,
        session: Session,
        context: TenantContext,
        asset_id: str,
        *,
        now: datetime,
    ) -> tuple[dict[str, Any], ...]:
        """Hand out queued actions and mark them delivered.

        Delivery uses pessimistic locking (``with_for_update``) on the pending
        action rows so concurrent ingest batches for the same asset do not
        claim the same action twice.

        Each delivered action carries:
          - ``generation``: monotonic per-device sequence for edge-side replay check
          - ``nonce``: anti-replay token
          - ``claim_token``: generated here and recorded on the record; the
            device must echo this exact token back in ``record_action_results``
        """

        records = (
            session.execute(
                select(NetworkPendingActionRecord)
                .where(
                    NetworkPendingActionRecord.tenant_id == context.tenant_id,
                    NetworkPendingActionRecord.asset_id == asset_id,
                    NetworkPendingActionRecord.status == "QUEUED",
                )
                .order_by(NetworkPendingActionRecord.issued_at)
                .with_for_update()
            )
            .scalars()
            .all()
        )
        delivered: list[dict[str, Any]] = []
        for record in records:
            claim_token = uuid4().hex
            record.status = "DELIVERED"
            record.delivered_at = now
            record.claimed_by_device_id = asset_id
            record.claim_token = claim_token
            record.version += 1
            delivered.append(
                {
                    "action_id": record.action_id,
                    "key": record.config_key,
                    "value": record.config_value,
                    "generation": record.generation,
                    "nonce": record.nonce,
                    "claim_token": claim_token,
                }
            )
        return tuple(delivered)

    def _to_summary(self, record: NetworkAssetRecord, *, now: datetime) -> NetworkAssetSummary:
        return NetworkAssetSummary(
            asset_id=record.asset_id,
            tenant_id=record.tenant_id,
            display_name=record.display_name,
            location=record.location,
            active_iface=record.active_iface,
            link_type=record.link_type,
            ip_address=record.ip_address,
            overall_state=record.overall_state,
            primary_issue=record.primary_issue,
            display_score=record.display_score,
            connection_status=_connection_status(
                overall_state=record.overall_state,
                last_heartbeat_at=record.last_heartbeat_at,
                now=now,
                offline_after_seconds=self._offline_after_seconds,
            ),
            last_heartbeat_at=as_utc(record.last_heartbeat_at)
            if record.last_heartbeat_at
            else None,
            config_generation=record.config_generation,
            network_epoch=record.network_epoch,
        )

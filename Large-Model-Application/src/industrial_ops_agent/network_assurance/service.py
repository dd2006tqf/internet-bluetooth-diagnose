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
)
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import (
    NetworkAssetRecord,
    NetworkPendingActionRecord,
    NetworkSnapshotRecord,
)
from industrial_ops_agent.persistence.tenant import TenantContext, validate_boundary_identifier

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
    ) -> None:
        if offline_after_seconds <= 0:
            raise ValueError("offline_after_seconds must be positive")
        self._database = database
        self._offline_after_seconds = offline_after_seconds

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
            max_gen = (
                session.execute(
                    select(func.coalesce(func.max(NetworkPendingActionRecord.generation), 0))
                    .where(
                        NetworkPendingActionRecord.tenant_id == context.tenant_id,
                        NetworkPendingActionRecord.asset_id == asset_id,
                    )
                ).scalar_one()
            )
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
        return record

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
    def _is_current_headline(
        asset: NetworkAssetRecord, telemetry: NetworkDeviceTelemetry
    ) -> bool:
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

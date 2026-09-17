"""Closed interchange contracts for WeakNet edge network telemetry.

This module is deliberately separate from ``industrial_ops_agent.edge``: that
package governs signed offline *diagnosis packs* issued by the server to a
field runner, using JWS/EdDSA over a closed claim set. Here the direction is
inverted — an unattended edge device pushes its own observations *up* to the
server — so the two protocols must not share a verifier or a wire format.

## Why the signature covers raw bytes

The edge signer is C++ (OpenSSL) and the verifier is Python
(``cryptography``). Any scheme that has both sides re-serialize a structure
into "canonical JSON" before signing inherits a silent-failure risk across
languages: number formatting (``1.0`` vs ``1``), Unicode escaping, key
ordering and whitespace all differ between ``nlohmann::json`` and
``json.dumps``. A payload that verifies in testing can fail after an
unrelated library upgrade.

Therefore the signature is computed over *the exact bytes that travel on the
wire*. The edge signs its own serialized body; the server reads the request
body verbatim and verifies before parsing. Nothing is re-serialized.

## Where the credentials live

The device token, key id and signature travel as **HTTP headers**, and the
request body is the bare :class:`NetworkTelemetryBatch` JSON::

    POST /api/v1/network/edge/telemetry
    X-Edge-Key-Id:   <device signing key id>
    X-Edge-Token:    <device token>
    X-Edge-Signature: <hex-encoded 64-byte Ed25519 signature>

    <the exact bytes the signature covers>

This is the same shape as webhook signing. It is chosen over embedding the
signature inside a JSON envelope because an in-document signature would have
to be *excluded* from its own coverage — forcing the server to locate and
slice a sub-object out of the raw stream, which is precisely the
re-serialization hazard described above. With headers, the signed bytes are
the request body, byte for byte, with nothing to extract.

The key id is not itself signed. It does not need to be: it selects the trust
anchor, and the accompanying device token must match that anchor's token, so
relabelling a key id without holding its token fails at the token check.

## Batch semantics

``body.snapshots`` carries one or more assessments in ascending
``sequence_id`` order. A single POST usually holds one snapshot; after a
network outage the edge replays its ring buffer, so a batch may legitimately
contain up to :data:`MAX_SNAPSHOTS_PER_BATCH` entries. Duplicate delivery is
expected (the edge retries when a response is lost), which is why ingestion
is idempotent on ``(tenant_id, device_id, network_epoch, sequence_id)``.
"""

from __future__ import annotations

from datetime import datetime
from typing import Any, Final, Literal, Self

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator

#: The version string appears literally in the ``Literal`` aliases below,
#: because ``Literal`` cannot be built from a variable. These aliases are the
#: single place the wire strings are written down.
TelemetrySchemaVersion = Literal["network.edge.telemetry.v1"]
ActionResultsSchemaVersion = Literal["network.edge.action-results.v1"]

NETWORK_TELEMETRY_SCHEMA_VERSION: Final[TelemetrySchemaVersion] = "network.edge.telemetry.v1"
NETWORK_ACTION_RESULTS_SCHEMA_VERSION: Final[ActionResultsSchemaVersion] = (
    "network.edge.action-results.v1"
)

#: One batch is at most the edge ring buffer depth, plus headroom for a
#: snapshot published while a replay is in flight. Bounding this stops a
#: malformed or hostile client from forcing unbounded parse/insert work.
MAX_SNAPSHOTS_PER_BATCH = 32

#: A batch larger than this many bytes is rejected before parsing.
MAX_TELEMETRY_BODY_BYTES = 1024 * 1024

#: Ed25519 signatures are exactly 64 bytes; hex encoding doubles that.
ED25519_SIGNATURE_HEX_LENGTH = 128

#: Health states are the closed set produced by the WeakNet evaluator.
HealthStateLiteral = Literal["GOOD", "DEGRADED", "BAD", "UNKNOWN"]
CoverageLiteral = Literal["FULL_FOR_PROFILE", "PARTIAL", "NONE"]
ApplicabilityLiteral = Literal["APPLICABLE", "NOT_APPLICABLE"]

#: Derived connectivity of the device, not a health verdict: a device can be
#: ``WEAK_NET`` (reachable but degraded) or ``OFFLINE`` (missed heartbeats).
ConnectionStatusLiteral = Literal["ONLINE", "WEAK_NET", "OFFLINE"]

#: Media a snapshot was taken over. Drives which diagnostics are meaningful
#: (RF health is not applicable on a wired link; portal interception is not
#: applicable on a link with no gateway).
LinkTypeLiteral = Literal[
    "WIRED_ETHERNET",
    "WIFI_2_4G",
    "WIFI_5G",
    "WIFI_6G",
    "CELLULAR",
    "UNKNOWN",
]


class _ClosedModel(BaseModel):
    """Reject unknown fields so a newer edge cannot silently mislead the server."""

    model_config = ConfigDict(extra="forbid")


class NetworkEvidenceItem(_ClosedModel):
    """One measured fact backing a service-level expectation result."""

    metric: str = Field(min_length=1, max_length=128)
    value: float
    detail: str = Field(default="", max_length=1024)


class NetworkSleResult(_ClosedModel):
    """A service-level expectation result, mirroring the edge's ``SleResult``.

    ``capability_level_negative`` is carried across the wire because it is
    what licenses a hard veto of internet access. Without it the server could
    not distinguish "this host cannot resolve names" from "one domain
    returned SERVFAIL", and would be free to invent a stronger conclusion
    than the edge actually reached.
    """

    state: HealthStateLiteral
    coverage: CoverageLiteral
    applicability: ApplicabilityLiteral = "APPLICABLE"
    reason: str = Field(default="", max_length=1024)
    capability_level_negative: bool = False
    evidence: list[NetworkEvidenceItem] = Field(default_factory=list, max_length=64)


class NetworkExperienceSnapshot(_ClosedModel):
    """The edge's immutable assessment, schema v2, plus its identity envelope.

    Field names follow ``LegacyAdapter::toExperienceJsonV2`` so the C++ side
    can emit this structure without a translation table that could drift.
    """

    interface: str = Field(min_length=1, max_length=64)
    assessment_profile: str = Field(default="INTERNET_ACCESS", max_length=64)
    overall_state: HealthStateLiteral
    overall_coverage: CoverageLiteral
    display_score: int = Field(ge=0, le=100)
    network_health: dict[str, NetworkSleResult] = Field(default_factory=dict)
    service_health: dict[str, NetworkSleResult] = Field(default_factory=dict)
    warnings: list[str] = Field(default_factory=list, max_length=64)
    primary_issue: str | None = Field(default=None, max_length=512)

    # --- identity / topology at capture time ---------------------------------
    # Collected by the edge because the server cannot observe the device's
    # layer-2 identity. These are what make "which network was it on when it
    # failed" answerable after the fact.
    link_type: LinkTypeLiteral = "UNKNOWN"
    mac_address: str | None = Field(default=None, max_length=32)
    ip_address: str | None = Field(default=None, max_length=64)
    gateway_ip: str | None = Field(default=None, max_length=64)
    dns_servers: list[str] = Field(default_factory=list, max_length=16)
    ap_ssid: str | None = Field(default=None, max_length=128)
    ap_bssid: str | None = Field(default=None, max_length=32)

    # --- edge build/runtime identity -----------------------------------------
    hardware_arch: str = Field(default="", max_length=64)
    os_kernel: str = Field(default="", max_length=128)


class NetworkDeviceTelemetry(_ClosedModel):
    """One assessment published by one edge device."""

    #: Device ids are also used as asset primary keys and as the subject of
    #: the `TenantContext` built for each upload. They therefore must satisfy
    #: the platform-wide boundary identifier charset, which excludes `:`.
    #: Constraining it here means a device cannot choose an id that would be
    #: rejected deeper in (or worse, escape the naming assumptions of) the
    #: tenant boundary.
    device_id: str = Field(
        min_length=1,
        max_length=128,
        pattern=r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$",
    )
    display_name: str | None = Field(default=None, max_length=255)
    location: str | None = Field(default=None, max_length=255)

    #: Monotonic per device. gap-free unless snapshots were dropped from the
    #: ring buffer, which is exactly the condition the server detects.
    sequence_id: int = Field(ge=1)

    #: Increments on every reconnect. A snapshot from a previous epoch must
    #: never be compared against the current one.
    network_epoch: int = Field(ge=0)

    #: Increments on every configuration change. Used to detect that a
    #: conclusion was reached under settings that no longer apply.
    config_generation: int = Field(ge=1)

    captured_at: datetime

    #: Effective edge monitor settings, so a diagnosis can state which
    #: thresholds produced the verdict instead of assuming defaults.
    rtt_interval_seconds: float | None = Field(default=None, gt=0, le=3600)
    rtt_probe_targets: list[str] = Field(default_factory=list, max_length=16)

    snapshot: NetworkExperienceSnapshot

    @field_validator("captured_at")
    @classmethod
    def _require_timezone(cls, value: datetime) -> datetime:
        if value.tzinfo is None:
            raise ValueError("captured_at must carry a timezone")
        return value

    @model_validator(mode="after")
    def _require_consistent_primary_issue(self) -> Self:
        """A healthy verdict must not carry a primary issue, and vice versa.

        This is not cosmetic: ``primary_issue`` is rendered to operators as
        the single root cause. A payload claiming GOOD health while naming a
        root cause would surface a fault that the evaluator did not find.
        """
        stated = self.snapshot.primary_issue
        if self.snapshot.overall_state == "GOOD" and stated:
            raise ValueError("primary_issue is only valid for non-GOOD overall state")
        return self


class NetworkTelemetryBatch(_ClosedModel):
    """The signed payload: one or more snapshots from a single device."""

    schema_version: TelemetrySchemaVersion = NETWORK_TELEMETRY_SCHEMA_VERSION
    snapshots: list[NetworkDeviceTelemetry] = Field(
        min_length=1, max_length=MAX_SNAPSHOTS_PER_BATCH
    )

    @model_validator(mode="after")
    def _require_single_device_and_ascending_sequence(self) -> Self:
        """Reject mixed-device batches and out-of-order replays.

        A batch is signed by exactly one device key, so mixing devices inside
        one signed payload would let a compromised device attribute
        fabricated snapshots to a peer. Ascending order is required so the
        server's watermark comparison stays meaningful.
        """
        device_ids = {item.device_id for item in self.snapshots}
        if len(device_ids) != 1:
            raise ValueError("a signed batch must carry snapshots from exactly one device")

        sequences = [item.sequence_id for item in self.snapshots]
        if sequences != sorted(sequences):
            raise ValueError("snapshots must be ordered by ascending sequence_id")
        if len(set(sequences)) != len(sequences):
            raise ValueError("snapshots must not repeat a sequence_id")
        return self


class NetworkActionOutcome(_ClosedModel):
    """Result of an operator-issued configuration action, reported by the edge.

    Reported on the next upload rather than through a separate push channel,
    so a device that is offline simply reports late instead of losing the
    record.
    """

    action_id: str = Field(min_length=1, max_length=128)
    status: Literal["APPLIED", "REJECTED"]
    detail: str = Field(default="", max_length=1024)
    reported_at: datetime

    @field_validator("reported_at")
    @classmethod
    def _require_timezone(cls, value: datetime) -> datetime:
        if value.tzinfo is None:
            raise ValueError("reported_at must carry a timezone")
        return value


class NetworkActionResults(_ClosedModel):
    """The body of an action-result report from one device."""

    schema_version: ActionResultsSchemaVersion = NETWORK_ACTION_RESULTS_SCHEMA_VERSION
    device_id: str = Field(min_length=1, max_length=128)
    results: list[NetworkActionOutcome] = Field(min_length=1, max_length=MAX_SNAPSHOTS_PER_BATCH)


class NetworkAssetSummary(_ClosedModel):
    """Operator-facing view of one managed edge device."""

    asset_id: str
    tenant_id: str
    display_name: str | None
    location: str | None
    active_iface: str | None
    link_type: LinkTypeLiteral
    ip_address: str | None
    overall_state: HealthStateLiteral
    primary_issue: str | None
    display_score: int
    connection_status: ConnectionStatusLiteral
    last_heartbeat_at: datetime | None
    config_generation: int | None
    network_epoch: int | None


class NetworkAssetDetail(_ClosedModel):
    """Full five-dimension view of one managed edge device."""

    summary: NetworkAssetSummary
    hardware_arch: str | None
    os_kernel: str | None
    mac_address: str | None
    gateway_ip: str | None
    dns_servers: list[str]
    ap_ssid: str | None
    ap_bssid: str | None
    rtt_interval_seconds: float | None
    rtt_probe_targets: list[str]
    latest_experience: dict[str, Any] | None


class NetworkTimelinePoint(_ClosedModel):
    """One assessment on the device timeline."""

    sequence_id: int
    network_epoch: int
    config_generation: int
    captured_at: datetime
    overall_state: HealthStateLiteral
    display_score: int
    primary_issue: str | None

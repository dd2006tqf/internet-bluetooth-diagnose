"""Edge network assurance: WeakNet telemetry ingestion and diagnosis.

Kept separate from :mod:`industrial_ops_agent.assurance`, which governs
security exercises and production sign-off, and from
:mod:`industrial_ops_agent.edge`, which governs server-issued offline
diagnosis packs. This package owns the inbound direction only: unattended
devices reporting their own network assessments.
"""

from industrial_ops_agent.network_assurance.contracts import (
    NETWORK_TELEMETRY_SCHEMA_VERSION,
    NetworkAssetDetail,
    NetworkAssetSummary,
    NetworkDeviceTelemetry,
    NetworkTelemetryBatch,
    NetworkTimelinePoint,
)
from industrial_ops_agent.network_assurance.service import (
    DEFAULT_OFFLINE_AFTER_SECONDS,
    NetworkAssuranceConflict,
    NetworkAssuranceNotFound,
    NetworkAssuranceService,
)
from industrial_ops_agent.network_assurance.signing import (
    DeviceIdentity,
    EdgeTelemetryVerifier,
    NetworkSignatureError,
    load_ed25519_public_key,
)

__all__ = [
    "DEFAULT_OFFLINE_AFTER_SECONDS",
    "NETWORK_TELEMETRY_SCHEMA_VERSION",
    "DeviceIdentity",
    "EdgeTelemetryVerifier",
    "NetworkAssuranceConflict",
    "NetworkAssuranceNotFound",
    "NetworkAssuranceService",
    "NetworkAssetDetail",
    "NetworkAssetSummary",
    "NetworkDeviceTelemetry",
    "NetworkSignatureError",
    "NetworkTelemetryBatch",
    "NetworkTimelinePoint",
    "load_ed25519_public_key",
]

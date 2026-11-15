"""Canonical MinIO object-key construction."""

from __future__ import annotations

from dataclasses import dataclass
from enum import StrEnum

from industrial_ops_agent.persistence.errors import TenantBoundaryViolation
from industrial_ops_agent.persistence.tenant import TenantContext, validate_boundary_identifier


class ObjectZone(StrEnum):
    QUARANTINE = "quarantine"
    CLEAN = "clean"
    GENERATED = "generated"


@dataclass(frozen=True, slots=True)
class TenantObjectKey:
    tenant_id: str
    zone: ObjectZone
    value: str

    def assert_owned_by(self, context: TenantContext) -> None:
        if self.tenant_id != context.tenant_id:
            raise TenantBoundaryViolation("object key is outside the active tenant")


def _object_key(
    context: TenantContext,
    zone: ObjectZone,
    draft_id: str,
    media_id: str,
) -> TenantObjectKey:
    draft_id = validate_boundary_identifier(draft_id, field="draft_id")
    media_id = validate_boundary_identifier(media_id, field="media_id")
    value = f"tenant/{context.tenant_id}/{zone.value}/{draft_id}/{media_id}"
    return TenantObjectKey(tenant_id=context.tenant_id, zone=zone, value=value)


def quarantine_object_key(context: TenantContext, draft_id: str, media_id: str) -> TenantObjectKey:
    return _object_key(context, ObjectZone.QUARANTINE, draft_id, media_id)


def clean_object_key(context: TenantContext, draft_id: str, media_id: str) -> TenantObjectKey:
    return _object_key(context, ObjectZone.CLEAN, draft_id, media_id)


def generated_object_key(
    context: TenantContext, source_id: str, artifact_id: str
) -> TenantObjectKey:
    """Build a tenant-owned key for immutable model-generated media."""

    return _object_key(context, ObjectZone.GENERATED, source_id, artifact_id)


def knowledge_source_object_key(
    context: TenantContext,
    zone: ObjectZone,
    ingestion_id: str,
) -> TenantObjectKey:
    ingestion_id = validate_boundary_identifier(ingestion_id, field="ingestion_id")
    value = f"tenant/{context.tenant_id}/{zone.value}/knowledge/{ingestion_id}/source"
    return TenantObjectKey(tenant_id=context.tenant_id, zone=zone, value=value)


def edge_video_chunk_object_key(
    context: TenantContext,
    clip_id: str,
    chunk_index: int,
) -> TenantObjectKey:
    """Edge-uploaded evidence clip chunk.

    Deliberately **not** reusing ``quarantine_object_key``: that builder's
    middle segment means "incident draft id" and its last segment means
    "media id". Passing a clip id as a draft id would produce keys that
    parse back into a plausible but wrong draft/media pair, so anything
    that later enumerates a draft's media would see these objects as
    members. A separate segment keeps the two namespaces from ever
    being confused.

    The chunk index is zero-padded so that lexicographic key order equals
    numeric chunk order — an operator listing objects in a bucket gets the
    stream in playback order rather than ``0, 1, 10, 11, 2``.
    """

    clip_id = validate_boundary_identifier(clip_id, field="clip_id")
    if chunk_index < 0:
        raise TenantBoundaryViolation("chunk index must not be negative")
    value = (
        f"tenant/{context.tenant_id}/{ObjectZone.QUARANTINE.value}/"
        f"video-clips/{clip_id}/chunk-{chunk_index:05d}"
    )
    return TenantObjectKey(tenant_id=context.tenant_id, zone=ObjectZone.QUARANTINE, value=value)


def tenant_object_key_from_value(
    context: TenantContext,
    value: str,
) -> TenantObjectKey:
    """Rehydrate only an already-canonical key owned by the active tenant."""

    prefix = f"tenant/{context.tenant_id}/"
    if not value.startswith(prefix):
        raise TenantBoundaryViolation("object key is outside the active tenant")
    remainder = value.removeprefix(prefix)
    zone_value, separator, tail = remainder.partition("/")
    if not separator or not tail or "//" in value or value.endswith("/"):
        raise TenantBoundaryViolation("object key is not canonical")
    try:
        zone = ObjectZone(zone_value)
    except ValueError as exc:
        raise TenantBoundaryViolation("object key has an invalid zone") from exc
    key = TenantObjectKey(tenant_id=context.tenant_id, zone=zone, value=value)
    key.assert_owned_by(context)
    return key

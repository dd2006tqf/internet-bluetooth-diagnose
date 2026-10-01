"""Tests for the S6 gateway asset bridge (network asset -> platform asset).

The platform's incident table keys on ``assets.asset_id``. Without this bridge
a WeakNet gateway exists only in ``network_assets``, so nothing downstream
(incidents, work orders) can reference it. The bridge must therefore be:

  * automatic — a gateway that reports appears in the asset pool without an
    operator step, matching how network assets register themselves;
  * idempotent — repeated telemetry must not create duplicate asset rows nor
    keep bumping the row version;
  * non-destructive — fields an operator owns (serial number, lifecycle) are
    never overwritten by telemetry.
"""

from __future__ import annotations

import pytest
from sqlalchemy import create_engine, select
from sqlalchemy.orm import Session
from sqlalchemy.pool import StaticPool

from industrial_ops_agent.network_assurance.contracts import (
    NetworkTelemetryBatch,
)
from industrial_ops_agent.network_assurance.service import NetworkAssuranceService
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import AssetRecord, Base
from industrial_ops_agent.persistence.tenant import TenantContext

DEVICE = "radxa-cubie-a7a"
KEY_ID = "weaknet-edge-telemetry-v1"


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


def _snapshot_json(sequence_id: int) -> dict:
    return {
        "device_id": DEVICE,
        "display_name": "车间网关 A",
        "sequence_id": sequence_id,
        "network_epoch": 7,
        "config_generation": 1,
        "captured_at": "2026-10-01T05:00:00Z",
        "snapshot": {
            "interface": "wlan0",
            "assessment_profile": "INTERNET_ACCESS",
            "overall_state": "GOOD",
            "overall_coverage": "FULL_FOR_PROFILE",
            "display_score": 90,
            "network_health": {},
            "service_health": {},
            "link_type": "WIFI_2_4G",
        },
    }


def _batch(sequence_id: int = 1) -> NetworkTelemetryBatch:
    return NetworkTelemetryBatch.model_validate({"snapshots": [_snapshot_json(sequence_id)]})


def _assets(db: Database) -> list[AssetRecord]:
    with Session(db.engine) as session:  # type: ignore[attr-defined]
        return list(session.scalars(select(AssetRecord)).all())


def test_gateway_appears_in_asset_pool_on_first_telemetry(
    test_db: Database, tenant_context: TenantContext
):
    service = NetworkAssuranceService(database=test_db)
    service.ingest_batch(tenant_context, _batch(), key_id=KEY_ID)

    assets = _assets(test_db)
    assert len(assets) == 1
    asset = assets[0]
    assert asset.asset_id == DEVICE
    assert asset.tenant_id == "tenant-alpha"
    assert asset.source_system == "weaknet"
    assert asset.source_record_id == DEVICE
    assert asset.display_name == "车间网关 A"
    assert asset.model_code == "weaknet-gateway"
    assert asset.lifecycle_status == "IN_SERVICE"


def test_bridge_is_idempotent_and_does_not_churn_version(
    test_db: Database, tenant_context: TenantContext
):
    service = NetworkAssuranceService(database=test_db)
    service.ingest_batch(tenant_context, _batch(1), key_id=KEY_ID)
    first_version = _assets(test_db)[0].version

    # 重复心跳 + 新序号上报都不该造行、也不该无意义地涨版本
    service.ingest_batch(tenant_context, _batch(1), key_id=KEY_ID)
    service.ingest_batch(tenant_context, _batch(2), key_id=KEY_ID)

    assets = _assets(test_db)
    assert len(assets) == 1
    assert assets[0].version == first_version


def test_bridge_preserves_operator_owned_fields(
    test_db: Database, tenant_context: TenantContext
):
    service = NetworkAssuranceService(database=test_db)
    service.ingest_batch(tenant_context, _batch(), key_id=KEY_ID)

    with Session(test_db.engine) as session:  # type: ignore[attr-defined]
        asset = session.scalars(select(AssetRecord)).one()
        asset.serial_number = "SN-OPERATOR-0001"
        asset.lifecycle_status = "MAINTENANCE"
        session.commit()

    service.ingest_batch(tenant_context, _batch(2), key_id=KEY_ID)

    asset = _assets(test_db)[0]
    # 人填的序列号与生命周期状态不被遥测覆盖
    assert asset.serial_number == "SN-OPERATOR-0001"
    assert asset.lifecycle_status == "MAINTENANCE"


def test_two_tenants_do_not_share_bridged_asset(
    test_db: Database, tenant_context: TenantContext
):
    service = NetworkAssuranceService(database=test_db)
    service.ingest_batch(tenant_context, _batch(), key_id=KEY_ID)

    # 第二个租户用同一个 device_id：归属冲突必须 fail-closed（409 语义），
    # 不会在资产池里造出第二行指向同一设备
    from industrial_ops_agent.network_assurance.service import NetworkAssuranceConflict

    other = TenantContext(tenant_id="tenant-beta", subject_id="edge-automation")
    with pytest.raises(NetworkAssuranceConflict):
        service.ingest_batch(other, _batch(), key_id=KEY_ID)

    assert len(_assets(test_db)) == 1

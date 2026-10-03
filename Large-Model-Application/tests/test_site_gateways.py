"""M2 现场聚合查询出口：运维按"现场"看见有哪几台网关。

窗口内的跨网关聚合（M2 诊断侧）发生在 wireless_diagnosis 内部，此前没有任何
REST 入口回答"这个现场有哪几台网关"——本文件钉住 service.list_sites 的语义：

  1. site/gateway 身份来自三张事实表（baselines / events / incidents）的
     并集：板端只在事实上行里携带 site_id，network_assets 行本身没有现场列；
  2. 空 site_id（历史缺省行）不呈现为一个匿名现场；
  3. 网关按 network_assets 做展示层富化（display_name / 连接状态 /
     心跳），未注册的网关仍要出现在清单里，只是富化字段为 None；
  4. 租户隔离：别的租户的现场与网关不可见。
"""

from __future__ import annotations

from datetime import UTC, datetime

import pytest

from industrial_ops_agent.network_assurance.service import NetworkAssuranceService
from industrial_ops_agent.persistence.database import Database
from industrial_ops_agent.persistence.models import (
    Base,
    NetworkAssetRecord,
    NetworkDeviceBaselineRecord,
    NetworkSiteIncidentRecord,
    NetworkWirelessEventRecord,
)
from industrial_ops_agent.persistence.tenant import TenantContext

TENANT = "tenant-alpha"
OTHER_TENANT = "tenant-beta"


@pytest.fixture
def test_db() -> Database:
    from sqlalchemy import create_engine
    from sqlalchemy.pool import StaticPool

    engine = create_engine(
        "sqlite:///:memory:",
        connect_args={"check_same_thread": False},
        poolclass=StaticPool,
    )
    Base.metadata.create_all(engine)
    return Database.from_engine(engine)


@pytest.fixture
def service(test_db: Database) -> NetworkAssuranceService:
    return NetworkAssuranceService(database=test_db)


@pytest.fixture
def tenant() -> TenantContext:
    return TenantContext(tenant_id=TENANT, subject_id="operator-1")


def _asset(db: Database, *, asset_id: str, tenant_id: str = TENANT) -> None:
    with db.transaction(TenantContext(tenant_id=tenant_id, subject_id="seed")) as session:
        session.add(
            NetworkAssetRecord(
                asset_id=asset_id,
                tenant_id=tenant_id,
                display_name=f"gateway-{asset_id}",
                link_type="WIFI_2_4G",
                overall_state="GOOD",
                display_score=90,
                connection_status="ONLINE",
                last_heartbeat_at=datetime.now(UTC),
            )
        )


def _baseline(
    db: Database,
    *,
    baseline_id: str,
    site_id: str,
    gateway_id: str,
    asset_id: str = "radxa-cubie-a7a",
    tenant_id: str = TENANT,
) -> None:
    with db.transaction(TenantContext(tenant_id=tenant_id, subject_id="seed")) as session:
        session.add(
            NetworkDeviceBaselineRecord(
                baseline_id=baseline_id,
                tenant_id=tenant_id,
                asset_id=asset_id,
                site_id=site_id,
                gateway_id=gateway_id,
                device_address="AA:BB:CC:DD:EE:FF",
            )
        )


def _event(
    db: Database,
    *,
    event_id: str,
    site_id: str,
    gateway_id: str,
    tenant_id: str = TENANT,
) -> None:
    with db.transaction(TenantContext(tenant_id=tenant_id, subject_id="seed")) as session:
        session.add(
            NetworkWirelessEventRecord(
                event_id=event_id,
                tenant_id=tenant_id,
                asset_id=gateway_id,
                site_id=site_id,
                gateway_id=gateway_id,
                device_address="AA:BB:CC:DD:EE:FF",
                event_type="LINK_DISCONNECTED",
                ts_ms=1_790_900_000_000,
            )
        )


def _incident(
    db: Database,
    *,
    incident_id: str,
    site_id: str,
    gateway_id: str,
    tenant_id: str = TENANT,
) -> None:
    with db.transaction(TenantContext(tenant_id=tenant_id, subject_id="seed")) as session:
        session.add(
            NetworkSiteIncidentRecord(
                incident_id=incident_id,
                tenant_id=tenant_id,
                asset_id=gateway_id,
                site_id=site_id,
                gateway_id=gateway_id,
                started_at_ms=1_790_900_000_000,
                last_event_ms=1_790_900_000_000,
                state="OPEN",
            )
        )


def test_sites_and_gateways_union_across_fact_tables(
    test_db: Database, service: NetworkAssuranceService, tenant: TenantContext
):
    """三张事实表取并集：只有事件而无基线的网关同样要被看见。"""
    _asset(test_db, asset_id="gw-a")
    _baseline(test_db, baseline_id="b1", site_id="field-1", gateway_id="gw-a")
    # gw-b 只在事件表出现（基线被清理/尚未建立）
    _event(test_db, event_id="ev1", site_id="field-1", gateway_id="gw-b")
    # gw-c 只在事故表出现
    _incident(test_db, incident_id="inc1", site_id="field-1", gateway_id="gw-c")

    sites = service.list_sites(tenant)
    assert [s.site_id for s in sites] == ["field-1"]
    gateways = sites[0].gateways
    assert [g.gateway_id for g in gateways] == ["gw-a", "gw-b", "gw-c"]


def test_gateway_display_enriched_from_assets(
    test_db: Database, service: NetworkAssuranceService, tenant: TenantContext
):
    _asset(test_db, asset_id="gw-a")
    _baseline(test_db, baseline_id="b1", site_id="field-1", gateway_id="gw-a")
    # 未注册为资产的网关：仍出现，富化字段为空
    _baseline(test_db, baseline_id="b2", site_id="field-1", gateway_id="gw-unregistered")

    sites = service.list_sites(tenant)
    by_id = {g.gateway_id: g for g in sites[0].gateways}

    registered = by_id["gw-a"]
    assert registered.display_name == "gateway-gw-a"
    assert registered.connection_status == "ONLINE"
    assert registered.last_heartbeat_at is not None

    unknown = by_id["gw-unregistered"]
    assert unknown.display_name is None
    assert unknown.connection_status is None
    assert unknown.last_heartbeat_at is None


def test_empty_site_id_is_not_presented_as_anonymous_site(
    test_db: Database, service: NetworkAssuranceService, tenant: TenantContext
):
    """历史缺省行 site_id="" 不是现场；不能凭空出现一个匿名分组。"""
    _baseline(test_db, baseline_id="b1", site_id="", gateway_id="gw-a")

    assert service.list_sites(tenant) == []


def test_sites_are_tenant_scoped(
    test_db: Database, service: NetworkAssuranceService, tenant: TenantContext
):
    _baseline(test_db, baseline_id="b1", site_id="field-1", gateway_id="gw-a")
    _baseline(
        test_db,
        baseline_id="b2",
        site_id="secret-field",
        gateway_id="gw-x",
        tenant_id=OTHER_TENANT,
    )

    sites = service.list_sites(tenant)
    assert [s.site_id for s in sites] == ["field-1"]


def test_no_facts_yields_empty(
    test_db: Database, service: NetworkAssuranceService, tenant: TenantContext
):
    assert service.list_sites(tenant) == []


def test_multiple_sites_sorted_by_site_id(
    test_db: Database, service: NetworkAssuranceService, tenant: TenantContext
):
    _baseline(test_db, baseline_id="b1", site_id="field-2", gateway_id="gw-a")
    _baseline(test_db, baseline_id="b2", site_id="field-1", gateway_id="gw-b")

    sites = service.list_sites(tenant)
    assert [s.site_id for s in sites] == ["field-1", "field-2"]

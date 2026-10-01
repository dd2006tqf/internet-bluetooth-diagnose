"""Minimal rehearsal server: the Phase 4a uplink + diagnosis slice only.

Why this exists
---------------
The full platform (`compose.lite.yaml`) brings up ~20 containers (Keycloak,
Vault, MinIO, OPA, Temporal, ClamAV, …) and the **full migration chain**,
which is far more than this cross-project rehearsal needs — and this host has
3 GB of RAM. The rehearsal's question is narrower and sharper:

    does a signed uplink from the board land in the four fact tables, and does
    the diagnosis default path then read them back?

That question needs exactly: this router, a database, the two services, and
the two lookup tables the platform's own service requires (``AssetRecord``'s
FK target ``tenants``). Nothing else is on the path, so nothing else is
started. The full-stack path remains the acceptance environment; this script
is the reachable one, and it says so rather than pretending to be the other.

Usage
-----
    ALEMBIC_DB_URL=... python scripts/rehearsal_server.py
"""

from __future__ import annotations

import logging
import os
import sys
from datetime import UTC, datetime, timedelta
from pathlib import Path

import uvicorn
from fastapi import FastAPI, Request
from sqlalchemy import create_engine
from sqlalchemy.orm import Session
from sqlalchemy.pool import StaticPool

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "src"))

# ``api.dependencies`` imports the entire platform (Presidio/spaCy, MLflow, Kafka,
# graph-RAG, …) because it is the composition root for every route. Only six
# accessors are actually needed by the route module under rehearsal, so this
# shim supplies them from ``app.state`` before the real module is imported.
# The router itself — request parsing, signature verification order, error
# mapping — remains the platform's own code, which is the part worth exercising.
import types  # noqa: E402

# ``industrial_ops_agent.api.__init__`` imports ``create_app``, which pulls in
# every route module. Stand up an empty package object with a real ``__path__``
# so submodule imports still resolve, then substitute the two module-level
# names the pieces we do import would otherwise have to load.
_api_pkg = types.ModuleType("industrial_ops_agent.api")
_api_pkg.__path__ = [str(REPO_ROOT / "src/industrial_ops_agent/api")]  # type: ignore[attr-defined]
sys.modules.setdefault("industrial_ops_agent.api", _api_pkg)

_api_deps = types.ModuleType("industrial_ops_agent.api.dependencies")


def _state_dep(attr: str):
    # FastAPI inspects dependant signatures: an unannotated parameter becomes a
    # *query* parameter, which is exactly the 422 this shim first produced
    # ("missing query.request"). ``Request`` must be importable from *this
    # module's* globals — PEP 563 string annotations are resolved there, not in
    # the function's local scope.
    async def _get(request: Request):
        value = getattr(request.app.state, attr, None)
        if value is None:
            raise RuntimeError(f"rehearsal server missing app.state.{attr}")
        return value

    return _get


_api_deps.get_network_assurance_service = _state_dep("network_assurance_service")
_api_deps.get_edge_telemetry_verifier = _state_dep("edge_telemetry_verifier")
_api_deps.get_wireless_diagnosis_service = _state_dep("wireless_diagnosis_service")
_api_deps.get_network_copilot_service = _state_dep("network_copilot_service")
_api_deps.get_identity = _state_dep("rehearsal_identity")
_api_deps.get_authorizer = _state_dep("authorizer")
_api_deps.AccessTokenVerifier = object
sys.modules.setdefault("industrial_ops_agent.api.dependencies", _api_deps)

from industrial_ops_agent.api.routes.network_assurance import router  # noqa: E402
from industrial_ops_agent.network_assurance.service import (  # noqa: E402
    NetworkAssuranceService,
)
from industrial_ops_agent.network_assurance.wireless_diagnosis import (  # noqa: E402
    WirelessDiagnosisService,
)
from industrial_ops_agent.persistence.database import Database  # noqa: E402
from industrial_ops_agent.persistence.models import (  # noqa: E402
    Base,
    TenantRecord,
)

logging.basicConfig(level=logging.INFO, format="%(levelname)s %(name)s %(message)s")
LOGGER = logging.getLogger("rehearsal")

TENANT_ID = os.environ.get("REHEARSAL_TENANT", "tenant-m1-demo")
#: Rehearsal trust anchor. Generated for this run only; the key material is
#: created next to this script by the rehearsal driver and never committed.
PUBLIC_KEY_PATH = os.environ.get("REHEARSAL_EDGE_PUBLIC_KEY", "")
DEVICE_TOKEN = os.environ.get("REHEARSAL_EDGE_TOKEN", "")
KEY_ID = os.environ.get("REHEARSAL_EDGE_KEY_ID", "weaknet-edge-telemetry-v1")


def _build_database() -> Database:
    url = os.environ.get("ALEMBIC_DB_URL") or os.environ.get("REHEARSAL_DB_URL")
    if url:
        LOGGER.info("using database %s", url.split("@")[-1])
        return Database(url)
    LOGGER.warning("no database URL; falling back to in-memory SQLite (rehearsal only)")
    engine = create_engine(
        "sqlite:///:memory:",
        connect_args={"check_same_thread": False},
        poolclass=StaticPool,
    )
    Base.metadata.create_all(engine)
    return Database.from_engine(engine)


def _ensure_tenant(db: Database) -> None:
    """The platform's ``assets.tenant_id`` is a real FK to ``tenants``.

    A rehearsal database created from the model metadata alone has no tenants,
    and the FK would reject the bridged asset row. Creating the one tenant the
    rehearsal uses keeps the schema honest instead of dropping the constraint.
    """

    with Session(db.engine) as session:  # type: ignore[attr-defined]
        if session.get(TenantRecord, TENANT_ID) is None:
            session.add(
                TenantRecord(
                    id=TENANT_ID,
                    status="active",
                    display_name="Rehearsal tenant",
                    version=1,
                    created_at=datetime.now(UTC),
                    updated_at=datetime.now(UTC),
                )
            )
            session.commit()
            LOGGER.info("seeded tenant %s", TENANT_ID)


def _load_public_key() -> str | None:
    if not PUBLIC_KEY_PATH:
        return None
    path = Path(PUBLIC_KEY_PATH)
    if not path.exists():
        LOGGER.error("public key %s does not exist", path)
        return None
    return path.read_text(encoding="utf-8")


def build_app() -> FastAPI:
    app = FastAPI(title="WeakNet rehearsal slice", version="0.1.0")
    database = _build_database()
    _ensure_tenant(database)

    from industrial_ops_agent.auth.policy import Authorizer  # noqa: PLC0415
    from industrial_ops_agent.security_audit import (  # noqa: PLC0415
        InMemorySecurityAuditSink,
        SecurityAuditor,
    )

    authorizer = Authorizer(
        SecurityAuditor(
            InMemorySecurityAuditSink(),
            hash_key=b"weaknet-rehearsal-audit-key",
        )
    )
    service = NetworkAssuranceService(
        database,
        auto_incident_draft_enabled=False,  # 排练不触发草稿（无主体/无审批链）
        incident_draft_service_factory=None,
    )

    verifier = None
    public_key_pem = _load_public_key()
    if public_key_pem and DEVICE_TOKEN:
        from cryptography.hazmat.primitives.serialization import (  # noqa: PLC0415
            load_pem_public_key,
        )

        from industrial_ops_agent.network_assurance.signing import (  # noqa: PLC0415
            EdgeTelemetryVerifier,
        )

        verifier = EdgeTelemetryVerifier(
            load_pem_public_key(public_key_pem.encode()),
            key_id=KEY_ID,
            device_token=DEVICE_TOKEN,
        )
        LOGGER.info("edge trust anchor loaded (key_id=%s)", KEY_ID)
    else:
        LOGGER.error(
            "no edge trust anchor: set REHEARSAL_EDGE_PUBLIC_KEY and REHEARSAL_EDGE_TOKEN"
        )

    app.state.database = database
    app.state.authorizer = authorizer
    app.state.network_copilot_service = None
    # A real IdentityContext so the operator routes run their real authorization
    # path (the authorizer below is the platform's own policy engine) instead of
    # being stubbed out.
    from industrial_ops_agent.auth.identity import IdentityContext, Role  # noqa: PLC0415

    now = datetime.now(UTC)
    app.state.rehearsal_identity = IdentityContext(
        subject_id="rehearsal-operator",
        oidc_subject="rehearsal-operator",
        tenant_id=TENANT_ID,
        roles=frozenset({Role.AFTER_SALES_ENGINEER}),
        asset_ids=frozenset(),
        site_ids=frozenset(),
        issued_at=now,
        expires_at=now + timedelta(minutes=30),
    )
    app.state.network_assurance_service = service
    app.state.edge_telemetry_verifier = verifier
    app.state.wireless_diagnosis_service = WirelessDiagnosisService(database)

    app.include_router(router, prefix="/api/v1")
    return app


app = build_app()


if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=int(os.environ.get("REHEARSAL_PORT", "8000")))

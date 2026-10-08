"""Server-side edge tenant binding (`_edge_tenant_id`).

The `X-Edge-Tenant` header is device-controlled. These tests pin the three
behaviors of the helper:

1. Configured binding wins; a conflicting header is 403.
2. Configured binding tolerates an absent header (C++ edge always sends it,
   but the value no longer decides).
3. No binding keeps the old header behavior (backward compatibility).

Run with: .venv/bin/python -m pytest tests/test_edge_tenant_binding.py
"""

from __future__ import annotations

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from starlette.requests import Request

from industrial_ops_agent.api.errors import AppError
from industrial_ops_agent.api.routes.network_assurance import _edge_tenant_id
from industrial_ops_agent.network_assurance.signing import EdgeTelemetryVerifier


def _request_with_tenant(tenant: str | None) -> Request:
    headers = {} if tenant is None else {"x-edge-tenant": tenant}
    scope = {
        "type": "http",
        "method": "POST",
        "path": "/edge/telemetry",
        "headers": [
            (k.encode(), v.encode()) for k, v in headers.items()
        ],
        "query_string": b"",
    }
    return Request(scope)


def _verifier(bound_tenant: str | None) -> EdgeTelemetryVerifier:
    # Tenant resolution runs after signature verification and only reads
    # `.tenant_id`, but a real instance keeps the construction contract in
    # the test rather than poking at internals.
    return EdgeTelemetryVerifier(
        Ed25519PrivateKey.generate().public_key(),
        key_id="weaknet-edge-telemetry-v1",
        device_token="test-device-token",
        tenant_id=bound_tenant,
    )


def test_bound_tenant_ignores_absent_header() -> None:
    # Binding configured, header missing → configured tenant decides.
    assert (
        _edge_tenant_id(_request_with_tenant(None), _verifier("tenant-bound"))
        == "tenant-bound"
    )


def test_bound_tenant_accepts_matching_header() -> None:
    assert (
        _edge_tenant_id(
            _request_with_tenant("tenant-bound"), _verifier("tenant-bound")
        )
        == "tenant-bound"
    )


def test_bound_tenant_rejects_conflicting_header() -> None:
    # Device claims another tenant → 403, not silent acceptance.
    try:
        _edge_tenant_id(_request_with_tenant("tenant-evil"), _verifier("tenant-bound"))
    except AppError as exc:
        assert exc.status_code == 403
        assert exc.code == "edge_tenant_mismatch"
        assert exc.retryable is False
    else:
        raise AssertionError("conflicting header must raise AppError")


def test_unbound_tenant_uses_header() -> None:
    # No binding (multi-tenant anchor / rehearsal): header still decides.
    assert (
        _edge_tenant_id(_request_with_tenant("tenant-legacy"), _verifier(None))
        == "tenant-legacy"
    )


def test_unbound_tenant_requires_header() -> None:
    # Old behavior: header required, missing → 400.
    try:
        _edge_tenant_id(_request_with_tenant(None), _verifier(None))
    except AppError as exc:
        assert exc.status_code == 400
        assert exc.code == "edge_tenant_invalid"
    else:
        raise AssertionError("missing header without binding must raise AppError")
"""S1#1 downlink trust: cloud-side signing contract for ``pending_actions``.

The board extracts the exact ``pending_actions`` array bytes out of the
received body and verifies a detached Ed25519 signature against them
(``EdgeTelemetryExporter::verifyPendingActionsSignature``). These tests lock
the byte-level contract that makes that possible:

  1. ``EdgeDownlinkActionSigner.actions_bytes`` equals the compact JSON
     serialization Starlette emits for the response body (separators, unicode);
  2. a signature over those bytes verifies against the board-style substring
     extraction, and fails against a tampered array;
  3. the response model carries optional signature fields (null = legacy
     unprovisioned deployment; boards without ``edge.cloud_public_key_path``
     keep consuming those responses during migration);
  4. the ``get_edge_downlink_signer`` dependency surfaces app state exactly.
"""

from __future__ import annotations

import json

import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

from industrial_ops_agent.network_assurance.signing import (
    EdgeDownlinkActionSigner,
    load_ed25519_private_key,
)

ACTIONS: list[dict[str, object]] = [
    {
        "action_id": "nact-1234567890abcdef",
        "key": "rtt.interval_ms",
        "value": "2000",
        "generation": 7,
        "nonce": "a" * 32,
        "claim_token": "b" * 32,
    }
]


def _signer() -> tuple[EdgeDownlinkActionSigner, Ed25519PrivateKey]:
    private_key = Ed25519PrivateKey.generate()
    pem = private_key.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.PKCS8,
        encryption_algorithm=serialization.NoEncryption(),
    ).decode()
    return EdgeDownlinkActionSigner.from_private_pem(pem, key_id="cloud-downlink-v1"), private_key


def _extract_actions_array(body: str) -> bytes:
    """Board-style substring extraction (see the C++ parser)."""

    key = '"pending_actions"'
    pos = body.find(key)
    assert pos != -1
    start = body.find("[", pos + len(key))
    assert start != -1
    in_string = False
    escaped = False
    for i in range(start + 1, len(body)):
        c = body[i]
        if escaped:
            escaped = False
            continue
        if c == "\\":
            escaped = True
            continue
        if c == '"':
            in_string = not in_string
            continue
        if c == "]" and not in_string:
            return body[start : i + 1].encode()
    raise AssertionError("actions array never closed")


def test_actions_bytes_match_wire_serialization() -> None:
    signer, _ = _signer()
    expected = json.dumps(ACTIONS, ensure_ascii=False, separators=(",", ":")).encode()
    assert EdgeDownlinkActionSigner.actions_bytes(ACTIONS) == expected
    assert signer.actions_bytes(tuple(ACTIONS)) == expected


def test_signature_verifies_against_board_style_substring() -> None:
    signer, private_key = _signer()
    signature_hex, key_id = signer.sign_actions(ACTIONS)

    body = json.dumps(
        {
            "accepted": 1,
            "duplicates": 0,
            "pending_actions_signature": signature_hex,
            "pending_actions_key_id": key_id,
            "pending_actions": ACTIONS,
        },
        ensure_ascii=False,
        separators=(",", ":"),
    )

    array_bytes = _extract_actions_array(body)
    assert array_bytes == EdgeDownlinkActionSigner.actions_bytes(ACTIONS)
    private_key.public_key().verify(bytes.fromhex(signature_hex), array_bytes)


def test_tampered_array_breaks_signature() -> None:
    signer, private_key = _signer()
    signature_hex, key_id = signer.sign_actions(ACTIONS)

    tampered = [dict(ACTIONS[0], value="1")]
    body = json.dumps(
        {
            "accepted": 1,
            "duplicates": 0,
            "pending_actions_signature": signature_hex,
            "pending_actions_key_id": key_id,
            "pending_actions": tampered,
        },
        ensure_ascii=False,
        separators=(",", ":"),
    )

    from cryptography.exceptions import InvalidSignature

    with pytest.raises(InvalidSignature):
        private_key.public_key().verify(
            bytes.fromhex(signature_hex), _extract_actions_array(body)
        )


def test_load_private_key_rejects_non_ed25519() -> None:
    from industrial_ops_agent.network_assurance.signing import NetworkSignatureError

    with pytest.raises(NetworkSignatureError):
        load_ed25519_private_key("not a pem at all")


def test_response_model_signature_fields_default_to_none() -> None:
    from industrial_ops_agent.api.routes.network_assurance import (
        EdgeTelemetryAcceptedResponse,
    )

    legacy = EdgeTelemetryAcceptedResponse(accepted=1, duplicates=0)
    assert legacy.pending_actions_signature is None
    assert legacy.pending_actions_key_id is None
    assert "pending_actions_signature" not in json.dumps(
        legacy.model_dump(include={"accepted", "duplicates", "pending_actions"})
    )


def test_downlink_signer_dependency_reads_app_state() -> None:
    import asyncio
    from types import SimpleNamespace

    from industrial_ops_agent.api.dependencies import get_edge_downlink_signer

    signer, _ = _signer()
    with_signer = SimpleNamespace(
        app=SimpleNamespace(state=SimpleNamespace(edge_downlink_signer=signer))
    )
    assert asyncio.run(get_edge_downlink_signer(with_signer)) is signer

    unprovisioned = SimpleNamespace(app=SimpleNamespace(state=SimpleNamespace()))
    assert asyncio.run(get_edge_downlink_signer(unprovisioned)) is None

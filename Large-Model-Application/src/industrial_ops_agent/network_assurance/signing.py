"""Raw detached-Ed25519 verification for WeakNet edge telemetry.

## Why this is not ``edge/signing.py``

``industrial_ops_agent.edge.signing`` verifies JWS/EdDSA tokens issued *by the
server* to a semi-trusted field runner, and its claim set is bound to that
purpose (issuer, audience, expiry, work-order binding). Reusing it here would
mean an unattended device would have to mint JWT claims — pulling JOSE
base64url encoding and claim lifecycle management into a C++ daemon that has
none of the surrounding machinery.

Instead this module verifies a **detached signature over the exact request
bytes**. See ``network_assurance.contracts`` for why byte-exactness across
languages is the property worth protecting.

## Failure modes are deliberately undifferentiated to the caller

:class:`NetworkSignatureError` carries a machine-readable ``reason`` for logs
and tests, but callers must map every one of them to a single opaque 401
response. Telling an unauthenticated peer *which* part of its credential was
wrong (unknown key id vs bad signature vs missing header) hands it an oracle
for probing key ids.
"""

from __future__ import annotations

import hmac
from dataclasses import dataclass
from hashlib import sha256

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

#: Key ids are operator-assigned labels, not secrets, but they index into the
#: trust anchor so they must be bounded and canonical.
MAX_KEY_ID_LENGTH = 128
MAX_PUBLIC_KEY_BYTES = 16 * 1024

#: Reject absurd device tokens before hashing. The token itself is never
#: logged or stored; only its digest appears in audit records.
MAX_DEVICE_TOKEN_BYTES = 512


class NetworkSignatureError(ValueError):
    """Telemetry could not be attributed to a trusted edge device.

    ``reason`` is for server-side logs and tests only. It must never be
    echoed to the caller; see the module docstring.
    """

    def __init__(self, reason: str) -> None:
        self.reason = reason
        super().__init__(reason)


@dataclass(frozen=True, slots=True)
class DeviceIdentity:
    """The authenticated origin of a telemetry submission."""

    device_id: str
    key_id: str
    #: Digest of the presented device token. Recorded so an operator can tell
    #: "same device, rotated credential" from "different device" without the
    #: server ever holding the token in cleartext.
    token_fingerprint: str


def load_ed25519_public_key(pem: str) -> Ed25519PublicKey:
    """Load the deployment trust anchor.

    Rejects any key type other than Ed25519 so a misconfigured ``RSA`` or
    ``EC`` PEM fails loudly at startup rather than at first telemetry.
    """

    if not pem or len(pem) > MAX_PUBLIC_KEY_BYTES:
        raise NetworkSignatureError("edge_public_key_invalid")
    try:
        loaded = serialization.load_pem_public_key(pem.encode())
    except (TypeError, ValueError) as exc:
        raise NetworkSignatureError("edge_public_key_invalid") from exc
    if not isinstance(loaded, Ed25519PublicKey):
        raise NetworkSignatureError("edge_public_key_must_be_ed25519")
    return loaded


class EdgeTelemetryVerifier:
    """Verify device token and detached signature for one configured key.

    A single verifier instance serves one ``key_id``. Devices are expected to
    be provisioned with distinct keypairs; the server selects the verifier by
    the ``key_id`` presented in the request, so an unknown key id is a
    rejection rather than a fallback to any other key.
    """

    def __init__(
        self,
        public_key: Ed25519PublicKey,
        *,
        key_id: str,
        device_token: str,
    ) -> None:
        if not key_id or len(key_id) > MAX_KEY_ID_LENGTH:
            raise ValueError("network_edge_key_id_invalid")
        if not device_token:
            raise ValueError("network_edge_device_token_required")
        if len(device_token.encode()) > MAX_DEVICE_TOKEN_BYTES:
            raise ValueError("network_edge_device_token_too_long")
        self._public_key = public_key
        self.key_id = key_id
        # Stored as bytes: comparison happens against the raw header value so
        # that a token with trailing whitespace is a mismatch rather than a
        # silently trimmed success.
        self._device_token = device_token.encode()

    @classmethod
    def from_settings(
        cls,
        *,
        public_key_pem: str,
        key_id: str,
        device_token: str,
    ) -> EdgeTelemetryVerifier:
        return cls(
            load_ed25519_public_key(public_key_pem),
            key_id=key_id,
            device_token=device_token,
        )

    def token_fingerprint(self, presented: bytes) -> str:
        """Return a stable, non-reversible label for a presented token."""

        return f"sha256:{sha256(presented).hexdigest()[:32]}"

    def verify(
        self,
        *,
        presented_key_id: str,
        presented_token: bytes,
        signature_hex: str,
        body_bytes: bytes,
        device_id: str,
    ) -> DeviceIdentity:
        """Authenticate one submission.

        Order is deliberate: the cheapest checks run first, and the signature
        is verified against the untouched ``body_bytes``. Nothing here parses
        or re-serializes the body.

        Raises:
            NetworkSignatureError: on any failure. Callers must not
                distinguish the reasons in a response.
        """

        if presented_key_id != self.key_id:
            raise NetworkSignatureError("edge_key_id_unknown")
        if not hmac.compare_digest(presented_token, self._device_token):
            raise NetworkSignatureError("edge_device_token_mismatch")
        if not device_id:
            raise NetworkSignatureError("edge_device_id_missing")

        try:
            signature = bytes.fromhex(signature_hex)
        except ValueError as exc:
            raise NetworkSignatureError("edge_signature_not_hex") from exc
        if len(signature) != 64:
            raise NetworkSignatureError("edge_signature_wrong_length")

        try:
            self._public_key.verify(signature, body_bytes)
        except InvalidSignature as exc:
            raise NetworkSignatureError("edge_signature_invalid") from exc

        return DeviceIdentity(
            device_id=device_id,
            key_id=self.key_id,
            token_fingerprint=self.token_fingerprint(presented_token),
        )

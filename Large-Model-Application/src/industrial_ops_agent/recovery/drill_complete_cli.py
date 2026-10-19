"""CLI for completing an in-flight disaster recovery drill via HTTP API."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any
from urllib.parse import urlparse

import httpx

MAX_PAYLOAD_BYTES = 128 * 1024


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Complete an in-flight recovery drill with component verification results."
    )
    parser.add_argument("--drill-id", required=True, help="ID of the recovery drill to complete")
    parser.add_argument("--version", required=True, type=int, help="Expected drill version (If-Match)")
    parser.add_argument("--results-file", required=True, type=Path, help="JSON file with component_results")
    parser.add_argument("--api-url", required=True, help="Base API URL (e.g. http://127.0.0.1:8000)")
    parser.add_argument("--token-url", required=True, help="OIDC token URL")
    parser.add_argument("--client-id", required=True, help="OIDC client ID")
    parser.add_argument("--client-secret-file", required=True, type=Path, help="File holding client secret")
    parser.add_argument("--ca-file", type=Path, help="Optional custom CA bundle")
    return parser


def run() -> None:
    parser = build_parser()
    args = parser.parse_args()
    results = _load_payload(args.results_file)
    secret = _read_secret(args.client_secret_file)
    verify: bool | str = str(args.ca_file) if args.ca_file is not None else True
    _validate_endpoint(args.api_url)
    _validate_endpoint(args.token_url)

    with httpx.Client(timeout=15.0, verify=verify) as client:
        token_response = client.post(
            args.token_url,
            data={
                "grant_type": "client_credentials",
                "client_id": args.client_id,
                "client_secret": secret,
            },
            headers={"Accept": "application/json"},
        )
        token_response.raise_for_status()
        token = token_response.json()["access_token"]

        endpoint = f"{args.api_url.rstrip('/')}/api/v1/recovery/drills/{args.drill_id}/complete"
        response = client.post(
            endpoint,
            headers={
                "Authorization": f"Bearer {token}",
                "If-Match": str(args.version),
                "Content-Type": "application/json",
            },
            json=results,
        )
        response.raise_for_status()
        drill = response.json().get("data", {})
        print(f"Drill {drill.get('drill_id')} completed: status={drill.get('status')}")


def _validate_endpoint(url: str) -> None:
    parsed = urlparse(url)
    if parsed.scheme not in {"http", "https"}:
        raise SystemExit(f"Unsupported scheme: {url}")
    if parsed.hostname in {"0.0.0.0", "::"}:  # noqa: S104
        raise SystemExit(f"Unsafe target: {url}")


def _read_secret(path: Path) -> str:
    content = path.read_text(encoding="utf-8").strip()
    if not content:
        raise SystemExit("Client secret file is empty")
    return content


def _load_payload(path: Path) -> dict[str, Any]:
    if not path.is_file():
        raise SystemExit(f"File not found: {path}")
    raw = path.read_bytes()
    if len(raw) > MAX_PAYLOAD_BYTES:
        raise SystemExit("Payload file exceeds size limit")
    data = json.loads(raw.decode("utf-8"))
    if not isinstance(data, dict) or "component_results" not in data:
        raise SystemExit("Payload must contain 'component_results' array")
    return data

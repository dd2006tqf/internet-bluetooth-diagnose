#!/usr/bin/env python3
"""Offline telemetry event replay tool.

Replays recorded device/sensor telemetry JSON files to the governed
`POST /api/v1/telemetry/events` ingestion endpoint for diagnostic reproduction.
"""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path
from typing import Any
from urllib.parse import urlparse

import httpx

MAX_EVENT_BYTES = 64 * 1024


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Replay telemetry events from a JSON file into the API."
    )
    parser.add_argument("--file", required=True, type=Path, help="JSON file containing array of events or line-delimited events")
    parser.add_argument("--api-url", default="http://127.0.0.1:8000", help="Base API URL")
    parser.add_argument("--token-url", help="Optional OIDC token URL (if auth is required)")
    parser.add_argument("--client-id", help="OIDC client ID")
    parser.add_argument("--client-secret-file", type=Path, help="OIDC client secret file")
    parser.add_argument("--delay-ms", type=int, default=0, help="Delay between events in milliseconds")
    return parser


def run() -> None:
    parser = build_parser()
    args = parser.parse_args()
    events = _load_events(args.file)
    headers: dict[str, str] = {"Content-Type": "application/json"}

    if args.token_url and args.client_id and args.client_secret_file:
        secret = args.client_secret_file.read_text(encoding="utf-8").strip()
        with httpx.Client(timeout=10.0) as auth_client:
            resp = auth_client.post(
                args.token_url,
                data={
                    "grant_type": "client_credentials",
                    "client_id": args.client_id,
                    "client_secret": secret,
                },
            )
            resp.raise_for_status()
            headers["Authorization"] = f"Bearer {resp.json()['access_token']}"

    endpoint = f"{args.api_url.rstrip('/')}/api/v1/telemetry/events"
    success, failure = 0, 0
    with httpx.Client(timeout=10.0) as client:
        for ev in events:
            try:
                r = client.post(endpoint, headers=headers, json=ev)
                if r.status_code == 201:
                    success += 1
                else:
                    failure += 1
            except httpx.HTTPError:
                failure += 1
            if args.delay_ms > 0:
                time.sleep(args.delay_ms / 1000.0)

    print(f"Replay complete: {success} ingested, {failure} failed (total {len(events)})")


def _load_events(path: Path) -> list[dict[str, Any]]:
    if not path.is_file():
        raise SystemExit(f"File not found: {path}")
    raw = path.read_text(encoding="utf-8").strip()
    if not raw:
        return []
    if raw.startswith("["):
        data = json.loads(raw)
        if not isinstance(data, list):
            raise SystemExit("File must contain JSON array of events")
        return data
    # JSONL fallback
    events = []
    for line in raw.splitlines():
        line = line.strip()
        if line:
            events.append(json.loads(line))
    return events


if __name__ == "__main__":
    run()

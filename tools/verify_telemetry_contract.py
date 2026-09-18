#!/usr/bin/env python3
"""Cross-language contract verifier for WeakNet edge telemetry.

Three checks in one shot:

1. **C++ source-of-truth extraction** — parse
   ``server/include/assurance/edge_telemetry_serializer.hpp`` for every
   ``dump_sle("<key>", ...)`` and collect the keys the emitter actually writes
   into ``network_health``/``service_health``. Also extract the top-level
   ``snapshot.<field>`` names it produces.
2. **Python deserialization** — construct a payload that exercises every
   emitted key and run ``NetworkExperienceSnapshot.model_validate`` on it.
   This catches a C++ rename that the Python whitelist no longer accepts.
3. **Causal-rule coverage** — every emitted SLE key must either be consumed
   by the copilot's ``_causal_chain`` (reachable via ``CAUSAL_CONSUMED_SLE_KEYS``)
   or be explicitly classified non-blocking (``NON_BLOCKING_SLE_KEYS``).
   Uncovered keys fail CI.

The script is intentionally stdlib + pydantic only so it can run in both the
root CI image and the LMA venv without extra provisioning.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parent.parent
SERIALIZER_PATH = REPO_ROOT / "server/include/assurance/edge_telemetry_serializer.hpp"
COPILOT_PATH = (
    REPO_ROOT
    / "Large-Model-Application/src/industrial_ops_agent/network_assurance/copilot.py"
)

sys.path.insert(0, str(REPO_ROOT / "Large-Model-Application/src"))

from industrial_ops_agent.network_assurance.contracts import (  # noqa: E402
    NETWORK_SLE_KEYS,
    SERVICE_SLE_KEYS,
    NetworkExperienceSnapshot,
    NetworkSleResult,
)
from industrial_ops_agent.network_assurance.copilot import (  # noqa: E402
    CAUSAL_CONSUMED_SLE_KEYS,
    NON_BLOCKING_SLE_KEYS,
)


def _extract_sle_keys(serializer_src: str) -> dict[str, set[str]]:
    """Return {'network_health': {...}, 'service_health': {...}} from the emitter.

    The C++ side calls ``dump_sle("<key>", exp.<member>)``. We slice the
    source between the section markers (``"network_health":{`` …
    ``"service_health":{`` … ``"warnings"``) so ``},`` terminators do not
    confuse the state machine. The markers must use the *escaped* form
    ``\\"network_health\\":{`` as they appear inside ``json <<`` literals —
    the doc-comment at the top of the file uses bare quotes and would match
    first otherwise.
    """

    def _slice(start_marker: str, end_marker: str) -> str:
        start = serializer_src.find(start_marker)
        if start == -1:
            return ""
        end = serializer_src.find(end_marker, start + len(start_marker))
        if end == -1:
            return ""
        return serializer_src[start:end]

    pattern = re.compile(r'dump_sle\("([^"]+)"\s*,\s*exp\.([A-Za-z_][A-Za-z0-9_]*)')
    out: dict[str, set[str]] = {"network_health": set(), "service_health": set()}

    network_block = _slice(r'\"network_health\":{', r'\"service_health\":{')
    for m in pattern.finditer(network_block):
        out["network_health"].add(m.group(1))

    service_block = _slice(r'\"service_health\":{', r'\"warnings\"')
    for m in pattern.finditer(service_block):
        out["service_health"].add(m.group(1))

    return out


def _extract_top_level_keys(serializer_src: str) -> set[str]:
    """Pull the ``"<field>":`` literals the snapshot writer emits."""
    return set(re.findall(r'"\\?"([A-Za-z_][A-Za-z0-9_]*)\\?"\s*:', serializer_src))


def _make_sle(state: str = "GOOD", capability_level_negative: bool = False) -> dict[str, Any]:
    return {
        "state": state,
        "coverage": "FULL_FOR_PROFILE",
        "applicability": "APPLICABLE",
        "capability_level_negative": capability_level_negative,
        "reason": "",
        "evidence": [],
    }


def _build_sample_snapshot(network_keys: set[str], service_keys: set[str]) -> dict[str, Any]:
    network_health = {key: _make_sle() for key in sorted(network_keys)}
    service_health = {key: _make_sle() for key in sorted(service_keys)}
    return {
        "interface": "wlan0",
        "assessment_profile": "INTERNET_ACCESS",
        "overall_state": "GOOD",
        "overall_coverage": "FULL_FOR_PROFILE",
        "display_score": 100,
        "network_health": network_health,
        "service_health": service_health,
        "warnings": [],
        "primary_issue": None,
        "link_type": "WIFI_2_4G",
    }


def _check_deserialization(payload: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    try:
        NetworkExperienceSnapshot.model_validate(payload)
    except Exception as exc:  # pydantic.ValidationError
        errors.append(f"NetworkExperienceSnapshot.model_validate failed: {exc}")
    return errors


def _check_key_whitelists(emitted: dict[str, set[str]]) -> list[str]:
    errors: list[str] = []
    for section, keys in emitted.items():
        allowed = NETWORK_SLE_KEYS if section == "network_health" else SERVICE_SLE_KEYS
        unknown = keys - allowed
        if unknown:
            errors.append(
                f"{section}: C++ emits keys the Python whitelist does not accept: {sorted(unknown)}"
            )
    return errors


def _check_causal_coverage(emitted: dict[str, set[str]]) -> list[str]:
    all_emitted = emitted["network_health"] | emitted["service_health"]
    covered = CAUSAL_CONSUMED_SLE_KEYS | NON_BLOCKING_SLE_KEYS
    uncovered = all_emitted - covered
    errors: list[str] = []
    if uncovered:
        errors.append(
            "causal coverage: keys emitted by C++ but neither consumed by "
            f"_causal_chain nor marked non-blocking: {sorted(uncovered)}"
        )
    stale = covered - all_emitted
    if stale:
        errors.append(
            "causal coverage: copilot references SLE keys the C++ emitter does "
            f"not produce: {sorted(stale)}"
        )
    return errors


def _selftest() -> int:
    """Verify the verifier fails when the C++ emitter drifts."""
    fixture = {
        "network_health": {"ip_reachability", "responsiveness", "reliability", "rf_health"},
        "service_health": {
            "dns",
            "tcp_connect",
            "http_access",
            "captive_portal",
            "active_dns",
            "active_tcp",
            "active_https",
            "active_portal",
            "mystery_sle",
        },
    }
    errs = _check_key_whitelists(fixture) + _check_causal_coverage(fixture)
    if not errs:
        print("selftest: expected failure but got none", file=sys.stderr)
        return 1
    print("selftest: verifier correctly flags drift")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--strict",
        action="store_true",
        help="exit non-zero on any failure (CI mode)",
    )
    parser.add_argument(
        "--selftest",
        action="store_true",
        help="run the verifier against a deliberately drifted fixture",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="emit a machine-readable report",
    )
    args = parser.parse_args(argv)

    if args.selftest:
        return _selftest()

    if not SERIALIZER_PATH.exists():
        print(f"error: missing serializer source at {SERIALIZER_PATH}", file=sys.stderr)
        return 2
    if not COPILOT_PATH.exists():
        print(f"error: missing copilot source at {COPILOT_PATH}", file=sys.stderr)
        return 2

    serializer_src = SERIALIZER_PATH.read_text(encoding="utf-8")
    emitted = _extract_sle_keys(serializer_src)
    top_level = _extract_top_level_keys(serializer_src)

    failures: list[str] = []
    failures.extend(_check_key_whitelists(emitted))
    sample = _build_sample_snapshot(emitted["network_health"], emitted["service_health"])
    failures.extend(_check_deserialization(sample))
    failures.extend(_check_causal_coverage(emitted))

    if args.json:
        report = {
            "emitted_sle_keys": {k: sorted(v) for k, v in emitted.items()},
            "top_level_snapshot_fields": sorted(top_level),
            "failures": failures,
        }
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        for line in (
            f"C++ emits {len(emitted['network_health'])} network SLEs: {sorted(emitted['network_health'])}",
            f"C++ emits {len(emitted['service_health'])} service SLEs: {sorted(emitted['service_health'])}",
            f"Python whitelist accepts {len(NETWORK_SLE_KEYS)} network / {len(SERVICE_SLE_KEYS)} service",
        ):
            print(line)
        if failures:
            print("\nFAILURES:")
            for f in failures:
                print(f"  - {f}")

    if failures:
        return 1 if args.strict else 0
    print("OK edge-telemetry-contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

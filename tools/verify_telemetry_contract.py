#!/usr/bin/env python3
"""Cross-language contract verifier for WeakNet edge telemetry.

Four checks in one shot (the fourth covers the Phase 4a wireless uplink):

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

4. **Wireless-fact uplink contract** — the edge's ``buildBody`` emitter
   (``server/src/edge_wireless_uplink_exporter.cpp``) and the cloud's
   ``WirelessEventUplinkBatch`` must agree on the schema version, the four
   group names and every event/incident/baseline field. A rename on either
   side is otherwise only caught by an end-to-end run against the board.

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

WIRELESS_UPLINK_CPP_PATH = (
    REPO_ROOT / "server/src/edge_wireless_uplink_exporter.cpp"
)

sys.path.insert(0, str(REPO_ROOT / "Large-Model-Application/src"))

from industrial_ops_agent.network_assurance.contracts import (  # noqa: E402
    NETWORK_SLE_KEYS,
    SERVICE_SLE_KEYS,
    NetworkExperienceSnapshot,
)
from industrial_ops_agent.network_assurance.copilot import (  # noqa: E402
    CAUSAL_CONSUMED_SLE_KEYS,
    NON_BLOCKING_SLE_KEYS,
)
from industrial_ops_agent.network_assurance.wireless_contracts import (  # noqa: E402
    WIRELESS_EVENT_UPLINK_SCHEMA_VERSION,
    WirelessEventUplinkBatch,
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
        "mac_address": "AA:BB:CC:DD:EE:01",
        "ip_address": "192.168.2.100",
        "gateway_ip": "192.168.2.1",
        "dns_servers": ["223.5.5.5", "114.114.114.114"],
        "ap_ssid": "Test-Workshop-WiFi",
        "ap_bssid": "00:11:22:33:44:55",
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


def _cpp_key(name: str) -> str:
    """C++ 发射器里 JSON 键的写法（源码层带转义反斜杠）。

    源码文本是 ``\\"name\\"``（反斜杠 + 引号），看源码而不是编译产物是为了
    让这条检查在 CI 与本地都不需要先构建。
    """

    return chr(92) + chr(34) + name + chr(92) + chr(34)


def _check_wireless_uplink_contract(cpp_src: str) -> list[str]:
    """C++ 无线事实发射器 vs 云端 pydantic 契约。

    只做**字段名级别的双向核对**（不比较值域）：两侧允许用不同表示
    （板端枚举 toString 与云端 StrEnum 取值相同，各由自己的单测守护）。
    这里要防的是加/改字段只改了一侧——那种漂移在端到端跑板子前完全不可见。
    """

    errors: list[str] = []

    if WIRELESS_EVENT_UPLINK_SCHEMA_VERSION not in cpp_src:
        errors.append(
            "wireless uplink: C++ emitter does not carry the cloud schema "
            f"version {WIRELESS_EVENT_UPLINK_SCHEMA_VERSION!r}"
        )

    for group in ("events", "incidents", "baselines", "env_window"):
        if _cpp_key(group) not in cpp_src:
            errors.append(f"wireless uplink: C++ emitter is missing the '{group}' group")

    # 信封 + 三组事实的全部字段名（云端契约为准，人工维护此清单；
    # 集合漂移会被下面第 4 步的契约一致性断言兜住）。
    expected_fields = (
        "schema_version",
        "device_id",
        "watermark_ms",
        # event
        "event_id",
        "ts_ms",
        "site_id",
        "gateway_id",
        "protocol",
        "device_address",
        "address_type",
        "hci_index",
        "event_type",
        "rssi_at_event_dbm",
        "raw_reason_code",
        "reason",
        "source",
        "source_detail",
        "details_json",
        # incident
        "incident_id",
        "started_at_ms",
        "last_event_ms",
        "resolved_at_ms",
        "affected_devices",
        "state",
        "suspected_cause",
        "evidence_event_ids",
        # baseline
        "baseline_rssi_dbm",
        "min_seen_rssi_dbm",
        "max_seen_rssi_dbm",
        "baseline_sample_count",
    )
    missing = [name for name in expected_fields if _cpp_key(name) not in cpp_src]
    if missing:
        errors.append(
            "wireless uplink: C++ emitter is missing contract fields: " f"{sorted(missing)}"
        )

    # 反向守护：清单里的字段必须真的存在于云端契约（防止清单自己烂掉后
    # 依然"全绿"）。
    events_model = WirelessEventUplinkBatch.model_fields["events"].annotation.__args__[0]
    event_fields = set(events_model.model_fields)
    incident_fields = set(
        WirelessEventUplinkBatch.model_fields["incidents"].annotation.__args__[0].model_fields
    )
    baseline_fields = set(
        WirelessEventUplinkBatch.model_fields["baselines"].annotation.__args__[0].model_fields
    )
    envelope_fields = set(WirelessEventUplinkBatch.model_fields)
    known = event_fields | incident_fields | baseline_fields | envelope_fields
    unknown = [name for name in expected_fields if name not in known]
    if unknown:
        errors.append(
            "wireless uplink: verifier's field list no longer matches the cloud "
            f"contract (typo or removed field): {sorted(unknown)}"
        )

    errors.extend(_check_kernel_snapshot_contract(cpp_src))

    return errors


#: `env_window.snapshots[]` 条目允许的 kind 值。云端该字段是自由 JSON 数组
#: （EnvironmentWindowView.snapshots: list[dict]），没有 pydantic 结构约束，
#: 因此这份白名单是板端"乱写 kind 云端照单全收"的唯一防线。
KERNEL_SNAPSHOT_KINDS = ("process_top", "skb_drop_hist")


def _check_kernel_snapshot_contract(cpp_src: str) -> list[str]:
    """深度内核快照（进程画像 / skb_drop）的 kind 与字段双侧核对。"""

    errors: list[str] = []
    for kind in KERNEL_SNAPSHOT_KINDS:
        if _cpp_key("kind") + ":" + _cpp_key(kind) not in cpp_src:
            errors.append(
                f"kernel snapshot: C++ emitter does not emit the '{kind}' kind "
                "(cloud accepts an untyped JSON array, so this drift is otherwise invisible)"
            )

    # process_top 条目字段
    for field in ("top_processes", "pid", "comm", "tx_bytes", "tx_packets", "retrans_count"):
        if _cpp_key(field) not in cpp_src:
            errors.append(f"kernel snapshot: C++ emitter is missing process field '{field}'")

    # skb_drop_hist 条目字段
    for field in (
        "total_drops",
        "top_reasons",
        "reason_code",
        "reason_name",
        "description",
        "count",
        "last_timestamp_ns",
    ):
        if _cpp_key(field) not in cpp_src:
            errors.append(f"kernel snapshot: C++ emitter is missing drop field '{field}'")

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
    if WIRELESS_UPLINK_CPP_PATH.exists():
        failures.extend(
            _check_wireless_uplink_contract(
                WIRELESS_UPLINK_CPP_PATH.read_text(encoding="utf-8")
            )
        )
    else:
        failures.append(
            f"wireless uplink: missing C++ emitter at {WIRELESS_UPLINK_CPP_PATH}"
        )
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

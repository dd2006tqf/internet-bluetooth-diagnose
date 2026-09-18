"""Unit tests for the deterministic causal chain in the network copilot.

The copilot must mirror ``overall_policy.hpp``'s ordering. The most important
property is R1 — a BAD ``ip_reachability`` is the one-vote veto; every other
SLE can only be a consequence, never the cause.

The C++ emitter writes ``ip_reachability``/``dns``/``tcp_connect``/...; the
copilot previously consulted ``reachability``/``dns_resolution``/``transport``
which never appear on the wire, so R1 could never fire. These tests pin the
mapping to the contract.
"""

from __future__ import annotations

from typing import Any

import pytest

from industrial_ops_agent.network_assurance.copilot import (
    CAUSAL_CONSUMED_SLE_KEYS,
    NON_BLOCKING_SLE_KEYS,
    NetworkCopilotService,
)


def _sle(
    state: str,
    *,
    capability_level_negative: bool = False,
    reason: str = "",
) -> dict[str, Any]:
    return {
        "state": state,
        "capability_level_negative": capability_level_negative,
        "reason": reason,
        "applicability": "APPLICABLE",
        "coverage": "FULL_FOR_PROFILE",
        "evidence": [],
    }


def _snapshot(
    *,
    network_health: dict[str, dict[str, Any]] | None = None,
    service_health: dict[str, dict[str, Any]] | None = None,
    overall_state: str = "GOOD",
) -> dict[str, Any]:
    return {
        "interface": "wlan0",
        "assessment_profile": "INTERNET_ACCESS",
        "overall_state": overall_state,
        "overall_coverage": "FULL_FOR_PROFILE",
        "display_score": 100,
        "network_health": network_health or {},
        "service_health": service_health or {},
        "warnings": [],
        "primary_issue": None,
        "link_type": "WIFI_2_4G",
    }


def _chain(snapshot: dict[str, Any]) -> list[dict[str, str]]:
    svc = NetworkCopilotService(database=None, network_assurance_service=None)
    answer = svc._deterministic_answer("test-asset", snapshot)
    return list(answer.causal_chain)


class TestR1GatewayVeto:
    def test_ip_reachability_bad_short_circuits_everything(self) -> None:
        snapshot = _snapshot(
            network_health={
                "ip_reachability": _sle("BAD", reason="gateway timeout"),
                "responsiveness": _sle("GOOD"),
                "reliability": _sle("GOOD"),
                "rf_health": _sle("GOOD"),
            },
            service_health={
                "dns": _sle("BAD"),
                "tcp_connect": _sle("BAD"),
                "http_access": _sle("BAD"),
                "captive_portal": _sle("BAD"),
            },
            overall_state="BAD",
        )
        chain = _chain(snapshot)
        assert len(chain) == 1
        assert "ip_reachability" in chain[0]["explanation"]
        assert "一票否决" in chain[0]["explanation"]

    def test_ip_reachability_good_falls_through(self) -> None:
        snapshot = _snapshot(
            network_health={
                "ip_reachability": _sle("GOOD"),
                "responsiveness": _sle("GOOD"),
                "reliability": _sle("GOOD"),
                "rf_health": _sle("GOOD"),
            },
            service_health={"dns": _sle("BAD")},
        )
        chain = _chain(snapshot)
        # DNS step should fire — no veto
        assert any("DNS" in step["explanation"] or "dns" in step["explanation"] for step in chain)


class TestPassiveVsCapabilityDns:
    def test_passive_dns_bad_only_advisory(self) -> None:
        snapshot = _snapshot(
            network_health={"ip_reachability": _sle("GOOD")},
            service_health={"dns": _sle("BAD", capability_level_negative=False)},
        )
        chain = _chain(snapshot)
        assert len(chain) >= 1
        assert any("被动 DNS" in step["explanation"] or "被动观测" in step["explanation"] for step in chain)
        # passive DNS is advisory — must not claim host-level capability loss
        assert not any("主机级 DNS" in step["explanation"] for step in chain)

    def test_active_dns_bad_drives_capability_verdict(self) -> None:
        snapshot = _snapshot(
            network_health={"ip_reachability": _sle("GOOD")},
            service_health={
                "active_dns": _sle("BAD", capability_level_negative=True),
            },
        )
        chain = _chain(snapshot)
        assert any("主机级 DNS" in step["explanation"] or "active_dns" in step["explanation"] for step in chain)

    def test_active_dns_bad_without_capability_bit_does_not_veto(self) -> None:
        snapshot = _snapshot(
            network_health={"ip_reachability": _sle("GOOD")},
            service_health={
                "active_dns": _sle("BAD", capability_level_negative=False),
            },
        )
        chain = _chain(snapshot)
        # No capability bit → falls back to advisory, must not say "主机级 DNS"
        assert not any("主机级 DNS" in step["explanation"] for step in chain)


class TestPassiveVsCapabilityTcp:
    def test_passive_tcp_bad_is_non_blocking(self) -> None:
        snapshot = _snapshot(
            network_health={"ip_reachability": _sle("GOOD")},
            service_health={"tcp_connect": _sle("BAD", capability_level_negative=False)},
        )
        chain = _chain(snapshot)
        assert any("非否决" in step["explanation"] or "被动 TCP" in step["explanation"] for step in chain)
        # Must not claim host-level capability loss
        assert not any("主机级 TCP" in step["explanation"] for step in chain)

    def test_active_tcp_bad_drives_capability_verdict(self) -> None:
        snapshot = _snapshot(
            network_health={"ip_reachability": _sle("GOOD")},
            service_health={
                "active_tcp": _sle("BAD", capability_level_negative=True),
            },
        )
        chain = _chain(snapshot)
        assert any("主机级 TCP" in step["explanation"] or "active_tcp" in step["explanation"] for step in chain)


class TestPortalStep:
    def test_captive_portal_bad_produces_portal_step(self) -> None:
        snapshot = _snapshot(
            network_health={"ip_reachability": _sle("GOOD")},
            service_health={"captive_portal": _sle("BAD")},
        )
        chain = _chain(snapshot)
        assert any("portal" in step["explanation"].lower() or "门户" in step["explanation"] for step in chain)


class TestWhitelist:
    def test_missing_optional_active_keys_does_not_raise(self) -> None:
        snapshot = _snapshot(
            network_health={"ip_reachability": _sle("GOOD")},
            service_health={"dns": _sle("GOOD")},
        )
        chain = _chain(snapshot)
        assert chain  # must return at least the "no findings" step

    def test_causal_and_nonblocking_cover_all_emitted_keys(self) -> None:
        # Mirror of what verify_telemetry_contract.py asserts — if C++ adds a
        # key the copilot neither consumes nor classifies, this test fails.
        emitted = {
            "ip_reachability",
            "responsiveness",
            "reliability",
            "rf_health",
            "dns",
            "tcp_connect",
            "http_access",
            "captive_portal",
            "active_dns",
            "active_tcp",
            "active_https",
            "active_portal",
        }
        covered = CAUSAL_CONSUMED_SLE_KEYS | NON_BLOCKING_SLE_KEYS
        assert emitted <= covered, f"emitted keys not covered by copilot: {emitted - covered}"


class TestContractWhitelist:
    """The contract layer must reject unknown SLE keys — see contracts.py."""

    def test_unknown_sle_key_rejected(self) -> None:
        from industrial_ops_agent.network_assurance.contracts import (
            NetworkExperienceSnapshot,
        )

        payload = {
            "interface": "wlan0",
            "assessment_profile": "INTERNET_ACCESS",
            "overall_state": "GOOD",
            "overall_coverage": "FULL_FOR_PROFILE",
            "display_score": 100,
            "network_health": {
                "ip_reachability": {
                    "state": "GOOD",
                    "coverage": "FULL_FOR_PROFILE",
                    "applicability": "APPLICABLE",
                    "capability_level_negative": False,
                    "reason": "",
                    "evidence": [],
                },
                "mystery_sle": {
                    "state": "GOOD",
                    "coverage": "FULL_FOR_PROFILE",
                    "applicability": "APPLICABLE",
                    "capability_level_negative": False,
                    "reason": "",
                    "evidence": [],
                },
            },
            "service_health": {},
            "warnings": [],
            "primary_issue": None,
            "link_type": "WIFI_2_4G",
        }
        with pytest.raises(Exception, match="unknown SLE keys"):
            NetworkExperienceSnapshot.model_validate(payload)

"""N2 cross-language contract test: cloud mirror == edge whitelist.

The cloud needs to know which actions and config keys the edge will actually
accept (E1's "action_id | config_change 必须属于板端白名单"). Keeping that as a
hand-maintained Python list would drift silently, so this test parses the two
C++ sources of truth and compares them field by field.

Sources of truth (edge):
  - ``server/include/assurance/action_registry.hpp`` — ``registerAction({...})``
    blocks: action_id, params (name/type/required/allowed values), executable, argv
  - ``server/src/weaknet_config.cpp`` — ``isTrialableKeyImpl`` whitelist and the
    per-monitor ``applyMonitorParam`` key dispatch

Negative verification: renaming a mirrored action must fail the comparison.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

from industrial_ops_agent.network_assurance.edge_action_catalog import (
    ACTION_CATALOG,
    CONFIG_KEYS,
    EdgeActionSpec,
    catalog_version,
    find_action,
    is_known_config_key,
)

REPO_ROOT = Path(__file__).resolve().parents[2]
ACTION_REGISTRY_HPP = REPO_ROOT / "server/include/assurance/action_registry.hpp"
WEAKNET_CONFIG_CPP = REPO_ROOT / "server/src/weaknet_config.cpp"


# ---------------------------------------------------------------------------
# C++ parsers (deliberately narrow: only what these two files actually write)
# ---------------------------------------------------------------------------


def _parse_register_actions(source: str) -> dict[str, dict[str, object]]:
    """Parse ``registerAction({ "ID", "desc", {params}, "exe", {argv} });`` blocks."""
    actions: dict[str, dict[str, object]] = {}
    # 每个 registerAction({ ... }); —— 用括号配平切出各块
    for match in re.finditer(r"registerAction\(\{", source):
        start = match.end() - 1
        depth = 0
        for idx in range(start, len(source)):
            if source[idx] == "{":
                depth += 1
            elif source[idx] == "}":
                depth -= 1
                if depth == 0:
                    block = source[start : idx + 1]
                    break
        else:
            continue
        action_id_match = re.search(r'"([A-Z_]+)"', block)
        if not action_id_match:
            continue
        action_id = action_id_match.group(1)
        params: list[dict[str, object]] = []
        # {"name", "type", required, {"allowed", "values"}}
        # 类型名可能含数字（ipv4_address），故字符类必须包含 0-9
        for param in re.finditer(
            r'\{"([a-z_]+)",\s*"([a-z0-9_]+)",\s*(true|false),\s*\{([^}]*)\}\}', block
        ):
            allowed = [
                value for value in re.findall(r'"([^"]+)"', param.group(4))
            ]
            params.append(
                {
                    "name": param.group(1),
                    "type": param.group(2),
                    "required": param.group(3) == "true",
                    "allowed_values": allowed,
                }
            )
        actions[action_id] = {"params": params}
    return actions


def _parse_trialable_keys(source: str) -> set[str]:
    """Parse the ``static const std::set<std::string> trialable = {...};`` block."""
    match = re.search(
        r"trialable\s*=\s*\{(.*?)\};", source, re.DOTALL
    )
    if not match:
        raise AssertionError("isTrialableKeyImpl 的 trialable 集合未找到")
    return set(re.findall(r'"([a-z_]+\.[a-z_]+)"', match.group(1)))


def main() -> None:  # pragma: no cover - helper for manual inspection
    print(_parse_register_actions(ACTION_REGISTRY_HPP.read_text(encoding="utf-8")))
    print(_parse_trialable_keys(WEAKNET_CONFIG_CPP.read_text(encoding="utf-8")))


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_action_ids_match_edge_registry_exactly() -> None:
    edge = _parse_register_actions(ACTION_REGISTRY_HPP.read_text(encoding="utf-8"))
    assert edge, "未能从 action_registry.hpp 解析出任何动作（解析器或源码已变）"
    assert set(ACTION_CATALOG) == set(edge), (
        "云端动作镜像与板端 ActionRegistry 不一致："
        f"云端多出 {set(ACTION_CATALOG) - set(edge)}，缺少 {set(edge) - set(ACTION_CATALOG)}"
    )


def test_action_params_match_edge_registry() -> None:
    edge = _parse_register_actions(ACTION_REGISTRY_HPP.read_text(encoding="utf-8"))
    for action_id, spec in ACTION_CATALOG.items():
        edge_params = {p["name"]: p for p in edge[action_id]["params"]}  # type: ignore[index]
        mirror_params = {p.name: p for p in spec.params}
        assert set(edge_params) == set(mirror_params), f"{action_id} 参数名不一致"
        for name, edge_param in edge_params.items():
            mirror = mirror_params[name]
            assert mirror.type == edge_param["type"], f"{action_id}.{name} 类型不一致"
            assert mirror.required == edge_param["required"], f"{action_id}.{name} 必填性不一致"
            assert sorted(mirror.allowed_values) == sorted(
                edge_param["allowed_values"]  # type: ignore[arg-type]
            ), f"{action_id}.{name} 允许值不一致"


def test_config_keys_cover_edge_trialable_whitelist() -> None:
    edge_keys = _parse_trialable_keys(WEAKNET_CONFIG_CPP.read_text(encoding="utf-8"))
    assert edge_keys, "未能从 weaknet_config.cpp 解析出 trialable 白名单"
    missing = edge_keys - set(CONFIG_KEYS)
    assert not missing, f"云端 config 白名单缺少板端可调键：{sorted(missing)}"


def test_config_keys_are_actually_dispatchable_on_edge() -> None:
    """云端声明的每个键都必须在板端 applyMonitorParam 里真实出现（防凭空加键）。"""
    source = WEAKNET_CONFIG_CPP.read_text(encoding="utf-8")
    unknown = [key for key in CONFIG_KEYS if f'"{key}"' not in source]
    assert not unknown, f"云端 config 白名单包含板端不存在的键：{unknown}"


def test_negative_verification_renaming_action_fails_comparison() -> None:
    """负向验证：把镜像里的动作改名，双向核对必须报错。"""
    edge = _parse_register_actions(ACTION_REGISTRY_HPP.read_text(encoding="utf-8"))
    tampered = dict(ACTION_CATALOG)
    victim = next(iter(tampered))
    spec = tampered.pop(victim)
    tampered["RENAMED_ACTION_XYZ"] = spec
    assert set(tampered) != set(edge), "改名后集合必须不同（否则测试失去意义）"


def test_catalog_helpers_behave() -> None:
    assert find_action("CHECK_RESOLVER_CONFIG") is not None
    assert find_action("REBOOT_FACTORY_NETWORK_STACK") is None
    assert is_known_config_key("rtt.interval")
    assert not is_known_config_key("rtt.nonexistent")
    assert len(catalog_version()) == 12


def test_resolver_and_interface_allowed_values_are_the_reviewed_four_and_two() -> None:
    resolver = find_action("PROBE_PUBLIC_RESOLVER")
    assert resolver is not None
    param = next(p for p in resolver.params if p.name == "resolver")
    assert sorted(param.allowed_values) == sorted(
        ["223.5.5.5", "119.29.29.29", "8.8.8.8", "114.114.114.114"]
    )
    iface = find_action("RESTART_NETWORK_INTERFACE")
    assert iface is not None
    param = next(p for p in iface.params if p.name == "interface")
    assert sorted(param.allowed_values) == ["eth0", "wlan0"]


def test_catalog_version_changes_when_content_changes() -> None:
    base = catalog_version()
    mutated = dict(ACTION_CATALOG)
    mutated["EXTRA_ACTION"] = EdgeActionSpec(
        action_id="EXTRA_ACTION", params=(), description="tamper"
    )
    assert catalog_version(mutated) != base


if __name__ == "__main__":  # pragma: no cover
    pytest.main([__file__, "-q"])

"""Reviewed cloud mirror of the edge's executable/parametric whitelist (④ E1).

E1 要求每一条建议都必须命中板端白名单。云端需要知道这份白名单才能校验，
但**手工维护的 Python 列表必然漂移**——因此：

- 本模块是**受审阅的镜像**，内容与板端源码一一对应；
- ``tests/test_edge_action_catalog.py`` 解析板端 ``action_registry.hpp`` 与
  ``weaknet_config.cpp`` 做**双向核对**，改名/增删/改参数都会让它失败。

职责边界（重要）：本模块只回答"**技术上可执行什么**"。
它**不**计算 risk / allowed / approval_required / production policy——那些属于 ⑤。

部署版本漂移：``catalog_version`` 是内容指纹，只能保证"仓库内 Python == 仓库内
C++"。它**不能**保证"云端版本 == 目标网关版本"（网关可能跑旧固件）。因此
``CouncilInput.operational_constraints`` 同时携带云端与网关两侧版本，v1 部署前提
是两者一致，不一致时由 ⑤ 的 Policy 阻断。
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import dataclass
from typing import Final

# ---------------------------------------------------------------------------
# 数据模型
# ---------------------------------------------------------------------------


@dataclass(frozen=True, slots=True)
class EdgeParamSpec:
    """板端 ``ActionParamSpec`` 的镜像。

    ``allowed_values`` 非空即为**闭集白名单**（板端 ``ActionRegistry::validate``
    会逐值精确匹配），因此云端校验必须用同一集合——不能只做类型检查。
    """

    name: str
    type: str
    required: bool
    allowed_values: tuple[str, ...] = ()


@dataclass(frozen=True, slots=True)
class EdgeActionSpec:
    """板端 ``ActionDef`` 的镜像（仅保留云端校验所需字段）。

    刻意不镜像 ``executable`` / ``args_template``：云端永远不构造 argv，
    板端 ``posix_spawn`` 才是执行者；把命令模板复制到云端只会制造漂移面。
    """

    action_id: str
    params: tuple[EdgeParamSpec, ...]
    description: str

    def param(self, name: str) -> EdgeParamSpec | None:
        for spec in self.params:
            if spec.name == name:
                return spec
        return None


# ---------------------------------------------------------------------------
# 镜像内容（必须与板端源码逐字一致，由跨语言测试守护）
# ---------------------------------------------------------------------------

#: 对应 server/include/assurance/action_registry.hpp 的 registerDefaultActions()
ACTION_CATALOG: Final[dict[str, EdgeActionSpec]] = {
    "CHECK_RESOLVER_CONFIG": EdgeActionSpec(
        action_id="CHECK_RESOLVER_CONFIG",
        params=(),
        description="查看本地 /etc/resolv.conf 配置文件",
    ),
    "PROBE_PUBLIC_RESOLVER": EdgeActionSpec(
        action_id="PROBE_PUBLIC_RESOLVER",
        params=(
            EdgeParamSpec(
                name="resolver",
                type="ipv4_address",
                required=True,
                allowed_values=("223.5.5.5", "119.29.29.29", "8.8.8.8", "114.114.114.114"),
            ),
        ),
        description="直连测试公共 DNS 解析器连通性",
    ),
    "INSPECT_DEFAULT_GATEWAY": EdgeActionSpec(
        action_id="INSPECT_DEFAULT_GATEWAY",
        params=(),
        description="检查系统当前默认路由及下一跳网关",
    ),
    "RESTART_NETWORK_INTERFACE": EdgeActionSpec(
        action_id="RESTART_NETWORK_INTERFACE",
        params=(
            EdgeParamSpec(
                name="interface",
                type="string",
                required=True,
                allowed_values=("wlan0", "eth0"),
            ),
        ),
        description="重新刷新本地网络接口状态",
    ),
}


#: 对应 server/src/weaknet_config.cpp 的 isTrialableKeyImpl() 白名单。
#: 只收 TRIAL 安全的运行时可调键（identity / enabled / bpf_obj / active_probe.*
#: 被板端刻意排除——它们要么需重启加载，要么可能让探针失联）。
CONFIG_KEYS: Final[frozenset[str]] = frozenset(
    {
        "rtt.interval_ms",
        "rtt.interval",
        "rtt.timeout_ms",
        "rtt.timeout",
        "rtt.target",
        "rtt.window_size",
        "rtt.window",
        "rssi.interval_ms",
        "rssi.interval",
        "tcp_loss.interval_ms",
        "tcp_loss.interval",
        "traffic.interval_ms",
        "traffic.interval",
        "quality.interval_ms",
        "quality.interval",
        "bluetooth.interval_ms",
        "bluetooth.interval",
        "dns.interval_ms",
        "dns.interval",
        "dns.capture_pages",
        "tcp_connect.interval_ms",
        "tcp_connect.interval",
        "tcp_connect.capture_pages",
        "wifi_loss.interval_ms",
        "wifi_loss.interval",
        "http_latency.interval_ms",
        "http_latency.interval",
        "process_profiler.interval_ms",
        "process_profiler.interval",
        "tcp_retrans.interval_ms",
        "tcp_retrans.interval",
        "tcp_conn.interval_ms",
        "tcp_conn.interval",
        "skb_drop.interval_ms",
        "skb_drop.interval",
        "edge.interval_ms",
        "edge.interval",
        "edge.timeout_ms",
        "edge.timeout",
    }
)


# ---------------------------------------------------------------------------
# 版本指纹
# ---------------------------------------------------------------------------


def _canonical_payload(
    actions: dict[str, EdgeActionSpec] | None = None,
    config_keys: frozenset[str] | None = None,
) -> str:
    actions = ACTION_CATALOG if actions is None else actions
    config_keys = CONFIG_KEYS if config_keys is None else config_keys
    return json.dumps(
        {
            "actions": {
                action_id: {
                    "params": [
                        {
                            "name": p.name,
                            "type": p.type,
                            "required": p.required,
                            "allowed": list(p.allowed_values),
                        }
                        for p in spec.params
                    ]
                }
                for action_id, spec in sorted(actions.items())
            },
            "config_keys": sorted(config_keys),
        },
        sort_keys=True,
        separators=(",", ":"),
    )


def catalog_version(
    actions: dict[str, EdgeActionSpec] | None = None,
    config_keys: frozenset[str] | None = None,
) -> str:
    """内容指纹（12 位）。

    任何白名单增删改都会改变它——这正是把"部署版本漂移"变成可比较事实的手段：
    云端与网关各自报出该指纹，不一致即拒绝执行（v1 由 ⑤ 的 Policy 落地）。
    """
    payload = _canonical_payload(actions, config_keys)
    return hashlib.sha256(payload.encode()).hexdigest()[:12]


TOP_LEVEL_CATALOG_VERSION: Final[str] = catalog_version()


# ---------------------------------------------------------------------------
# 查询辅助（E1 ProposalCatalogValidator 的唯一入口）
# ---------------------------------------------------------------------------


def find_action(action_id: str) -> EdgeActionSpec | None:
    return ACTION_CATALOG.get(action_id)


def is_known_config_key(key: str) -> bool:
    return key in CONFIG_KEYS


def validate_action_params(action_id: str, params: dict[str, str]) -> str | None:
    """校验动作参数是否落在受审阅白名单内。返回 ``None`` 表示合法。

    与板端 ``ActionRegistry::validate`` 同规则：必填齐备 + 闭集取值精确匹配。
    刻意**不**复制板端的 ``inet_pton`` 等类型检查细节——云端只做"是否属于受审阅
    集合"，最终执行前的类型校验仍由板端负责（服务端从不假设自己的检查是终局）。
    """
    spec = find_action(action_id)
    if spec is None:
        return f"unknown action_id: {action_id}"
    known = {p.name for p in spec.params}
    unknown = set(params) - known
    if unknown:
        return f"{action_id}: unknown params {sorted(unknown)}"
    for param in spec.params:
        value = params.get(param.name)
        if value is None or value == "":
            if param.required:
                return f"{action_id}: missing required param {param.name}"
            continue
        if param.allowed_values and value not in param.allowed_values:
            return (
                f"{action_id}.{param.name}: value {value!r} not in reviewed set "
                f"{sorted(param.allowed_values)}"
            )
    return None

# Design

## Overview

把参与定标的 eBPF Map 容量从"BPF 源码编译期常量"改为"运行时按物理内存与配置解算"，
并交付逐 key 驱逐可见性追踪器。全部改动走 `bpf_object__open` → `bpf_object__load`
间隙的 `bpf_map__set_max_entries`，不新增 SLE key、不改 v1 真值表、不改 D-Bus signal 表。

### 关键技术决策

1. **定标框架纯函数化**：`resolveMonitorMaps` 不依赖 libbpf（输入 `MapSizeSpec[]` +
   `MapSizingConfig` + RAM 字节数，输出 `ResolvedMapSize[]`），x86 可直接单测；
   libbpf 侧由 `applyMapSizingPlan` 封装，仅在 `HAVE_LIBBPF` 下生效。
2. **配置键扁平化**：YAML 解析器只支持两层缩进，故用 `map_sizing_mode` /
   `map_sizing_entries` / `map_sizing_ram_budget_bp` 平铺字段，而非嵌套块。
3. **这些键不进 TRIAL 白名单**：TRIAL 语义是"试改，不健康就回滚"，但 BPF map 一旦
   `bpf_object__load` 就无法 resize，回滚只能靠 `RestartMonitor` 重新 load。
   若允许 TRIAL，云端会看到"试改成功"却拿不到新容量，形成成功假象。
4. **缺省行为严格等于 v1**：`ram_bytes=0`、预算为 0、mode 非法 → 全部回落
   `default_entries`（即 v1 编译期常量），保证"不配任何文件时行为零变化"。
5. **驱逐追踪器接入生产扫描路径**：`tcp_retransmit` / `process_net_profiler` /
   `http_latency` / `dns_monitor` 的 `getStats()` 在取得当轮快照后更新各自的
   `PerKeyCounterTracker`，并暴露最新 `PerKeyStats` 供后续观测出口读取。
   这是必需的生产消费者——仅被单测调用的内部 API 属于孤儿接口。
6. **v1 冻结边界**：`eviction_limited` 仅是观测标注，不参与任何 SLE 判定。

### 范围收敛（本变更的显式边界）

以下三项**在本变更内不做**，均已在 proposal 的「非目标」中记录理由：

| 排除项 | 理由 |
|---|---|
| a2dp（`active_sessions`/`bt_traffic`）定标 | 唯一生产者 `bt_monitor` 需透传 plan，涉及 `bt_monitor.hpp/.cpp` |
| flow_rate 在 traffic 插件路径的定标 | `traffic`(order 10) 先于 `process_profiler`(order 20) 加载，plan 到不了首个加载点；涉及 `weaknetmgr.*`、`traffic_analyzer.*` |
| `GetEbpfMapStats` D-Bus 出口（暴露 `PerKeyStats`） | P1 后续增量。本变更内 `PerKeyStats` 由各 monitor 持有并可直接查询方法读取 |
| `MetricId` 驱逐指标发布 / CLI 子命令 | P1 后续增量 |

**流程教训（前一个 change 因此废弃）**：规划期必须保证
「`classification.production` 覆盖每个 task 实际要改的文件」且
「每个 task 的 `Verify` kind 都能构成 RED→GREEN→REGRESSION 闭环」。
本次已逐任务核对这两点。

<!-- autoai:tdd-policy:v1 -->
```json
{
  "schema_version": 1,
  "default": "required",
  "exceptions": [
    {
      "id": "exception-no-target-hardware",
      "category": "unavailable_hardware",
      "task_ids": ["4", "5"],
      "paths": ["server/src/*.cpp", "server/src/monitor_*_plugins.cpp"],
      "reason": "bpf_map__set_max_entries 的真实效果只能在 ARM64 开发板（Radxa Cubie A7A）上以真实内核验证；x86 无 CAP_BPF 与真实内核，且 --plan-check 要求 build/test 证据。纯逻辑（resolveMonitorMaps/PerKeyCounterTracker/配置解析）已在 x86 覆盖",
      "alternative_verify_kinds": ["build", "test"],
      "exit_condition": "x86 test-all 全绿 + ARM64 容器编译通过 + 板上 bpftool map show 与 resolved 值一致"
    }
  ]
}
```
<!-- /autoai:tdd-policy:v1 -->

<!-- autoai:implementation-economy:v2 -->
```json
{
  "schema_version": 2,
  "profile": "small",
  "rationale": "eBPF Map 容量运行时定标框架 + 配置接线 + 逐 key 驱逐可见性追踪器",
  "classification": {
    "production": [
      "server/include/utils/bpf_map_sizing.hpp",
      "server/src/utils/bpf_map_sizing.cpp",
      "server/include/utils/per_key_counter_tracker.hpp",
      "server/include/weaknet_config.hpp",
      "server/src/weaknet_config.cpp",
      "server/src/monitor_ebpf_plugins.cpp",
      "server/src/tcp_retransmit_monitor.cpp",
      "server/include/tcp_retransmit_monitor.hpp",
      "server/src/process_net_profiler.cpp",
      "server/include/process_net_profiler.hpp",
      "server/src/net_traffic.cpp",
      "server/include/net_traffic.h",
      "server/src/http_latency_monitor.cpp",
      "server/include/http_latency_monitor.hpp",
      "server/src/dns_monitor.cpp",
      "server/include/dns_monitor.hpp",
      "server/src/tcp_conn_monitor.cpp",
      "server/include/tcp_conn_monitor.hpp",
      "server/src/skb_drop_monitor.cpp",
      "server/include/skb_drop_monitor.hpp"
    ],
    "tests": [
      "server/test/unit/test_bpf_map_sizing_gtest.cpp",
      "server/test/unit/test_per_key_counter_tracker_gtest.cpp",
      "server/test/unit/test_weaknet_config_gtest.cpp",
      "server/test/CMakeLists.txt"
    ],
    "project_docs": ["config.yaml"],
    "project_tooling": [],
    "examples": [],
    "generated": [],
    "vendor": []
  },
  "thresholds": {
    "production": {
      "added_lines": {"expected": 520, "review_at": 800, "hard_limit": 1200},
      "touched_files": {"expected": 20, "review_at": 26, "hard_limit": 34},
      "new_files": {"expected": 3, "review_at": 5, "hard_limit": 7}
    },
    "tests": {
      "added_lines": {"expected": 420, "review_at": 650, "hard_limit": 900},
      "touched_files": {"expected": 4, "review_at": 6, "hard_limit": 8},
      "new_files": {"expected": 2, "review_at": 4, "hard_limit": 6}
    },
    "project_support": {
      "added_lines": {"expected": 30, "review_at": 60, "hard_limit": 100},
      "new_files": {"expected": 0, "review_at": 1, "hard_limit": 2}
    },
    "generated": {
      "files": {"expected": 0, "review_at": 0, "hard_limit": 0},
      "bytes": {"expected": 0, "review_at": 0, "hard_limit": 0}
    }
  },
  "structural_allowances": {
    "public_contracts": [
      {"id": "contract-001", "name": "map_sizing_* 配置键（扁平三键）", "reason": "对外可配置面扩展，需在 applyMonitorField/applyMonitorParam/serializeMonitorJson 三处一致接入"}
    ],
    "build_targets": [],
    "build_graph_entries": [],
    "distribution_surfaces": [],
    "direct_dependencies": []
  },
  "reuse_decisions": [
    {"id": "reuse-001", "path": "server/include/metrics/metric_normalizer.hpp", "symbol": "CounterNormalizer", "decision": "extend", "reason": "PerKeyCounterTracker 沿用同样的 per-key last-value diff 模式，泛化到任意 key+value map"},
    {"id": "reuse-002", "path": "server/include/ebpf_monitor_metrics.hpp", "symbol": "EbpfMonitorStateSupport", "decision": "reuse", "reason": "监控器状态机与指标追踪原样复用，不新增字段"},
    {"id": "reuse-003", "path": "server/src/weaknet_config.cpp", "symbol": "setMonitorParam + RestartMonitor", "decision": "extend", "reason": "map_sizing 变更复用现有调参+重启链路，不新增热改逻辑"}
  ],
  "obsolete_items": [],
  "exceptions": []
}
```
<!-- /autoai:implementation-economy:v2 -->

## Surface Inventory

本变更新增 3 个生产表面、修改 2 个生产表面：

### Surface: bpf_map_sizing 定标框架（internal_api, added）
- `server/include/utils/bpf_map_sizing.hpp`：`MapSizingConfig` / `MapSizeSpec` /
  `ResolvedMapSize` / `resolveMonitorMaps` / `resolveScopePlan` / `totalPhysicalRamBytes` /
  `applyMapSizingPlan` / `getMapSizingSpecs`
- `server/src/utils/bpf_map_sizing.cpp`：纯逻辑解算 + libbpf 应用层 + 各监控器规格表
- `producer_paths`: 上述两文件
- `consumer_paths`: 6 个 monitor 的 init 路径
- `consumer_kind`: `production_caller`
- `entrypoint`: plugin `start()` → monitor `init()`
- `contract_impact`: `compatible`
- `expected_observation`: 解算结果逐项落在 `[min,max]`；缺省配置下逐一等于 v1 常量

### Surface: PerKeyCounterTracker（internal_api, added）
- `server/include/utils/per_key_counter_tracker.hpp`：header-only 模板追踪器
- `producer_paths`: 该头文件
- `consumer_paths`: `tcp_retransmit` / `process_net_profiler` / `http_latency` / `dns_monitor` 的 `getStats()`
- `consumer_kind`: `production_caller`
- `entrypoint`: monitor `getStats()` 扫描循环
- `contract_impact`: `compatible`
- `expected_observation`: new/disappeared/reset 分类正确；水位与消失量同时满足才置 `eviction_limited`

### Surface: map_sizing 配置键（configuration, added）
- `WeakNetConfig::MapSizingCfg` 嵌入 7 个 eBPF 监控器块；扁平三键
- `producer_paths`: `server/include/weaknet_config.hpp` / `server/src/weaknet_config.cpp` / `config.yaml`
- `consumer_paths`: `server/src/monitor_ebpf_plugins.cpp`、`client/weaknet_cli.cpp`
- `consumer_kind`: `real_entrypoint`
- `entrypoint`: `weaknet-cli set <mon>.map_sizing_*` → `SetMonitorParam` → `RestartMonitor`
- `contract_impact`: `compatible`
- `expected_observation`: 三键可解析、可序列化、非法值被拒；缺省 `auto`/50 保持 v1 行为

### Surface: monitor init 签名扩展（internal_api, modified）
- 6 个 monitor 的 `init(path)` → `init(path, plan)`；`net_traffic.h` 的
  `initForInterface(iface, plan)`
- `producer_paths`: 各 monitor 头文件
- `consumer_paths`: `server/src/monitor_ebpf_plugins.cpp`
- `consumer_kind`: `production_caller`
- `entrypoint`: plugin `start()` → monitor `init()`
- `contract_impact`: `breaking`
- `expected_observation`: 全仓编译通过，无残留旧签名调用

### Surface: open→load 间隙应用容量（internal_api, modified）
- 6 个 monitor 的 `init()` 在 `bpf_object__load` 前调用 `applyMapSizingPlan`
- `producer_paths`: 各 monitor `.cpp`
- `consumer_paths`: 各 monitor `.cpp`
- `consumer_kind`: `production_caller`
- `entrypoint`: monitor `init()`
- `contract_impact`: `compatible`
- `expected_observation`: 板上 `bpftool map show` 的 `max_entries` 与 resolved 值一致

### Surface: config.yaml 分发示例（project_doc, modified）
- 各 eBPF 监控器节新增 `map_sizing_*` 注释示例
- `producer_paths`: `config.yaml`
- `consumer_paths`: `tools/ci.sh`（rsync 分发到板端）
- `consumer_kind`: `deployment`
- `entrypoint`: `weaknet-server` 启动读取 `/etc/weaknet/config.yaml`
- `contract_impact`: `compatible`
- `expected_observation`: 新增示例行可被现有解析器接受，不引入错误

<!-- autoai:integration-completeness:v1 -->
```json
{
  "schema_version": 1,
  "discovery": {
    "compile_commands_path": null,
    "mode": "reviewed_inventory"
  },
  "surfaces": [
    {
      "id": "surface-bpf-map-sizing-framework",
      "kind": "internal_api",
      "name": "bpf_map_sizing 定标框架",
      "change_kind": "added",
      "contract_impact": "compatible",
      "compatibility": null,
      "producer_paths": [
        "server/include/utils/bpf_map_sizing.hpp",
        "server/src/utils/bpf_map_sizing.cpp"
      ],
      "consumer_kind": "production_caller",
      "consumer_paths": [
        "server/src/tcp_retransmit_monitor.cpp",
        "server/src/process_net_profiler.cpp",
        "server/src/net_traffic.cpp",
        "server/src/http_latency_monitor.cpp",
        "server/src/dns_monitor.cpp",
        "server/src/tcp_conn_monitor.cpp",
        "server/src/skb_drop_monitor.cpp"
      ],
      "entrypoint": "plugin start -> monitor init",
      "evidence_contracts": [
        {
          "probe_id": "probe-map-sizing-test-current",
          "kind": "test",
          "role": "current",
          "argv": [
            "scripts/project_command.sh",
            "test-all",
            "--change",
            "ebpf-map-sizing-core",
            "--json"
          ],
          "expected_exit_codes": [
            0
          ],
          "output_contains": "100% tests passed"
        },
        {
          "probe_id": "probe-map-sizing-build-current",
          "kind": "build",
          "role": "current",
          "argv": [
            "scripts/project_command.sh",
            "build-server",
            "--change",
            "ebpf-map-sizing-core",
            "--json"
          ],
          "expected_exit_codes": [
            0
          ],
          "output_contains": "weaknet-dbus-server"
        }
      ],
      "requirement_refs": [
        {
          "spec_path": "specs/weaknet-server/spec.md",
          "operation": "ADDED",
          "requirement": "eBPF Map 容量运行时定标",
          "scenarios": [
            "auto 模式按物理内存分摊预算"
          ]
        },
        {
          "spec_path": "specs/weaknet-server/spec.md",
          "operation": "ADDED",
          "requirement": "eBPF Map 容量运行时定标",
          "scenarios": [
            "fixed 模式直接使用配置条数"
          ]
        },
        {
          "spec_path": "specs/weaknet-server/spec.md",
          "operation": "ADDED",
          "requirement": "eBPF Map 容量运行时定标",
          "scenarios": [
            "非法输入降级到默认"
          ]
        },
        {
          "spec_path": "specs/weaknet-server/spec.md",
          "operation": "ADDED",
          "requirement": "定标在 load 前应用且缺省行为与 v1 一致",
          "scenarios": [
            "open 与 load 之间应用容量"
          ]
        },
        {
          "spec_path": "specs/weaknet-server/spec.md",
          "operation": "ADDED",
          "requirement": "定标在 load 前应用且缺省行为与 v1 一致",
          "scenarios": [
            "未配置时逐一等于 v1 常量"
          ]
        }
      ],
      "task_ids": [
        "1",
        "4"
      ],
      "verify_kinds": [
        "test",
        "build"
      ],
      "task_obligations": [
        {
          "task_id": "1",
          "verify_kinds": [
            "test"
          ],
          "evidence_roles": [
            "current"
          ]
        },
        {
          "task_id": "4",
          "verify_kinds": [
            "build",
            "test"
          ],
          "evidence_roles": [
            "current"
          ]
        }
      ],
      "expected_observation": "x86 gtest 对 auto/fixed/降级全绿；ARM64 容器内 resize 在 load 前生效，板上 bpftool 与 resolved 一致",
      "symbol_identities": null
    },
    {
      "id": "surface-per-key-counter-tracker",
      "kind": "internal_api",
      "name": "PerKeyCounterTracker",
      "change_kind": "added",
      "contract_impact": "compatible",
      "compatibility": null,
      "producer_paths": [
        "server/include/utils/per_key_counter_tracker.hpp"
      ],
      "consumer_kind": "production_caller",
      "consumer_paths": [
        "server/src/tcp_retransmit_monitor.cpp",
        "server/src/process_net_profiler.cpp",
        "server/src/http_latency_monitor.cpp",
        "server/src/dns_monitor.cpp"
      ],
      "entrypoint": "monitor getStats() scan loop",
      "evidence_contracts": [
        {
          "probe_id": "probe-tracker-test-current",
          "kind": "test",
          "role": "current",
          "argv": [
            "scripts/project_command.sh",
            "test-all",
            "--change",
            "ebpf-map-sizing-core",
            "--json"
          ],
          "expected_exit_codes": [
            0
          ],
          "output_contains": "100% tests passed"
        },
        {
          "probe_id": "probe-tracker-build-current",
          "kind": "build",
          "role": "current",
          "argv": [
            "scripts/project_command.sh",
            "build-server",
            "--change",
            "ebpf-map-sizing-core",
            "--json"
          ],
          "expected_exit_codes": [
            0
          ],
          "output_contains": "weaknet-dbus-server"
        }
      ],
      "requirement_refs": [
        {
          "spec_path": "specs/weaknet-server/spec.md",
          "operation": "ADDED",
          "requirement": "eBPF Map 逐 key 驱逐可见性",
          "scenarios": [
            "per-key 回退被识别并剔除"
          ]
        },
        {
          "spec_path": "specs/weaknet-server/spec.md",
          "operation": "ADDED",
          "requirement": "eBPF Map 逐 key 驱逐可见性",
          "scenarios": [
            "水位与消失量触发 eviction_limited"
          ]
        },
        {
          "spec_path": "specs/weaknet-server/spec.md",
          "operation": "ADDED",
          "requirement": "eBPF Map 逐 key 驱逐可见性",
          "scenarios": [
            "单凭高水位不判驱逐受限"
          ]
        },
        {
          "spec_path": "specs/weaknet-server/spec.md",
          "operation": "ADDED",
          "requirement": "eBPF Map 逐 key 驱逐可见性",
          "scenarios": [
            "容量未知时不作水位判定"
          ]
        },
        {
          "spec_path": "specs/weaknet-server/spec.md",
          "operation": "ADDED",
          "requirement": "eBPF Map 逐 key 驱逐可见性",
          "scenarios": [
            "追踪器接入监控器扫描路径"
          ]
        }
      ],
      "task_ids": [
        "2",
        "5"
      ],
      "verify_kinds": [
        "test",
        "build"
      ],
      "task_obligations": [
        {
          "task_id": "2",
          "verify_kinds": [
            "test"
          ],
          "evidence_roles": [
            "current"
          ]
        },
        {
          "task_id": "5",
          "verify_kinds": [
            "build",
            "test"
          ],
          "evidence_roles": [
            "current"
          ]
        }
      ],
      "expected_observation": "四类分类与 eviction_limited 判据全绿（含容量未知与仅高水位两个反例）；四个监控器的 getStats 均更新追踪器",
      "symbol_identities": null
    },
    {
      "id": "surface-map-sizing-config-keys",
      "kind": "configuration",
      "name": "map_sizing 扁平配置三键",
      "change_kind": "added",
      "contract_impact": "compatible",
      "compatibility": null,
      "producer_paths": [
        "server/include/weaknet_config.hpp",
        "server/src/weaknet_config.cpp"
      ],
      "consumer_kind": "real_entrypoint",
      "consumer_paths": [
        "server/src/monitor_ebpf_plugins.cpp",
        "client/weaknet_cli.cpp"
      ],
      "entrypoint": "weaknet-cli set <mon>.map_sizing_* -> SetMonitorParam -> RestartMonitor",
      "evidence_contracts": [
        {
          "probe_id": "probe-config-keys-test-current",
          "kind": "test",
          "role": "current",
          "argv": [
            "scripts/project_command.sh",
            "test-all",
            "--change",
            "ebpf-map-sizing-core",
            "--json"
          ],
          "expected_exit_codes": [
            0
          ],
          "output_contains": "100% tests passed"
        }
      ],
      "requirement_refs": [
        {
          "spec_path": "specs/weaknet-server/spec.md",
          "operation": "ADDED",
          "requirement": "map_sizing 配置键接入运行时调参",
          "scenarios": [
            "扁平键在配置文件中被解析"
          ]
        },
        {
          "spec_path": "specs/weaknet-server/spec.md",
          "operation": "ADDED",
          "requirement": "map_sizing 配置键接入运行时调参",
          "scenarios": [
            "运行时修改 map_sizing 键需 restart 生效"
          ]
        },
        {
          "spec_path": "specs/weaknet-server/spec.md",
          "operation": "ADDED",
          "requirement": "map_sizing 配置键接入运行时调参",
          "scenarios": [
            "非法 mode 被拒绝"
          ]
        }
      ],
      "task_ids": [
        "3"
      ],
      "verify_kinds": [
        "test"
      ],
      "task_obligations": [
        {
          "task_id": "3",
          "verify_kinds": [
            "test"
          ],
          "evidence_roles": [
            "current"
          ]
        }
      ],
      "expected_observation": "三键在文件解析/调参/序列化三处一致；非法值与越界值被拒且保留原值；缺省为 auto/50",
      "symbol_identities": null
    },
    {
      "id": "surface-monitor-init-signature",
      "kind": "internal_api",
      "name": "monitor init(path, plan) 签名扩展",
      "change_kind": "modified",
      "contract_impact": "breaking",
      "compatibility": {
        "old_consumer_paths": [
          "server/src/monitor_ebpf_plugins.cpp"
        ],
        "replacement_consumer_paths": [
          "server/src/monitor_ebpf_plugins.cpp"
        ],
        "replacement_policy": "required",
        "expected_old_result": "旧 init(path) 调用点",
        "migration_path": "plugin start() 同步改为传入 resolveScopePlan 解算的 plan",
        "exit_condition": "全仓编译通过且无残留旧签名调用"
      },
      "producer_paths": [
        "server/include/tcp_retransmit_monitor.hpp",
        "server/include/process_net_profiler.hpp",
        "server/include/net_traffic.h",
        "server/include/http_latency_monitor.hpp",
        "server/include/dns_monitor.hpp",
        "server/include/tcp_conn_monitor.hpp",
        "server/include/skb_drop_monitor.hpp"
      ],
      "consumer_kind": "production_caller",
      "consumer_paths": [
        "server/src/monitor_ebpf_plugins.cpp"
      ],
      "entrypoint": "plugin start -> monitor init",
      "evidence_contracts": [
        {
          "probe_id": "probe-init-sig-test-old",
          "kind": "test",
          "role": "old_consumer",
          "argv": [
            "scripts/project_command.sh",
            "test-all",
            "--change",
            "ebpf-map-sizing-core",
            "--json"
          ],
          "expected_exit_codes": [
            0
          ],
          "output_contains": "100% tests passed"
        },
        {
          "probe_id": "probe-init-sig-test-replacement",
          "kind": "test",
          "role": "replacement_consumer",
          "argv": [
            "scripts/project_command.sh",
            "test-all",
            "--change",
            "ebpf-map-sizing-core",
            "--json"
          ],
          "expected_exit_codes": [
            0
          ],
          "output_contains": "100% tests passed"
        },
        {
          "probe_id": "probe-init-sig-build-old",
          "kind": "build",
          "role": "old_consumer",
          "argv": [
            "scripts/project_command.sh",
            "build-server",
            "--change",
            "ebpf-map-sizing-core",
            "--json"
          ],
          "expected_exit_codes": [
            0
          ],
          "output_contains": "weaknet-dbus-server"
        },
        {
          "probe_id": "probe-init-sig-build-replacement",
          "kind": "build",
          "role": "replacement_consumer",
          "argv": [
            "scripts/project_command.sh",
            "build-server",
            "--change",
            "ebpf-map-sizing-core",
            "--json"
          ],
          "expected_exit_codes": [
            0
          ],
          "output_contains": "weaknet-dbus-server"
        }
      ],
      "requirement_refs": [
        {
          "spec_path": "specs/weaknet-server/spec.md",
          "operation": "ADDED",
          "requirement": "定标在 load 前应用且缺省行为与 v1 一致",
          "scenarios": [
            "open 与 load 之间应用容量"
          ]
        }
      ],
      "task_ids": [
        "4"
      ],
      "verify_kinds": [
        "test",
        "build"
      ],
      "task_obligations": [
        {
          "task_id": "4",
          "verify_kinds": [
            "build",
            "test"
          ],
          "evidence_roles": [
            "old_consumer",
            "replacement_consumer"
          ]
        }
      ],
      "expected_observation": "全仓编译通过，6 个监控器均以新签名被调用",
      "symbol_identities": null
    },
    {
      "id": "surface-config-yaml-sample",
      "kind": "configuration",
      "name": "config.yaml 定标示例",
      "change_kind": "modified",
      "contract_impact": "compatible",
      "compatibility": null,
      "producer_paths": [
        "config.yaml"
      ],
      "consumer_kind": "real_entrypoint",
      "consumer_paths": [
        "server/src/weaknet_config.cpp"
      ],
      "entrypoint": "weaknet-server 启动读取 /etc/weaknet/config.yaml",
      "evidence_contracts": [
        {
          "probe_id": "probe-config-yaml-test-current",
          "kind": "test",
          "role": "current",
          "argv": [
            "scripts/project_command.sh",
            "test-all",
            "--change",
            "ebpf-map-sizing-core",
            "--json"
          ],
          "expected_exit_codes": [
            0
          ],
          "output_contains": "100% tests passed"
        }
      ],
      "requirement_refs": [
        {
          "spec_path": "specs/weaknet-server/spec.md",
          "operation": "ADDED",
          "requirement": "map_sizing 配置键接入运行时调参",
          "scenarios": [
            "扁平键在配置文件中被解析"
          ]
        }
      ],
      "task_ids": [
        "3"
      ],
      "verify_kinds": [
        "test"
      ],
      "task_obligations": [
        {
          "task_id": "3",
          "verify_kinds": [
            "test"
          ],
          "evidence_roles": [
            "current"
          ]
        }
      ],
      "expected_observation": "仓库分发的 config.yaml 中新增的注释示例可被解析器接受，不产生解析错误",
      "symbol_identities": null
    }
  ]
}
```
<!-- /autoai:integration-completeness:v1 -->

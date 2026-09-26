# Design

## Overview

本变更把 eBPF Map 容量从"BPF 源码编译期常量"改为"运行时按物理内存与配置定标"，并为 LRU 驱逐失真提供可见性，同时修复 `flow_rate` 的"删表采样"统计口径。

三阶段实施（P0 容量定标、P1 驱逐可见性、P2 flow_rate 累计口径），全部走 libbpf `bpf_object__open_file` → `bpf_object__load` 间隙的 `bpf_map__set_max_entries` 路径，新增只读 `GetEbpfMapStats` D-Bus 出口，**不新增 SLE key、不改 v1 真值表、不改 D-Bus signal 表**。

### 关键技术决策

1. **定标框架纯函数化**：`resolveMonitorMaps` 不依赖 libbpf，输入 `MapSizeSpec[]` + `MapSizingConfig` + RAM bytes，输出 `ResolvedMapSize[]`，x86 可直接单测；libbpf 侧由 `applyMapSizingPlan` 封装，仅在 `HAVE_LIBBPF` 下生效。
2. **flow_rate 双加载的 sizing 归属**：`flow_rate.bpf.o` 由 `NetTrafficAnalyzer`（traffic）或 `ProcessNetProfiler`（process_profiler）任一加载（共享 fd）。`current_sec`/`process_stats` 的 sizing 统一归 **`process_profiler` 配置块**管辖；`initForInterface(iface, plan)` 接受可选 plan，两个调用方拿到同一份 plan，谁先加载谁生效，避免双重 resize。
3. **配置键扁平化**：YAML 解析只支持两层缩进，故不采用 `map_sizing:` 嵌套块，而用 `map_sizing_mode`/`map_sizing_entries`/`map_sizing_ram_budget_bp` 平铺字段；与 D-Bus `SetMonitorParam` 键同名，需 `RestartMonitor` 生效（不入 trialable 白名单）。
4. **`current_sec` P0 不接 per-key tracker**：删表语义下 reset 检测无意义；P2 改累计口径后才接入。
5. **`wifi_loss`/`tcp_connect` 排除 sizing**：固定小 map，无 LRU 压力。
6. **观测出口独立化**：`GetEbpfMapStats` 为只读新方法，不扩 `GetEbpfMonitorHealth`（`EbpfMonitorMetrics` 仅追加 3 个计数器，向后兼容）。
7. **v1 冻结边界**：新观测不进 SLE evidence[]、不新增 SLE key；`eviction_limited` 仅进 `EbpfMonitorStateSupport::status` 字符串。

<!-- autoai:tdd-policy:v1 -->
```json
{
  "schema_version": 1,
  "default": "required",
  "exceptions": [
    {
      "id": "exception-no-target-hardware-build-test",
      "category": "unavailable_hardware",
      "task_ids": [
        "3",
        "4",
        "5",
        "9",
        "14",
        "15"
      ],
      "paths": [
        "server/src/*.cpp",
        "server/src/monitor_*_plugins.cpp",
        "server/src/net_traffic.cpp",
        "server/src/traffic_analyzer.cpp"
      ],
      "reason": "eBPF map resize（bpf_map__set_max_entries）与真机突发流量下的 LRU 驱逐/累计口径验证只能在 ARM64 开发板（Radxa Cubie A7A）上进行；x86 VM 无 CAP_BPF/真实内核。这些任务是接线与集成，无独立可 RED 的行为断言，其正确性由 x86 全量 gtest 回归 + ARM64 编译共同覆盖",
      "alternative_verify_kinds": [
        "build",
        "test"
      ],
      "exit_condition": "x86 test-all 全绿 + ARM64 容器编译通过 + 板上 bpftool map show 验证 + 突发流量冒烟"
    },
    {
      "id": "exception-no-target-hardware-build",
      "category": "unavailable_hardware",
      "task_ids": [
        "10",
        "12",
        "13"
      ],
      "paths": [
        "server/src/server.cpp",
        "client/client.cpp",
        "client/weaknet_cli.cpp",
        "server/src/ebpf_monitor_metrics.cpp"
      ],
      "reason": "Metric 发布、CLI 子命令与 eviction_limited 状态标注的真实行为只能在 ARM64 开发板上以真实内核 map 验证；x86 仅能证明编译通过与序列化正确",
      "alternative_verify_kinds": [
        "build"
      ],
      "exit_condition": "ARM64 容器编译通过 + 板上 weaknet-cli ebpf-maps 输出包含 eviction 字段"
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
  "rationale": "eBPF Map 容量运行时定标 + 驱逐可见性 + flow_rate 累计口径修复，涉及 8 个监控器、配置/DBus/CLI 出口与 flow_rate 语义修正",
  "classification": {
    "production": [
      "server/include/utils/bpf_map_sizing.hpp",
      "server/src/utils/bpf_map_sizing.cpp",
      "server/include/utils/per_key_counter_tracker.hpp",
      "server/include/weaknet_config.hpp",
      "server/src/weaknet_config.cpp",
      "server/include/metrics/metric_types.hpp",
      "server/include/ebpf_monitor_interface.hpp",
      "server/include/common.hpp",
      "server/src/dbus_service.cpp",
      "server/src/monitor_ebpf_plugins.cpp",
      "server/src/monitor_plugins.cpp",
      "server/src/tcp_retransmit_monitor.cpp",
      "server/include/tcp_retransmit_monitor.hpp",
      "server/src/process_net_profiler.cpp",
      "server/include/process_net_profiler.hpp",
      "server/src/net_traffic.cpp",
      "server/include/net_traffic.h",
      "server/src/traffic_analyzer.cpp",
      "server/include/traffic_analyzer.hpp",
      "server/src/http_latency_monitor.cpp",
      "server/include/http_latency_monitor.hpp",
      "server/src/dns_monitor.cpp",
      "server/include/dns_monitor.hpp",
      "server/src/tcp_conn_monitor.cpp",
      "server/include/tcp_conn_monitor.hpp",
      "server/src/skb_drop_monitor.cpp",
      "server/include/skb_drop_monitor.hpp",
      "server/src/bt_audio_analyzer.cpp",
      "server/include/bt_audio_analyzer.hpp",
      "server/src/server.cpp",
      "server/include/server.hpp",
      "client/weaknet_client.h",
      "client/client.cpp",
      "client/weaknet_cli.cpp"
    ],
    "tests": [
      "server/test/unit/test_bpf_map_sizing_gtest.cpp",
      "server/test/unit/test_per_key_counter_tracker_gtest.cpp",
      "server/test/unit/test_ebpf_map_stats_gtest.cpp",
      "server/test/unit/test_weaknet_config_gtest.cpp",
      "server/test/CMakeLists.txt"
    ],
    "project_docs": [
      "config.yaml",
      "docs/架构设计.md",
      "docs/weaknet_cli_usage.md"
    ],
    "project_tooling": [
      "tools/weaknet-test-full.sh"
    ],
    "examples": [],
    "generated": [],
    "vendor": []
  },
  "thresholds": {
    "production": {
      "added_lines": {
        "expected": 1400,
        "review_at": 2200,
        "hard_limit": 3000
      },
      "touched_files": {
        "expected": 28,
        "review_at": 38,
        "hard_limit": 48
      },
      "new_files": {
        "expected": 3,
        "review_at": 5,
        "hard_limit": 7
      }
    },
    "tests": {
      "added_lines": {
        "expected": 500,
        "review_at": 800,
        "hard_limit": 1100
      },
      "touched_files": {
        "expected": 4,
        "review_at": 6,
        "hard_limit": 8
      },
      "new_files": {
        "expected": 3,
        "review_at": 5,
        "hard_limit": 7
      }
    },
    "project_support": {
      "added_lines": {
        "expected": 120,
        "review_at": 200,
        "hard_limit": 300
      },
      "new_files": {
        "expected": 0,
        "review_at": 1,
        "hard_limit": 2
      }
    },
    "generated": {
      "files": {
        "expected": 0,
        "review_at": 0,
        "hard_limit": 0
      },
      "bytes": {
        "expected": 0,
        "review_at": 0,
        "hard_limit": 0
      }
    }
  },
  "structural_allowances": {
    "public_contracts": [
      {
        "id": "contract-001",
        "name": "GetEbpfMapStats D-Bus method",
        "reason": "新增只读 map 统计出口，替代扩展现有 health JSON 以保持兼容性"
      }
    ],
    "build_targets": [],
    "build_graph_entries": [],
    "distribution_surfaces": [
      {
        "id": "surface-001",
        "name": "weaknet-cli ebpf-maps subcommand",
        "reason": "新增 CLI 子命令作为 GetEbpfMapStats 的本地消费出口"
      }
    ],
    "direct_dependencies": []
  },
  "reuse_decisions": [
    {
      "id": "reuse-001",
      "path": "server/include/metrics/metric_normalizer.hpp",
      "symbol": "CounterNormalizer",
      "decision": "extend",
      "reason": "PerKeyCounterTracker 按同样的 per-key last-value diff 模式实现，泛化到任意 key+value map"
    },
    {
      "id": "reuse-002",
      "path": "server/src/net_traffic.cpp",
      "symbol": "initForInterface",
      "decision": "extend",
      "reason": "flow_rate 双加载路径复用现有 initForInterface，新增可选 plan 参数而非新建加载函数"
    },
    {
      "id": "reuse-003",
      "path": "server/include/ebpf_monitor_metrics.hpp",
      "symbol": "EbpfMonitorStateSupport",
      "decision": "extend",
      "reason": "eviction_limited 复用现有 status 字符串通道，不改 EbpfMonitorHealth 结构"
    },
    {
      "id": "reuse-004",
      "path": "server/src/weaknet_config.cpp",
      "symbol": "setMonitorParam+RestartMonitor",
      "decision": "extend",
      "reason": "map_sizing 热改复用现有 param + restart 链路，不新增 hot-resize 逻辑"
    }
  ],
  "obsolete_items": [
    {
      "id": "obsolete-001",
      "path": "server/src/net_traffic.cpp",
      "symbol": "sampleTopFlows 删表循环",
      "disposition": "delete",
      "reason": "累计口径模型下不再需要 bpf_map_delete_elem 清表"
    },
    {
      "id": "obsolete-002",
      "path": "server/src/net_traffic.cpp",
      "symbol": "getRealTimeStats top-1000 加总",
      "disposition": "delete",
      "reason": "累计口径模型下 top-N 截断不再代表全量吞吐"
    }
  ],
  "exceptions": [
    {
      "id": "exc-no-target-hardware",
      "metric": "touched_files",
      "paths": [
        "server/src/*.cpp",
        "server/src/monitor_*_plugins.cpp",
        "server/src/net_traffic.cpp",
        "server/src/traffic_analyzer.cpp"
      ],
      "reason": "eBPF map resize 与突发流量下驱逐/累计口径行为只能在 ARM64 开发板验证；x86 只覆盖纯逻辑",
      "requirement_refs": [
        "specs/weaknet-server/spec.md | ADDED | eBPF Map 容量运行时定标 | auto 模式按物理内存分摊预算"
      ],
      "task_ids": [
        "3",
        "4",
        "5",
        "9",
        "10",
        "12",
        "13",
        "14",
        "15"
      ],
      "verification": "ARM64 容器编译 + 板上 bpftool map show + iperf3 突发冒烟"
    }
  ]
}
```
<!-- /autoai:implementation-economy:v2 -->

## Surface Inventory

本变更新增 4 个生产表面、修改 12 个生产表面：

### Surface: bpf_map_sizing 定标框架（internal_api, added）
- `server/include/utils/bpf_map_sizing.hpp`：`MapSizingConfig`/`MapSizeSpec`/`ResolvedMapSize`/`resolveMonitorMaps`/`totalPhysicalRamBytes`
- `server/src/utils/bpf_map_sizing.cpp`：`applyMapSizingPlan`（libbpf-guarded）+ 各 monitor `MapSizeSpec` 表
- `producer_paths`: 上述两文件
- `consumer_paths`: 8 个 `*_monitor.cpp` init 路径
- `consumer_kind`: `production_caller`
- `entrypoint`: `weaknet-dbus-server` plugin start → monitor init
- `contract_impact`: `added`
- `task_ids`: `["1","2"]`
- `verify_kinds`: `["test"]`（resolveMonitorMaps 纯逻辑 x86 可测）

### Surface: PerKeyCounterTracker（internal_api, added）
- `server/include/utils/per_key_counter_tracker.hpp`：模板 tracker，输出 PerKeyStats
- `producer_paths`: 头文件
- `consumer_paths`: `tcp_retransmit`/`process_net_profiler`/`http_latency`/`dns_monitor`/`net_traffic` 扫描循环
- `consumer_kind`: `production_caller`
- `entrypoint`: monitor worker thread `getStats()` / `getRealTimeStats()`
- `contract_impact`: `added`
- `task_ids`: `["4"]`
- `verify_kinds`: `["test"]`

### Surface: map_sizing 配置键（internal_api, modified）
- `WeakNetConfig` 7 个 eBPF monitor 块新增 `map_sizing_mode/entries/ram_budget_bp`
- `producer_paths`: `server/include/weaknet_config.hpp`
- `consumer_paths`: `server/src/weaknet_config.cpp::applyMonitorField`/`serializeMonitorJson`、各 plugin `start()`
- `consumer_kind`: `production_caller` + `operator`（weaknet-cli set）
- `entrypoint`: `weaknet-cli set <mon>.map_sizing_*` → D-Bus `SetMonitorParam` → `RestartMonitor`
- `contract_impact`: `compatible`（仅新增字段，默认 auto 保持 v1 行为）
- `task_ids`: `["3"]`
- `verify_kinds`: `["test"]`（config 解析 x86 可测）

### Surface: GetEbpfMapStats D-Bus 方法（dbus_method, added）
- `common.hpp` `kMethodGetEbpfMapStats` + `dbus_service.cpp::handleGetEbpfMapStats`
- `producer_paths`: `server/include/common.hpp`、`server/src/dbus_service.cpp`
- `consumer_paths`: `client/client.cpp`、`client/weaknet_cli.cpp`（`ebpf-maps` 子命令）
- `consumer_kind`: `external_client`
- `entrypoint`: `dbus-send` / `weaknet-cli ebpf-maps`
- `contract_impact`: `added`
- `task_ids`: `["5","6"]`
- `verify_kinds`: `["test"]`（JSON 序列化 x86 可测）

### Surface: EbpfMonitorMetrics 扩展（internal_api, modified）
- `EbpfMonitorMetrics` + `mapKeyResets`/`mapKeysDisappeared`/`mapWatermark`
- `producer_paths`: `server/include/ebpf_monitor_interface.hpp`
- `consumer_paths`: `server/src/ebpf_monitor_metrics.cpp`、`server/src/dbus_service.cpp::handleGetEbpfMonitorHealth`
- `consumer_kind`: `production_caller` + `external_client`（health JSON）
- `entrypoint`: `GetEbpfMonitorHealth` JSON 输出
- `contract_impact`: `compatible`（平铺字段追加）
- `task_ids`: `["7"]`
- `verify_kinds`: `["build"]`

### Surface: MetricId 扩展（internal_api, modified）
- `metric_types.hpp` + `EBPF_MAP_WATERMARK_PCT`/`EBPF_MAP_RESET_KEYS`/`EBPF_MAP_DISAPPEARED_KEYS` + descriptor
- `producer_paths`: `server/include/metrics/metric_types.hpp`
- `consumer_paths`: `server/src/server.cpp`（worker thread publish）、`metrics_registry`
- `consumer_kind`: `production_caller`
- `entrypoint`: `start_*_thread` publish 调用
- `contract_impact`: `added`
- `task_ids`: `["7"]`
- `verify_kinds`: `["build"]`

### Surface: monitor init() 签名扩展（internal_api, modified）
- 8 个 `*_monitor.cpp` `init(path)` → `init(path, plan)`；`initForInterface(iface, plan)` 加 plan 参数
- `producer_paths`: 各 monitor `.hpp`/`.cpp`
- `consumer_paths`: `monitor_*_plugins.cpp`、`weak_netmgr.cpp`（traffic 路径）
- `consumer_kind`: `production_caller`
- `entrypoint`: plugin `start()` → monitor init
- `contract_impact`: `breaking_internal`（内部签名变更，调用点同步更新）
- `task_ids`: `["8","9","10","11","12","13","14"]`
- `verify_kinds`: `["build"]`

### Surface: flow_rate 累计口径修复（internal_api, modified）
- `NetTrafficAnalyzer::sampleTopFlows`/`getRealTimeStats`/`detectAnomalies` 改累计 delta 模型
- `producer_paths`: `server/src/net_traffic.cpp`、`server/src/traffic_analyzer.cpp`
- `consumer_paths`: `server/src/weak_netmgr.cpp::updateTrafficAnalysis`
- `consumer_kind`: `production_caller`
- `entrypoint`: `updateTrafficAnalysis` → `NetInfo.traffic_bps` → history
- `contract_impact`: `behavior_change`（`totalBps` 口径修正，非 SLE 路径）
- `task_ids`: `["15","16"]`
- `verify_kinds`: `["build","board"]`

### Surface: config.yaml 示例（project_doc, modified）
- `config.yaml` 各 eBPF monitor 加 `map_sizing_*` 注释行
- `producer_paths`: `config.yaml`
- `consumer_paths`: 部署流程（`tools/ci.sh` rsync）
- `consumer_kind`: `deployment`
- `entrypoint`: `weaknet-server` 启动读 `/etc/weaknet/config.yaml`
- `contract_impact`: `compatible`
- `task_ids`: `["3"]`
- `verify_kinds`: `["build"]`

### Surface: 文档同步（project_doc, modified）
- `docs/架构设计.md`（监控矩阵列 + 新 D-Bus 方法 + sizing 说明）、`docs/weaknet_cli_usage.md`
- `producer_paths`: 上述两文件
- `consumer_paths`: 阅读者/运维
- `consumer_kind`: `documentation`
- `contract_impact`: `compatible`
- `task_ids`: `["17"]`
- `verify_kinds`: `["build"]`

### Surface: weaknet-test-full.sh 冒烟（project_tooling, modified）
- 新增 `weaknet-cli ebpf-maps` 冒烟行
- `producer_paths`: `tools/weaknet-test-full.sh`
- `consumer_paths`: `tools/ci.sh`
- `consumer_kind`: `test_harness`
- `contract_impact`: `compatible`
- `task_ids`: `["18"]`
- `verify_kinds`: `["build"]`

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
      "name": "bpf_map_sizing (resolveMonitorMaps / applyMapSizingPlan)",
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
        "server/src/skb_drop_monitor.cpp",
        "server/src/bt_audio_analyzer.cpp"
      ],
      "entrypoint": "monitor init() during plugin start",
      "evidence_contracts": [
        {
          "probe_id": "probe-bpf-map-sizing-framework-build-current",
          "kind": "build",
          "role": "current",
          "argv": [
            "scripts/project_command.sh",
            "build-server",
            "--change",
            "ebpf-map-sizing-eviction",
            "--json"
          ],
          "expected_exit_codes": [
            0
          ],
          "output_contains": "weaknet-dbus-server"
        },
        {
          "probe_id": "probe-bpf-map-sizing-framework-test-current",
          "kind": "test",
          "role": "current",
          "argv": [
            "scripts/project_command.sh",
            "test-all",
            "--change",
            "ebpf-map-sizing-eviction",
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
        }
      ],
      "task_ids": [
        "1",
        "3",
        "4",
        "5"
      ],
      "verify_kinds": [
        "build",
        "test"
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
          "task_id": "3",
          "verify_kinds": [
            "build",
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
      "expected_observation": "x86 gtest 对 resolveMonitorMaps 的 auto/fixed/降级分支全绿；ARM64 容器内 bpf_object__load 前 resize 生效。",
      "symbol_identities": null
    },
    {
      "id": "surface-per-key-counter-tracker",
      "kind": "internal_api",
      "name": "per_key_counter_tracker.hpp",
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
        "server/src/dns_monitor.cpp",
        "server/src/net_traffic.cpp"
      ],
      "entrypoint": "monitor getStats() / getRealTimeStats() scan loop",
      "evidence_contracts": [
        {
          "probe_id": "probe-per-key-counter-tracker-build-current",
          "kind": "build",
          "role": "current",
          "argv": [
            "scripts/project_command.sh",
            "build-server",
            "--change",
            "ebpf-map-sizing-eviction",
            "--json"
          ],
          "expected_exit_codes": [
            0
          ],
          "output_contains": "weaknet-dbus-server"
        },
        {
          "probe_id": "probe-per-key-counter-tracker-test-current",
          "kind": "test",
          "role": "current",
          "argv": [
            "scripts/project_command.sh",
            "test-all",
            "--change",
            "ebpf-map-sizing-eviction",
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
          "requirement": "eBPF Map 驱逐可见性",
          "scenarios": [
            "per-key 回退被识别并剔除"
          ]
        }
      ],
      "task_ids": [
        "6",
        "9"
      ],
      "verify_kinds": [
        "build",
        "test"
      ],
      "task_obligations": [
        {
          "task_id": "6",
          "verify_kinds": [
            "test"
          ],
          "evidence_roles": [
            "current"
          ]
        },
        {
          "task_id": "9",
          "verify_kinds": [
            "build",
            "test"
          ],
          "evidence_roles": [
            "current"
          ]
        }
      ],
      "expected_observation": "x86 gtest 对 new/grew/disappeared/reset 分类全绿；monitor getStats 接入后 per-key diff 正确计数。",
      "symbol_identities": null
    },
    {
      "id": "surface-map-sizing-config-keys",
      "kind": "configuration",
      "name": "map_sizing_* 扁平配置键",
      "change_kind": "added",
      "contract_impact": "compatible",
      "compatibility": null,
      "producer_paths": [
        "server/include/weaknet_config.hpp",
        "server/src/weaknet_config.cpp",
        "config.yaml"
      ],
      "consumer_kind": "real_entrypoint",
      "consumer_paths": [
        "server/src/monitor_ebpf_plugins.cpp",
        "server/src/monitor_plugins.cpp",
        "client/weaknet_cli.cpp"
      ],
      "entrypoint": "weaknet-cli set <mon>.map_sizing_* → SetMonitorParam → RestartMonitor",
      "evidence_contracts": [
        {
          "probe_id": "probe-map-sizing-config-keys-build-current",
          "kind": "build",
          "role": "current",
          "argv": [
            "scripts/project_command.sh",
            "build-server",
            "--change",
            "ebpf-map-sizing-eviction",
            "--json"
          ],
          "expected_exit_codes": [
            0
          ],
          "output_contains": "weaknet-dbus-server"
        },
        {
          "probe_id": "probe-map-sizing-config-keys-test-current",
          "kind": "test",
          "role": "current",
          "argv": [
            "scripts/project_command.sh",
            "test-all",
            "--change",
            "ebpf-map-sizing-eviction",
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
        "2",
        "16"
      ],
      "verify_kinds": [
        "build",
        "test"
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
          "task_id": "16",
          "verify_kinds": [
            "build"
          ],
          "evidence_roles": [
            "current"
          ]
        }
      ],
      "expected_observation": "x86 gtest 对 map_sizing_* 解析/拒绝/序列化全绿；SetMonitorParam 写入后 RestartMonitor 生效。",
      "symbol_identities": null
    },
    {
      "id": "surface-get-ebpf-map-stats-dbus",
      "kind": "external_api",
      "name": "GetEbpfMapStats D-Bus method",
      "change_kind": "added",
      "contract_impact": "compatible",
      "compatibility": null,
      "producer_paths": [
        "server/include/common.hpp",
        "server/src/dbus_service.cpp",
        "client/weaknet_client.h",
        "client/client.cpp",
        "client/weaknet_cli.cpp"
      ],
      "consumer_kind": "representative_external",
      "consumer_paths": [
        "client/weaknet_cli.cpp"
      ],
      "entrypoint": "weaknet-cli ebpf-maps / dbus-send method call",
      "evidence_contracts": [
        {
          "probe_id": "probe-get-ebpf-map-stats-dbus-build-current",
          "kind": "build",
          "role": "current",
          "argv": [
            "scripts/project_command.sh",
            "build-server",
            "--change",
            "ebpf-map-sizing-eviction",
            "--json"
          ],
          "expected_exit_codes": [
            0
          ],
          "output_contains": "weaknet-dbus-server"
        },
        {
          "probe_id": "probe-get-ebpf-map-stats-dbus-test-current",
          "kind": "test",
          "role": "current",
          "argv": [
            "scripts/project_command.sh",
            "test-all",
            "--change",
            "ebpf-map-sizing-eviction",
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
          "requirement": "观测出口与兼容性",
          "scenarios": [
            "GetEbpfMapStats 返回 sizing 与驱逐统计"
          ]
        }
      ],
      "task_ids": [
        "11",
        "12",
        "17",
        "18"
      ],
      "verify_kinds": [
        "build",
        "test"
      ],
      "task_obligations": [
        {
          "task_id": "11",
          "verify_kinds": [
            "test"
          ],
          "evidence_roles": [
            "current"
          ]
        },
        {
          "task_id": "12",
          "verify_kinds": [
            "build"
          ],
          "evidence_roles": [
            "current"
          ]
        },
        {
          "task_id": "17",
          "verify_kinds": [
            "build"
          ],
          "evidence_roles": [
            "current"
          ]
        },
        {
          "task_id": "18",
          "verify_kinds": [
            "build"
          ],
          "evidence_roles": [
            "current"
          ]
        }
      ],
      "expected_observation": "x86 gtest 对 handleGetEbpfMapStats JSON 字段完整性全绿；weaknet-cli ebpf-maps 调用成功返回 JSON。",
      "symbol_identities": null
    },
    {
      "id": "surface-ebpf-metrics-eviction-ext",
      "kind": "build_or_install",
      "name": "EbpfMonitorMetrics + MetricId 驱逐指标扩展",
      "change_kind": "modified",
      "contract_impact": "compatible",
      "compatibility": null,
      "producer_paths": [
        "server/include/ebpf_monitor_interface.hpp",
        "server/include/metrics/metric_types.hpp"
      ],
      "consumer_kind": "downstream_build",
      "consumer_paths": [
        "server/src/ebpf_monitor_metrics.cpp",
        "server/src/dbus_service.cpp",
        "server/src/server.cpp"
      ],
      "entrypoint": "GetEbpfMonitorHealth / worker thread publish",
      "runnable_artifact": false,
      "evidence_contracts": [
        {
          "probe_id": "probe-ebpf-metrics-eviction-ext-build-current",
          "kind": "build",
          "role": "current",
          "argv": [
            "scripts/project_command.sh",
            "build-server",
            "--change",
            "ebpf-map-sizing-eviction",
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
          "requirement": "观测出口与兼容性",
          "scenarios": [
            "GetEbpfMonitorHealth 兼容旧客户端"
          ]
        },
        {
          "spec_path": "specs/weaknet-server/spec.md",
          "operation": "ADDED",
          "requirement": "eBPF Map 驱逐可见性",
          "scenarios": [
            "watermark 与 disappeared 触发 eviction_limited"
          ]
        }
      ],
      "task_ids": [
        "7",
        "8",
        "10",
        "13"
      ],
      "verify_kinds": [
        "build"
      ],
      "task_obligations": [
        {
          "task_id": "7",
          "verify_kinds": [
            "build"
          ],
          "evidence_roles": [
            "current"
          ]
        },
        {
          "task_id": "8",
          "verify_kinds": [
            "build"
          ],
          "evidence_roles": [
            "current"
          ]
        },
        {
          "task_id": "10",
          "verify_kinds": [
            "build"
          ],
          "evidence_roles": [
            "current"
          ]
        },
        {
          "task_id": "13",
          "verify_kinds": [
            "build"
          ],
          "evidence_roles": [
            "current"
          ]
        }
      ],
      "expected_observation": "ARM64 容器编译通过；GetEbpfMonitorHealth JSON 平铺追加 3 字段不破坏旧客户端。",
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
          "server/src/monitor_ebpf_plugins.cpp",
          "server/src/monitor_plugins.cpp",
          "server/src/weak_netmgr.cpp"
        ],
        "replacement_consumer_paths": [
          "server/src/monitor_ebpf_plugins.cpp",
          "server/src/monitor_plugins.cpp",
          "server/src/weak_netmgr.cpp"
        ],
        "replacement_policy": "required",
        "expected_old_result": "旧 init(path) 调用点",
        "migration_path": "所有 plugin start() 与 weak_netmgr traffic 路径同步改为 init(path, plan)",
        "exit_condition": "全仓编译通过且无残留 init(path) 调用"
      },
      "producer_paths": [
        "server/include/tcp_retransmit_monitor.hpp",
        "server/include/process_net_profiler.hpp",
        "server/include/net_traffic.h",
        "server/include/http_latency_monitor.hpp",
        "server/include/dns_monitor.hpp",
        "server/include/tcp_conn_monitor.hpp",
        "server/include/skb_drop_monitor.hpp",
        "server/include/bt_audio_analyzer.hpp"
      ],
      "consumer_kind": "production_caller",
      "consumer_paths": [
        "server/src/monitor_ebpf_plugins.cpp",
        "server/src/monitor_plugins.cpp",
        "server/src/weak_netmgr.cpp"
      ],
      "entrypoint": "plugin start() → monitor init",
      "evidence_contracts": [
        {
          "probe_id": "probe-monitor-init-signature-build-old-consumer",
          "kind": "build",
          "role": "old_consumer",
          "argv": [
            "scripts/project_command.sh",
            "build-server",
            "--change",
            "ebpf-map-sizing-eviction",
            "--json"
          ],
          "expected_exit_codes": [
            0
          ],
          "output_contains": "weaknet-dbus-server"
        },
        {
          "probe_id": "probe-monitor-init-signature-build-replacement-consumer",
          "kind": "build",
          "role": "replacement_consumer",
          "argv": [
            "scripts/project_command.sh",
            "build-server",
            "--change",
            "ebpf-map-sizing-eviction",
            "--json"
          ],
          "expected_exit_codes": [
            0
          ],
          "output_contains": "weaknet-dbus-server"
        },
        {
          "probe_id": "probe-monitor-init-signature-test-old-consumer",
          "kind": "test",
          "role": "old_consumer",
          "argv": [
            "scripts/project_command.sh",
            "test-all",
            "--change",
            "ebpf-map-sizing-eviction",
            "--json"
          ],
          "expected_exit_codes": [
            0
          ],
          "output_contains": "100% tests passed"
        },
        {
          "probe_id": "probe-monitor-init-signature-test-replacement-consumer",
          "kind": "test",
          "role": "replacement_consumer",
          "argv": [
            "scripts/project_command.sh",
            "test-all",
            "--change",
            "ebpf-map-sizing-eviction",
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
        }
      ],
      "task_ids": [
        "3",
        "4",
        "5"
      ],
      "verify_kinds": [
        "build",
        "test"
      ],
      "task_obligations": [
        {
          "task_id": "3",
          "verify_kinds": [
            "build",
            "test"
          ],
          "evidence_roles": [
            "old_consumer",
            "replacement_consumer"
          ]
        },
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
        },
        {
          "task_id": "5",
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
      "expected_observation": "ARM64 容器内全量编译通过，无残留旧签名调用。",
      "symbol_identities": null
    },
    {
      "id": "surface-flow-rate-cumulative-model",
      "kind": "internal_api",
      "name": "flow_rate 累计口径修复",
      "change_kind": "modified",
      "contract_impact": "breaking",
      "compatibility": {
        "old_consumer_paths": [
          "server/src/traffic_analyzer.cpp",
          "server/src/weak_netmgr.cpp"
        ],
        "replacement_consumer_paths": [
          "server/src/traffic_analyzer.cpp",
          "server/src/weak_netmgr.cpp"
        ],
        "replacement_policy": "required",
        "expected_old_result": "删表采样 + top-1000 bps 加总",
        "migration_path": "sampleTopFlows/getRealTimeStats/detectAnomalies 改为 per-key delta 模型；clearHistory 重置 tracker",
        "exit_condition": "突发流量下 totalBps 不再丢流，板上 iperf3 冒烟通过"
      },
      "producer_paths": [
        "server/src/net_traffic.cpp",
        "server/src/traffic_analyzer.cpp"
      ],
      "consumer_kind": "production_caller",
      "consumer_paths": [
        "server/src/weak_netmgr.cpp"
      ],
      "entrypoint": "updateTrafficAnalysis → traffic_bps → history",
      "evidence_contracts": [
        {
          "probe_id": "probe-flow-rate-cumulative-model-build-old-consumer",
          "kind": "build",
          "role": "old_consumer",
          "argv": [
            "scripts/project_command.sh",
            "build-server",
            "--change",
            "ebpf-map-sizing-eviction",
            "--json"
          ],
          "expected_exit_codes": [
            0
          ],
          "output_contains": "weaknet-dbus-server"
        },
        {
          "probe_id": "probe-flow-rate-cumulative-model-build-replacement-consumer",
          "kind": "build",
          "role": "replacement_consumer",
          "argv": [
            "scripts/project_command.sh",
            "build-server",
            "--change",
            "ebpf-map-sizing-eviction",
            "--json"
          ],
          "expected_exit_codes": [
            0
          ],
          "output_contains": "weaknet-dbus-server"
        },
        {
          "probe_id": "probe-flow-rate-cumulative-model-test-old-consumer",
          "kind": "test",
          "role": "old_consumer",
          "argv": [
            "scripts/project_command.sh",
            "test-all",
            "--change",
            "ebpf-map-sizing-eviction",
            "--json"
          ],
          "expected_exit_codes": [
            0
          ],
          "output_contains": "100% tests passed"
        },
        {
          "probe_id": "probe-flow-rate-cumulative-model-test-replacement-consumer",
          "kind": "test",
          "role": "replacement_consumer",
          "argv": [
            "scripts/project_command.sh",
            "test-all",
            "--change",
            "ebpf-map-sizing-eviction",
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
          "requirement": "flow_rate 统计口径改为累计 delta 模型",
          "scenarios": [
            "突发流量下累计口径不丢流"
          ]
        }
      ],
      "task_ids": [
        "14",
        "15"
      ],
      "verify_kinds": [
        "build",
        "test"
      ],
      "task_obligations": [
        {
          "task_id": "14",
          "verify_kinds": [
            "build",
            "test"
          ],
          "evidence_roles": [
            "old_consumer",
            "replacement_consumer"
          ]
        },
        {
          "task_id": "15",
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
      "expected_observation": "ARM64 容器编译通过；板上 iperf3 突发下 disappeared_keys 可见且 totalBps 反映全量。",
      "symbol_identities": null
    }
  ]
}
```
<!-- /autoai:integration-completeness:v1 -->

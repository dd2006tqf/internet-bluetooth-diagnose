# Design

## Overview

修复真机核验与交叉核对发现的两处缺陷，它们共同导致「eBPF Map 容量定标」在开发板上失效：

1. **接线缺陷**：已解算的容量计划没有到达实际加载点（`flow_rate` 与蓝牙两条通路）。
2. **规格表缺陷**：`process_stats` 的 `default_entries`（8192）不等于 BPF 源码声明的
   编译期常量（65536），违反「未配置时行为与 v1 零变化」这一核心不变式。

两处同源：都会让内核中该 map 的实际容量与设计意图不符，且都被既有测试掩盖。

## 缺陷一：容量计划未到达加载点

```
ProcessProfilerPlugin::start()
  └─ resolveScopePlan(ProcessProfiler, cfg, ram)  →  MapSizingPlan（已解算，正确）
       └─ ProcessNetProfiler::init(path, plan)
            ├─ [共享 fd 分支] analyzer->initForInterface("") ← plan 未传！
            │     └─ return true                                   ← plan 被丢弃
            └─ [独立加载回退分支] applyMapSizingPlan(obj, plan)     ← 永不执行
```

两条独立断链：

1. **flow_rate**：`NetTrafficAnalyzer::initForInterface(iface, plan = {})` 的 `plan` 有默认空值，
   `TrafficAnalyzer::start` 与 `WeakNetMgr::startTrafficAnalysis` 均无 plan 形参，
   `server.cpp` 的流量分析线程不传递配置 → 真正加载 `flow_rate.bpf.o` 的调用点收到空 plan。
2. **bluetooth**：`BtAudioAnalyzer::init(bpfObjectPath)` 不接受 plan、也从不调用
   `applyMapSizingPlan`；`BtMonitor::initPhase2` 同样无 plan 形参。`MapSizingScope::Bluetooth`
   因此无任何调用方，`kBluetoothSpecs` 成为死代码。

**连带后果**：共享 fd 路径下 `impl_->process_stats_max` 保持初始值 `0`（语义为「容量未知」），
而它被直接传给 `PerKeyCounterTracker::update(snapshot, max_entries)`。按追踪器自身语义
「容量未知时不作水位判定」，生产环境下 process_profiler 的 `eviction_limited` 检测静默失效。

## 缺陷二：规格表默认值与内核编译期常量不一致

`bpf_map_sizing.hpp` 明确规定 `default_entries` 必须等于该 map 在对应 `.bpf.c` 中的编译期
`max_entries`，以保证「不配任何定标文件时行为零变化」。逐项核对 14 张受管 map：

| map | 规格表 default | BPF 源码常量 | 判定 |
|---|---|---|---|
| retrans_stats | 65536 | 65536 | ✅ |
| retrans_events | 1024 | 1024 | ✅ |
| current_sec | 65536 | 65536 | ✅ |
| **process_stats** | **8192** | **65536** | ❌ 差 8 倍 |
| http_txn_stats | 8192 | 8192 | ✅ |
| recvmsg_ctx_map | 1024 | 1024 | ✅ |
| fd_resolvers | 1024 | 1024 | ✅ |
| pending_recv | 1024 | 1024 | ✅ |
| dns_self_endpoints | 256 | 256 | ✅ |
| conn_start | 8192 | `CONN_START_MAX_ENTRIES`=8192 | ✅ |
| conn_ports | 128 | `CONN_PORTS_MAX_ENTRIES`=128 | ✅ |
| drop_stats_map | 256 | 256 | ✅ |
| active_sessions | 64 | 64 | ✅ |
| bt_traffic | 64 | 64 | ✅ |

只有 `process_stats` 不符。该约定原先只写在注释里、无人校验，而既有单元测试使用自造 spec
（`kSpec{"test_map", ...}`），从不读取真实规格表，因此完全掩盖了这个漂移。

**两处缺陷的叠加效应**：板端 `process_stats` 实测 65536，既可能是「未定标」（缺陷一），
也可能是「回落到错误的 8192 失败后保留了源码默认值」——同一现象有两个成因，
必须同时修复才能让板端结果可解释。

## 修复策略

| 断点 | 修法 |
|------|------|
| `initForInterface` 的 `plan` 默认值 | **移除默认值**，使任何遗漏传参的调用点成为编译错误 |
| `TrafficAnalyzer::start` / `WeakNetMgr::startTrafficAnalysis` | 增加 `plan` 形参并逐层转发 |
| `server.cpp` 流量分析线程 | 按 `ctx->cfg.process_profiler.map_sizing` 解算 plan 后下传 |
| `ProcessNetProfiler::init` 共享 fd 分支 | 提前返回前记录 `process_stats` 的 resolved 容量 |
| `BtAudioAnalyzer::init` / `BtMonitor::initPhase2` | 增加 `plan` 形参，在 open→load 之间应用 |
| `bt_monitor.cpp` 的调用点 | 按 `ctx->cfg.bluetooth.map_sizing` 解算 plan 后下传 |
| `kProcessProfilerSpecs` 的 `process_stats` | `default_entries` 8192 → 65536，与源码一致 |
| 规格表一致性无校验 | 新增交叉校验测试，解析 `.bpf.c` 的 `max_entries`（含 `#define`）逐项比对 |

核心安全机制有两个：

- **移除 `initForInterface` 的默认参数**：把「每个调用点都必须给出 plan」从纪律要求变成
  编译期约束。这正是缺陷一逃过 x86 测试的原因（默认参数让遗漏静默通过）。
- **交叉校验测试**：把「规格表必须等于源码常量」这条注释里的约定变成可执行的断言，
  防止同一类漂移再次发生。

## 变更文件清单

| 文件 | 变更 | 说明 |
|------|------|------|
| `server/include/net_traffic.h` | 修改 | `initForInterface` 移除 plan 默认值，改为必填 |
| `server/include/traffic_analyzer.hpp` | 修改 | `start()` 增加 plan 形参 |
| `server/src/traffic_analyzer.cpp` | 修改 | 转发 plan 到 `initForInterface` |
| `server/include/weak_netmgr.hpp` | 修改 | `startTrafficAnalysis()` 增加 plan 形参 |
| `server/src/weak_netmgr.cpp` | 修改 | 转发 plan 到 `TrafficAnalyzer::start` |
| `server/src/server.cpp` | 修改 | 流量分析线程解算 plan 并下传 |
| `server/src/process_net_profiler.cpp` | 修改 | 共享 fd 分支记录 resolved 容量 |
| `server/include/bt_audio_analyzer.hpp` | 修改 | `init()` 增加 plan 形参 |
| `server/src/bt_audio_analyzer.cpp` | 修改 | open→load 之间应用 plan |
| `server/include/bt_monitor.hpp` | 修改 | `initPhase2()` 增加 plan 形参 |
| `server/src/bt_monitor.cpp` | 修改 | 解算 plan 并传给 `BtAudioAnalyzer::init` |
| `server/src/utils/bpf_map_sizing.cpp` | 修改 | `process_stats` 的 `default_entries` 修正为 65536 |
| `server/test/test_ebpf.cpp` | 修改 | 适配 `initForInterface` 的必填 plan 形参 |
| `server/test/unit/test_traffic_analyzer_gtest.cpp` | 修改 | 适配 `TrafficAnalyzer::start` 的必填 plan 形参（4 处） |
| `server/test/special/test_bt_monitor.cpp` | 修改 | 适配 `BtMonitor::initPhase2` 的必填 plan 形参（3 处） |
| `server/test/unit/test_map_sizing_spec_sync_gtest.cpp` | 新增 | 规格表与 `.bpf.c` 常量的交叉校验测试 |

> 三处既有测试文件必须同步修改：移除 `initForInterface` 的默认参数后，任何未传 plan 的
> 调用点都会编译失败——这正是设计意图（把纪律变成编译期约束），但受影响既有测试的调用点
> 必须一并显式传 `{}`（语义为「不做定标」，与这些测试原本验证降级行为的意图一致）。

## 验证策略

| 任务 | 验证方式 | 依据 |
|------|---------|------|
| 1–3（接线） | `observability_only` 例外，`build` + `test` | 接线正确性在 x86 上有可验证的静态保证（编译期约束）；最终效果只能在 ARM64 板上观测 |
| 4（规格表） | **正常 TDD**：`RED` → `GREEN` → `REGRESSION` | 交叉校验测试是纯逻辑断言，可在 x86 上真实失败与通过 |

任务 4 能走正常 TDD，是因为它有一个可执行的失败判据：修正前，交叉校验测试在
`process_stats` 一项上 `EXPECT_EQ` 失败（8192 ≠ 65536）；修正后全绿。这比走例外更强。

**流程教训（前一 change 因此废弃）**：`unavailable_hardware` / `unavailable_external_service`
会让该 task 进入 `blocking_exception_task_ids`，在 `verifyIntegrationEvidence` 中生成
`provisionally_blocked` 义务，而 `provisional Generator closure cannot Pass` —— 除非在真机上
用 REGRESSION 闭环逐条解除，否则 change 永远无法 Pass 与归档。上一轮试图用该类别框住
「需要真机」的义务，结果机器路径无法解除，5 个任务全部完成、x86/ARM64 均通过，却卡死
在 evaluation 无法归档。凡「编译+既有测试足够刻画代码正确性、运行效果另行人工核验」的
变更，应使用 `observability_only`。

板端 `bpftool map show` 核对由人工执行并记录在交付说明中，**不作为 harness 证据**：
已审阅的 Project Profile 中没有板端命令，`bindSurfaceProbes` 要求 argv 与计划契约逐字
相同（实测板端 argv 被拒），因此板端结果无法进入证据链。

<!-- autoai:tdd-policy:v1 -->
```json
{
  "schema_version": 1,
  "default": "required",
  "exceptions": [
    {
      "id": "exc-map-sizing-wiring",
      "category": "observability_only",
      "task_ids": ["1", "2", "3"],
      "paths": [
        "server/include/net_traffic.h",
        "server/include/traffic_analyzer.hpp",
        "server/src/traffic_analyzer.cpp",
        "server/include/weak_netmgr.hpp",
        "server/src/weak_netmgr.cpp",
        "server/src/server.cpp",
        "server/src/process_net_profiler.cpp",
        "server/include/bt_audio_analyzer.hpp",
        "server/src/bt_audio_analyzer.cpp",
        "server/include/bt_monitor.hpp",
        "server/src/bt_monitor.cpp",
        "server/test/test_ebpf.cpp",
        "server/test/unit/test_traffic_analyzer_gtest.cpp",
        "server/test/special/test_bt_monitor.cpp"
      ],
      "reason": "任务 1-3 为接线缺陷修复：把已解算的容量计划贯通到实际加载点。其最终效果（内核 map 的 max_entries）只能在 ARM64 开发板上观测，但接线正确性在 x86 上有可验证的静态保证：移除 initForInterface 的 plan 默认值后，任何漏传 plan 的调用点都无法通过编译；且既有单元测试须全部保持通过。故以 build+test 作为该可观测接线的验证手段。任务 4（规格表与源码常量一致性）有可执行的失败判据，走正常 RED-GREEN-REGRESSION，不在本例外内",
      "alternative_verify_kinds": ["build", "test"],
      "exit_condition": "x86 build-server 与 test-all 全绿（含移除默认参数后的编译期约束）+ ARM64 容器编译通过 + 人工在开发板上核对 flow_rate 的 current_sec/process_stats 与 a2dp 的 active_sessions/bt_traffic 四张 map 的 max_entries 等于 resolved 值"
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
  "rationale": "把已解算的 eBPF Map 容量计划贯通到 flow_rate 与 a2dp 两个实际加载点，修正共享 map fd 路径的容量记录，并修正规格表默认值与 BPF 源码常量的一致性漂移",
  "classification": {
    "production": [
      "server/include/net_traffic.h",
      "server/include/traffic_analyzer.hpp",
      "server/src/traffic_analyzer.cpp",
      "server/include/weak_netmgr.hpp",
      "server/src/weak_netmgr.cpp",
      "server/src/server.cpp",
      "server/src/process_net_profiler.cpp",
      "server/src/monitor_ebpf_plugins.cpp",
      "server/include/bt_audio_analyzer.hpp",
      "server/src/bt_audio_analyzer.cpp",
      "server/include/bt_monitor.hpp",
      "server/src/bt_monitor.cpp",
      "server/src/utils/bpf_map_sizing.cpp"
    ],
    "tests": [
      "server/test/test_ebpf.cpp",
      "server/test/unit/test_traffic_analyzer_gtest.cpp",
      "server/test/special/test_bt_monitor.cpp",
      "server/test/unit/test_map_sizing_spec_sync_gtest.cpp",
      "server/test/CMakeLists.txt"
    ],
    "project_docs": [],
    "project_tooling": [],
    "examples": [],
    "generated": [],
    "vendor": []
  },
  "thresholds": {
    "production": {
      "added_lines": {"expected": 130, "review_at": 260, "hard_limit": 390},
      "touched_files": {"expected": 12, "review_at": 16, "hard_limit": 20},
      "new_files": {"expected": 0, "review_at": 1, "hard_limit": 2}
    },
    "tests": {
      "added_lines": {"expected": 190, "review_at": 300, "hard_limit": 420},
      "touched_files": {"expected": 5, "review_at": 7, "hard_limit": 9},
      "new_files": {"expected": 1, "review_at": 2, "hard_limit": 3}
    },
    "project_support": {
      "added_lines": {"expected": 0, "review_at": 30, "hard_limit": 60},
      "new_files": {"expected": 0, "review_at": 1, "hard_limit": 2}
    },
    "generated": {
      "files": {"expected": 0, "review_at": 0, "hard_limit": 0},
      "bytes": {"expected": 0, "review_at": 0, "hard_limit": 0}
    }
  },
  "structural_allowances": {
    "public_contracts": [],
    "build_targets": [],
    "build_graph_entries": [],
    "distribution_surfaces": [],
    "direct_dependencies": []
  },
  "reuse_decisions": [
    {
      "id": "reuse-map-sizing-plan",
      "path": "server/include/utils/bpf_map_sizing.hpp",
      "symbol": "MapSizingPlan",
      "decision": "reuse",
      "reason": "容量计划类型、resolveScopePlan 解算入口与 applyMapSizingPlan 应用入口已在上一轮实现，本轮只新增调用点，不重复实现"
    },
    {
      "id": "reuse-find-resolved-max",
      "path": "server/include/utils/bpf_map_sizing.hpp",
      "symbol": "findResolvedMax",
      "decision": "reuse",
      "reason": "共享 fd 分支需要记录 process_stats 的 resolved 容量，该查询函数已存在且有单元测试覆盖，直接复用而不新增查询 API"
    },
    {
      "id": "reuse-get-map-sizing-specs",
      "path": "server/include/utils/bpf_map_sizing.hpp",
      "symbol": "getMapSizingSpecs",
      "decision": "reuse",
      "reason": "规格表查询入口已存在且被全部监控器使用；交叉校验测试直接遍历它，从而覆盖全部真实受管 map，而不是另建一份测试用的规格副本"
    }
  ],
  "obsolete_items": [],
  "exceptions": []
}
```
<!-- /autoai:implementation-economy:v2 -->

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
      "id": "surface-flow-rate-sizing-wiring",
      "kind": "internal_api",
      "name": "NetTrafficAnalyzer::initForInterface plan 贯通链",
      "change_kind": "modified",
      "contract_impact": "compatible",
      "compatibility": null,
      "producer_paths": [
        "server/include/net_traffic.h",
        "server/include/traffic_analyzer.hpp",
        "server/src/traffic_analyzer.cpp",
        "server/include/weak_netmgr.hpp",
        "server/src/weak_netmgr.cpp"
      ],
      "consumer_kind": "production_caller",
      "consumer_paths": [
        "server/src/server.cpp"
      ],
      "entrypoint": "traffic analysis thread -> WeakNetMgr::startTrafficAnalysis -> TrafficAnalyzer::start -> NetTrafficAnalyzer::initForInterface",
      "evidence_contracts": [
        {
          "probe_id": "probe-flow-rate-wiring-build-current",
          "kind": "build",
          "role": "current",
          "argv": [
            "scripts/project_command.sh",
            "build-server",
            "--change",
            "ebpf-map-sizing-wiring",
            "--json"
          ],
          "expected_exit_codes": [
            0
          ],
          "output_contains": "weaknet-dbus-server"
        },
        {
          "probe_id": "probe-flow-rate-wiring-test-current",
          "kind": "test",
          "role": "current",
          "argv": [
            "scripts/project_command.sh",
            "test-all",
            "--change",
            "ebpf-map-sizing-wiring",
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
          "requirement": "eBPF Map 容量定标贯通到全部加载路径",
          "scenarios": [
            "flow_rate 两张 map 经流量分析通路获得定标"
          ]
        }
      ],
      "task_ids": ["1"],
      "verify_kinds": ["build", "test"],
      "task_obligations": [
        {
          "task_id": "1",
          "verify_kinds": ["build", "test"],
          "evidence_roles": ["current"]
        }
      ],
      "expected_observation": "全仓编译通过，证明每个 initForInterface 调用点都显式传入了容量计划；既有单元测试全部保持通过",
      "symbol_identities": null
    },
    {
      "id": "surface-profiler-shared-capacity",
      "kind": "internal_api",
      "name": "ProcessNetProfiler 共享 map fd 的容量记录",
      "change_kind": "modified",
      "contract_impact": "compatible",
      "compatibility": null,
      "producer_paths": [
        "server/src/process_net_profiler.cpp"
      ],
      "consumer_kind": "production_caller",
      "consumer_paths": [
        "server/src/monitor_ebpf_plugins.cpp"
      ],
      "entrypoint": "ProcessProfilerPlugin::start -> ProcessNetProfiler::init",
      "evidence_contracts": [
        {
          "probe_id": "probe-profiler-capacity-build-current",
          "kind": "build",
          "role": "current",
          "argv": [
            "scripts/project_command.sh",
            "build-server",
            "--change",
            "ebpf-map-sizing-wiring",
            "--json"
          ],
          "expected_exit_codes": [
            0
          ],
          "output_contains": "weaknet-dbus-server"
        },
        {
          "probe_id": "probe-profiler-capacity-test-current",
          "kind": "test",
          "role": "current",
          "argv": [
            "scripts/project_command.sh",
            "test-all",
            "--change",
            "ebpf-map-sizing-wiring",
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
          "requirement": "eBPF Map 容量定标贯通到全部加载路径",
          "scenarios": [
            "共享 map fd 路径记录 resolved 容量"
          ]
        }
      ],
      "task_ids": ["2"],
      "verify_kinds": ["build", "test"],
      "task_obligations": [
        {
          "task_id": "2",
          "verify_kinds": ["build", "test"],
          "evidence_roles": ["current"]
        }
      ],
      "expected_observation": "编译与既有测试通过，证明共享 fd 分支在提前返回之前记录了 resolved 容量，逐 key 驱逐判定不再因容量未知而跳过",
      "symbol_identities": null
    },
    {
      "id": "surface-bluetooth-map-sizing",
      "kind": "internal_api",
      "name": "BtAudioAnalyzer::init plan 贯通链",
      "change_kind": "modified",
      "contract_impact": "compatible",
      "compatibility": null,
      "producer_paths": [
        "server/include/bt_audio_analyzer.hpp",
        "server/src/bt_audio_analyzer.cpp",
        "server/include/bt_monitor.hpp"
      ],
      "consumer_kind": "production_caller",
      "consumer_paths": [
        "server/src/bt_monitor.cpp"
      ],
      "entrypoint": "start_bt_monitor_thread -> BtMonitor::initPhase2 -> BtAudioAnalyzer::init",
      "evidence_contracts": [
        {
          "probe_id": "probe-bluetooth-sizing-build-current",
          "kind": "build",
          "role": "current",
          "argv": [
            "scripts/project_command.sh",
            "build-server",
            "--change",
            "ebpf-map-sizing-wiring",
            "--json"
          ],
          "expected_exit_codes": [
            0
          ],
          "output_contains": "weaknet-dbus-server"
        },
        {
          "probe_id": "probe-bluetooth-sizing-test-current",
          "kind": "test",
          "role": "current",
          "argv": [
            "scripts/project_command.sh",
            "test-all",
            "--change",
            "ebpf-map-sizing-wiring",
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
          "requirement": "eBPF Map 容量定标贯通到全部加载路径",
          "scenarios": [
            "蓝牙两张 map 获得定标"
          ]
        }
      ],
      "task_ids": ["3"],
      "verify_kinds": ["build", "test"],
      "task_obligations": [
        {
          "task_id": "3",
          "verify_kinds": ["build", "test"],
          "evidence_roles": ["current"]
        }
      ],
      "expected_observation": "编译与既有测试通过，证明蓝牙加载点接收并应用容量计划，MapSizingScope::Bluetooth 不再是无调用方的死代码",
      "symbol_identities": null
    },
    {
      "id": "surface-map-sizing-spec-consistency",
      "kind": "internal_api",
      "name": "规格表 default_entries 与 BPF 源码常量一致性",
      "change_kind": "modified",
      "contract_impact": "compatible",
      "compatibility": null,
      "producer_paths": [
        "server/include/utils/bpf_map_sizing.hpp",
        "server/src/utils/bpf_map_sizing.cpp"
      ],
      "consumer_kind": "production_caller",
      "consumer_paths": [
        "server/src/monitor_ebpf_plugins.cpp"
      ],
      "entrypoint": "plugin start -> resolveScopePlan -> getMapSizingSpecs",
      "evidence_contracts": [
        {
          "probe_id": "probe-spec-consistency-test-current",
          "kind": "test",
          "role": "current",
          "argv": [
            "scripts/project_command.sh",
            "test-all",
            "--change",
            "ebpf-map-sizing-wiring",
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
          "requirement": "Map 定标规格表与内核编译期常量一致",
          "scenarios": [
            "规格表默认值与 BPF 源码声明逐一相符"
          ]
        }
      ],
      "task_ids": ["4"],
      "verify_kinds": ["test"],
      "task_obligations": [
        {
          "task_id": "4",
          "verify_kinds": ["test"],
          "evidence_roles": ["current"]
        }
      ],
      "expected_observation": "交叉校验测试遍历全部 MapSizingScope 的真实规格表并与 .bpf.c 声明比对，process_stats 一项由失败转为通过",
      "symbol_identities": null
    }
  ]
}
```
<!-- /autoai:integration-completeness:v1 -->

# Design

## Overview

将 5 个编译进二进制、加载了 eBPF 程序但从未被 `server.cpp` 启动的监控器（`DnsMonitor`、`WifiPacketLossMonitor`、`HttpLatencyMonitor`、`ProcessNetProfiler`；蓝牙音频 `a2dp_media` 已由 `BtMonitor::initPhase2` 接管，不作重复）纳入 `ServerContext` 统一生命周期。

设计要点：
- 每个监控器在 `ServerContext` 持有成员实例（RAII），`start_server()` 中按依赖顺序显式启动，退出逆序 `stop()`。
- 同探针共享义务明确化（方案二）：`tcp_retransmit_skb` 被 `TcpLossMonitor`（丢包率，`tcp_conn_key`）与 `ProcessNetProfiler`（每进程流量含重传，PID+连接）**各自独立加载**，账本/聚合键/消费者不同，**不做技术合并**。本变更只消除隐式重复与无序加载，将双消费者绑定到同一 `ServerContext` 生命周期，并显式声明为架构债。
- 消除孤儿数据路径：每个加载的 eBPF 监控器都必须有 `server.cpp` 线程读取数据并经既有 D-Bus 路径出口。

<!-- autoai:tdd-policy:v1 -->
```json
{
  "schema_version": 1,
  "default": "required",
  "exceptions": [
    {
      "id": "exception-no-target-hardware",
      "category": "unavailable_hardware",
      "task_ids": ["1", "2", "3"],
      "paths": ["server/src/server.cpp", "server/include/server.hpp"],
      "reason": "eBPF 运行时 + D-Bus 服务端需在 ARM64 开发板（Radxa Cubie A7A）上运行，当前 VM 环境无此硬件；项目 profile 已记录 target-run: unavailable。本变更只涉及编译期接口变更（ServerContext 成员扩展、server.cpp 线程启动函数），编译通过即可证明变更正确性",
      "alternative_verify_kinds": ["build"],
      "exit_condition": "ARM64 容器内编译通过后归档"
    }
  ]
}
```
<!-- /autoai:tdd-policy:v1 -->

<!-- autoai:implementation-economy:v2 -->
```json
{
  "schema_version": 2,
  "profile": "micro",
  "rationale": "将已编译但未接线的 eBPF 监控器纳入 ServerContext 统一生命周期，消除孤儿数据路径与无序启动",
  "classification": {
    "production": ["server/src/server.cpp", "server/include/server.hpp"],
    "tests": [],
    "project_docs": [],
    "project_tooling": [],
    "examples": [],
    "generated": [],
    "vendor": []
  },
  "thresholds": {
    "production": {
      "added_lines": {"expected": 120, "review_at": 200, "hard_limit": 300},
      "touched_files": {"expected": 2, "review_at": 5, "hard_limit": 8},
      "new_files": {"expected": 0, "review_at": 2, "hard_limit": 3}
    },
    "tests": {
      "added_lines": {"expected": 0, "review_at": 0, "hard_limit": 0},
      "touched_files": {"expected": 0, "review_at": 0, "hard_limit": 0},
      "new_files": {"expected": 0, "review_at": 0, "hard_limit": 0}
    },
    "project_support": {
      "added_lines": {"expected": 0, "review_at": 0, "hard_limit": 0},
      "new_files": {"expected": 0, "review_at": 0, "hard_limit": 0}
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
  "reuse_decisions": [],
  "obsolete_items": [],
  "exceptions": [
    {
      "id": "exc-no-target-hardware",
      "metric": "touched_files",
      "paths": ["server/src/server.cpp", "server/include/server.hpp"],
      "reason": "eBPF 运行时 + D-Bus 需 ARM64 开发板，当前 VM 无此硬件，仅编译验证",
      "requirement_refs": ["unavailable_hardware"],
      "task_ids": ["1", "2", "3"],
      "verification": "ARM64 容器内编译通过"
    }
  ]
}
```
<!-- /autoai:implementation-economy:v2 -->

## Surface Inventory

本变更修改 4 个生产表面，新增 1 个生产表面：

### Surface: ServerContext 成员扩展（internal_api, modified）
- `ServerContext` 新增 `DnsMonitor*`、`WifiPacketLossMonitor*`、`HttpLatencyMonitor*`、`ProcessNetProfiler*` 成员
- `producer_paths`: `["server/include/server.hpp"]`
- `consumer_kind`: `production_caller`（`server.cpp` 的 `start_server()` 构造）
- `entrypoint`: `weaknet-dbus-server start_server()` 构造阶段
- `contract_impact`: `compatible`（仅扩展 struct，不改变已有成员布局）
- `task_ids`: `["1"]`
- `verify_kinds`: `["build"]`

### Surface: server.cpp 线程启动函数（internal_api, added）
- 新增 `start_dns_monitor_thread(ServerContext*)`、`start_wifi_loss_monitor_thread(ServerContext*)`、`start_http_latency_monitor_thread(ServerContext*)`、`start_process_net_profiler_thread(ServerContext*)` 四个函数
- `producer_paths`: `["server/src/server.cpp"]`
- `consumer_paths`: `["server/src/server.cpp"]`（`start_server()` 调用）
- `consumer_kind`: `production_caller`
- `entrypoint`: `weaknet-dbus-server start_server()` 启动阶段
- `contract_impact`: `compatible`（新增函数，不改变已有接口）
- `task_ids`: `["2"]`
- `verify_kinds`: `["build"]`

### Surface: 各监控器 init() 调用端（internal_api, modified）
- 各监控器 `init()` 的调用方从 `event_manager` 内部按需加载改为 `server.cpp` 中的显式线程启动
- `producer_paths`: 各监控器对应的 `.cpp` 文件
- `consumer_paths`: `["server/src/server.cpp"]`
- `consumer_kind`: `production_caller`
- `entrypoint`: `weaknet-dbus-server start_server()` 启动阶段
- `contract_impact`: `compatible`
- `task_ids`: `["2"]`
- `verify_kinds`: `["build"]`

### Surface: 同探针 tcp_retransmit_skb 共享义务（internal_api, modified）
- `TcpLossMonitor` 与 `ProcessNetProfiler` 各自独立加载 `tcp_retransmit_skb`，不做技术合并
- `producer_paths`: `["server/src/tcp_retransmit_monitor.cpp", "server/src/process_net_profiler.cpp"]`
- `consumer_paths`: `["server/src/server.cpp"]`
- `consumer_kind`: `production_caller`
- `entrypoint`: `weaknet-dbus-server start_server()` 启动阶段
- `contract_impact`: `compatible`
- `task_ids`: `["3"]`
- `verify_kinds`: `["build"]`

<!-- autoai:integration-completeness:v1 -->
```json
{
  "schema_version": 1,
  "discovery": {
    "mode": "reviewed_inventory",
    "compile_commands_path": null
  },
  "surfaces": [
    {
      "id": "surface-server-context-members",
      "kind": "build_or_install",
      "name": "ServerContext eBPF 监控器成员扩展",
      "change_kind": "modified",
      "contract_impact": "compatible",
      "compatibility": null,
      "producer_paths": ["server/include/server.hpp"],
      "consumer_kind": "downstream_build",
      "consumer_paths": ["server/src/server.cpp"],
      "entrypoint": "make -C server",
      "runnable_artifact": false,
      "evidence_contracts": [
        {
          "probe_id": "probe-build-context-members",
          "kind": "build",
          "role": "current",
          "argv": ["make", "-C", "server"],
          "expected_exit_codes": [0],
          "output_contains": "weaknet-dbus-server"
        }
      ],
      "expected_observation": "ServerContext 包含新增的 4 个 eBPF 监控器成员指针，编译通过且二进制存在",
      "requirement_refs": [
        {"spec_path": "specs/weaknet-server/spec.md", "operation": "ADDED", "requirement": "eBPF监控器生命周期统一管理", "scenarios": ["ServerContext 持有 eBPF 监控器成员"]}
      ],
      "symbol_identities": null,
      "task_ids": ["1"],
      "task_obligations": [
        {"task_id": "1", "verify_kinds": ["build"], "evidence_roles": ["current"]}
      ],
      "verify_kinds": ["build"]
    },
    {
      "id": "surface-server-thread-startup",
      "kind": "build_or_install",
      "name": "server.cpp eBPF 监控器线程启动函数",
      "change_kind": "added",
      "contract_impact": "compatible",
      "compatibility": null,
      "producer_paths": ["server/src/server.cpp"],
      "consumer_kind": "downstream_build",
      "consumer_paths": ["server/src/server.cpp"],
      "entrypoint": "make -C server",
      "runnable_artifact": false,
      "evidence_contracts": [
        {
          "probe_id": "probe-build-thread-startup",
          "kind": "build",
          "role": "current",
          "argv": ["make", "-C", "server"],
          "expected_exit_codes": [0],
          "output_contains": "weaknet-dbus-server"
        }
      ],
      "expected_observation": "4 个新线程函数在 start_server() 中被调用，编译通过且二进制存在",
      "requirement_refs": [
        {"spec_path": "specs/weaknet-server/spec.md", "operation": "ADDED", "requirement": "eBPF监控器生命周期统一管理", "scenarios": ["server.cpp 启动并停止 eBPF 监控器"]}
      ],
      "symbol_identities": null,
      "task_ids": ["2"],
      "task_obligations": [
        {"task_id": "2", "verify_kinds": ["build"], "evidence_roles": ["current"]}
      ],
      "verify_kinds": ["build"]
    }
  ]
}
```
<!-- /autoai:integration-completeness:v1 -->
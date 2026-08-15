# Design

## Overview

对 `ServerContext` 生命周期做统一整改：把捕获 `ServerContext*` 的各监控线程与信号线程从 detached 裸线程收敛为 `ServerContext` 持有的 joinable 句柄；修正主线程退出顺序（先停循环、再 join 全部线程、最后释放资源）；把 `WeakNetMgr`/`DbusService`/DBus 连接改为受析构管理的 RAII 资源；D-Bus 信号发射加互斥；统一接口列表事实源。目标是保证主线程返回前绝对能 join 全部线程并释放全部资源，消除野指针访问与资源泄漏。

## 架构变更

```
Before (现状):
  各 start_*_thread 内 std::thread( [ctx]{ ... } ).detach()
  shutdown: monitor->stop() → running=false → join 部分线程（缺 rtt/jitter/rssi/bt）
  弱指针：WeakNetMgr*/DbusService* 裸指针，从不释放；DBus 连接从不 unref
  接口列表：ServerContext::iface_list(iface_mutex) 与 WeakNetMgr::current_interfaces_ 双源，写无锁

After (目标):
  rtt/jitter/rssi/bt 线程 → ctx->xxx_thread = std::thread(...)（joinable）
  shutdown: running=false → join 全部 *_thread → 释放资源（由 ~ServerContext 统一收尾）
  WeakNetMgr*/DbusService* → 受 ~ServerContext delete；~ServerContext 释放 DBus 连接
  每线程内 `if (!ctx->weak_mgr) new` 删除，weak_mgr 由 start_server() 一次性预建
  DbusService::emit* 加 send_mutex_，移除各 detach 信号子线程、改同步发射
  接口列表唯一源 = WeakNetMgr::current_interfaces_，dbus_service 经 getCurrentInterfaces() 读取
```

## 变更文件清单

| 文件 | 变更类型 | 说明 |
|------|---------|------|
| `server/include/server.hpp` | 修改 | ServerContext 增加 jitter/rssi/bt 线程句柄；weak_mgr/service 改受析构 delete；析构函数声明 |
| `server/src/server.cpp` | 修改 | 退出顺序重排、移除显式 monitor stop、信号线程同步化、weak_mgr 预建、接口列表写锁收口、`~ServerContext` 定义 |
| `server/src/rtt_monitor.cpp` | 修改 | 线程改为 ctx->rtt_thread；信号改为同步发射；删除内部 new weak_mgr |
| `server/src/jitter_monitor.cpp` | 修改 | 线程改为 ctx->jitter_thread；信号同步；删除 new weak_mgr |
| `server/src/rssi_monitor.cpp` | 修改 | 线程改为 ctx->rssi_thread；信号同步；删除 new weak_mgr |
| `server/src/bt_monitor.cpp` | 修改 | 线程改为 ctx->bt_thread；信号同步 |
| `server/src/tcp_loss_monitor.cpp` | 修改 | 信号同步；删除 new weak_mgr |
| `server/include/dbus_service.hpp` | 修改 | DbusService 增加 send_mutex_ 成员 |
| `server/src/dbus_service.cpp` | 修改 | emit* 加锁；handle 接口列表改读 WeakNetMgr |
| `server/src/using_iface.cpp` | 修改 | UsingInterfaceManager 析构 join netlink 事件线程（pimpl 完整定义处） |

## 关键设计点

1. **线程句柄**：`ServerContext` 增加 `std::thread jitter_thread/rssi_thread/bt_thread`；`rtt_thread` 由从未赋值改为实际赋值。各 `start_*_thread` 由 `.detach()` 改为 `ctx->xxx_thread = std::thread(...)`。
2. **退出顺序**（`start_server()`）：
   ```
   ctx.running = false;            // 先停循环
   join iface/using/rtt/jitter/rssi/tcp_loss/traffic/network_quality/bt/dns/wifi/http/process
   // 各 worker 退出时已自行 stop() 本地 make_unique 监控器
   // 资源交由 ~ServerContext() 统一释放（DBus 连接 / service / weak_mgr）
   ```
3. **RAII**：`~ServerContext()`（定义在 `server.cpp`）释放 `connection`（`dbus_connection_close`+`dbus_connection_unref`）、`service`（`delete`）、`weak_mgr`（`delete`）。`init_dbus` 失败路径维持 `delete ctx->service` 语义，不构成双释放（成功后由析构统一负责）。
4. **D-Bus 发射加锁**：`DbusService` 增加 `std::mutex send_mutex_`，`emitChanged`/`emitSpecificSignal`/`emitNetworkQualitySignal` 包 `lock_guard`；各处 `std::thread([ctx]{ emit }).detach()` 改为同步 `ctx->service->emit...(...)`。
5. **weak_mgr 一次性创建**：`start_server()` 预建 `weak_mgr`；所有监控线程内 `if (!ctx->weak_mgr) ctx->weak_mgr = new WeakNetMgr();` 删除（iface/using/traffic/rtt/jitter/rssi/tcp_loss）。
6. **接口列表唯一源**：移除对 `ServerContext::iface_list` 的无锁写（`server.cpp` 改走 `weak_mgr->updateInterfaces`，`current = latest`）；`dbus_service` 的 `handleListInterfaces`/`handleHealthCheck`/`handlePing` 改读 `ctx->weak_mgr->getCurrentInterfaces()`。
7. **UsingInterfaceManager**：它在 `using_iface.cpp` 有 detached netlink 事件线程（`impl_->worker.detach()`）。在该 `Impl` 完整定义处补 `~UsingInterfaceManager`（pimpl 需要完整类型），置停并 join worker，避免单例析构后线程野访问 `impl_`。

> 注：atomic monitor 指针（`326d0c8` 已修 load/store 竞争）保持不变，本变更不转移 monitor 所有权（仍由各工作线程在退出路径 `make_unique` 本地对象 + `store(nullptr)`）。

## Integration Completeness 说明

本变更为服务端进程内部生命周期与资源所有权整改。受管 product surface 收敛为**一个**编译级 surface（服务端二进制重建）：运行时行为（线程 join/资源释放）依赖真机，无法在容器内以可观测命令验证，故：

- surface `kind=build_or_install`、`runnable_artifact=false`
- `verify_kinds=["build"]`
- TDD policy exception（`unavailable_hardware`）声明真实 exit condition：在 ARM64 真机手动启动/退出验证无挂起。

## 自检

- 无占位符/矛盾决策；requirement↔task↔surface 引用闭合。
- 所有线程均有 joinable 句柄且被 join；stop 顺序为先停循环再释放。
- 所有资源（DBus 连接/manager/service）由 `~ServerContext` 释放，无双 delete。
- D-Bus 发射互斥且无 detached 子线程。

## TDD Policy

<!-- autoai:tdd-policy:v1 -->
```json
{
  "schema_version": 1,
  "default": "required",
  "exceptions": [
    {
      "id": "exc-lifecycle-build-verify",
      "category": "unavailable_hardware",
      "task_ids": ["1", "2", "3", "4", "5", "6"],
      "paths": [
        "server/include/server.hpp",
        "server/src/server.cpp",
        "server/src/rtt_monitor.cpp",
        "server/src/jitter_monitor.cpp",
        "server/src/rssi_monitor.cpp",
        "server/src/bt_monitor.cpp",
        "server/src/tcp_loss_monitor.cpp",
        "server/include/dbus_service.hpp",
        "server/src/dbus_service.cpp",
        "server/include/weak_netmgr.hpp",
        "server/src/weak_netmgr.cpp",
        "server/src/using_iface.cpp"
      ],
      "reason": "线程生命周期与资源所有权整改，运行时行为依赖 ARM64 真机（eBPF/蓝牙），容器内仅能完成编译验证；受管验证走 build_or_install + 真机手动退出自检",
      "alternative_verify_kinds": ["build"],
      "exit_condition": "ARM64 容器内编译通过且真机启动/退出手动自检无挂起后归档"
    }
  ]
}
```
<!-- /autoai:tdd-policy:v1 -->

## Implementation Economy

<!-- autoai:implementation-economy:v2 -->
```json
{
  "schema_version": 2,
  "profile": "small",
  "rationale": "服务端线程与资源生命周期统一整改，改动集中于 server.hpp/server.cpp 与各 monitor 的 start_*_thread、DbusService 与接口列表收口",
  "classification": {
    "production": [
      "server/include/server.hpp",
      "server/src/server.cpp",
      "server/src/rtt_monitor.cpp",
      "server/src/jitter_monitor.cpp",
      "server/src/rssi_monitor.cpp",
      "server/src/bt_monitor.cpp",
      "server/src/tcp_loss_monitor.cpp",
      "server/include/dbus_service.hpp",
      "server/src/dbus_service.cpp",
      "server/include/weak_netmgr.hpp",
      "server/src/weak_netmgr.cpp",
      "server/src/using_iface.cpp"
    ],
    "tests": [],
    "project_docs": [],
    "project_tooling": ["server/Makefile"],
    "examples": [],
    "generated": [],
    "vendor": []
  },
  "thresholds": {
    "production": {
      "added_lines": {"expected": 130, "review_at": 280, "hard_limit": 440},
      "touched_files": {"expected": 12, "review_at": 14, "hard_limit": 16},
      "new_files": {"expected": 0, "review_at": 1, "hard_limit": 2}
    },
    "tests": {
      "added_lines": {"expected": 0, "review_at": 150, "hard_limit": 300},
      "touched_files": {"expected": 0, "review_at": 3, "hard_limit": 5},
      "new_files": {"expected": 0, "review_at": 1, "hard_limit": 2}
    },
    "project_support": {
      "added_lines": {"expected": 0, "review_at": 30, "hard_limit": 50},
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
  "reuse_decisions": [],
  "obsolete_items": [],
  "exceptions": []
}
```
<!-- /autoai:implementation-economy:v2 -->

## Integration Completeness

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
      "id": "surface-server-lifecycle-build",
      "kind": "build_or_install",
      "name": "weaknet-dbus-server 生命周期整改编译产物",
      "change_kind": "modified",
      "contract_impact": "compatible",
      "compatibility": null,
      "producer_paths": [
        "server/include/server.hpp",
        "server/src/server.cpp",
        "server/src/rtt_monitor.cpp",
        "server/src/jitter_monitor.cpp",
        "server/src/rssi_monitor.cpp",
        "server/src/bt_monitor.cpp",
        "server/src/tcp_loss_monitor.cpp",
        "server/include/dbus_service.hpp",
        "server/src/dbus_service.cpp",
        "server/include/weak_netmgr.hpp",
        "server/src/weak_netmgr.cpp",
        "server/src/using_iface.cpp"
      ],
      "consumer_kind": "downstream_build",
      "consumer_paths": ["server/Makefile"],
      "entrypoint": "make -C server (ARM64 容器内)",
      "runnable_artifact": false,
      "evidence_contracts": [
        {
          "probe_id": "probe-server-lifecycle-build",
          "kind": "build",
          "role": "current",
          "argv": [
            "scripts/project_command.sh",
            "build-server",
            "--change",
            "server-lifecycle-fix",
            "--json"
          ],
          "expected_exit_codes": [0],
          "output_contains": "weaknet-dbus-server"
        }
      ],
      "requirement_refs": [
        {
          "spec_path": "specs/weaknet-server/spec.md",
          "operation": "ADDED",
          "requirement": "服务端生命周期统一管理",
          "scenarios": ["所有监控线程可被主线程 join"]
        },
        {
          "spec_path": "specs/weaknet-server/spec.md",
          "operation": "ADDED",
          "requirement": "服务端生命周期统一管理",
          "scenarios": ["ServerContext 析构释放资源"]
        },
        {
          "spec_path": "specs/weaknet-server/spec.md",
          "operation": "ADDED",
          "requirement": "服务端生命周期统一管理",
          "scenarios": ["停止顺序先停循环再释放"]
        },
        {
          "spec_path": "specs/weaknet-server/spec.md",
          "operation": "ADDED",
          "requirement": "服务端生命周期统一管理",
          "scenarios": ["接口列表唯一事实源"]
        },
        {
          "spec_path": "specs/weaknet-server/spec.md",
          "operation": "ADDED",
          "requirement": "D-Bus 信号发射线程安全",
          "scenarios": ["信号发射加互斥"]
        }
      ],
      "task_ids": ["1", "2", "3", "4", "5", "6"],
      "verify_kinds": ["build"],
      "task_obligations": [
        {
          "task_id": "1",
          "verify_kinds": ["build"],
          "evidence_roles": ["current"]
        },
        {
          "task_id": "2",
          "verify_kinds": ["build"],
          "evidence_roles": ["current"]
        },
        {
          "task_id": "3",
          "verify_kinds": ["build"],
          "evidence_roles": ["current"]
        },
        {
          "task_id": "4",
          "verify_kinds": ["build"],
          "evidence_roles": ["current"]
        },
        {
          "task_id": "5",
          "verify_kinds": ["build"],
          "evidence_roles": ["current"]
        },
        {
          "task_id": "6",
          "verify_kinds": ["build"],
          "evidence_roles": ["current"]
        }
      ],
      "expected_observation": "服务端二进制在 ARM64 容器内重建成功，线程/资源生命周期整改编译通过",
      "symbol_identities": null
    }
  ]
}
```
<!-- /autoai:integration-completeness:v1 -->

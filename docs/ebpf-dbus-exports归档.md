# eBPF 监控器 D-Bus 结构化出口 —— 功能归档文档

> **功能状态**：✅ 已实现（代码提交于 `caa62ff`，ARM64 容器编译通过）
> **归档日期**：2026-08-06
> **背景**：本功能为 `ebpf-monitor-dbus-exports` 的代码实现部分。该 change 因"代码已实现不套用 OpenSpec 追认流程"的铁律而被废弃，但生产代码已提交并保留。本文档记录该功能的接口契约与用法，作为代码侧的功能归档。

---

## 一、功能概述

为 4 个已接入 `ServerContext` 的 eBPF 监控器（DNS、Wi-Fi 丢包、HTTP 延迟、进程画像）补齐 **D-Bus 结构化数据出口** 和 **客户端 C API**，消除"采集但客户端不可见"的黑盒状态，使 eBPF 监控数据经既有 D-Bus 路径向第三方开放。

在此之前，这 4 个监控器的数据只通过 `LOG_INFO` 打印到服务端日志，客户端无法查询。本功能补全了 `采集 → 生命周期 → D-Bus 出口 → 客户端可查` 的闭环。

## 二、涉及文件

| 文件 | 变更类型 | 说明 |
|------|---------|------|
| `server/include/common.hpp` | 新增 | 4 个 D-Bus 方法常量 |
| `server/include/dbus_service.hpp` | 新增 | 4 个 handler 声明 + 监控器前置声明 |
| `server/src/dbus_service.cpp` | 新增 | 4 个 handler 实现 + 消息分发 |
| `client/weaknet_client.h` | 新增 | 4 个 C API 声明 |
| `client/client.cpp` | 新增 | `WeakNetClient` 4 个查询方法 + 4 个 extern C API |

## 三、服务端 D-Bus 方法

服务名/对象路径/接口沿用既有约定（`com.example.WeakNet` / `/com/example/WeakNet` / `com.example.WeakNet`）。新增 4 个方法，均返回单个字符串：

| 方法名 | 读取监控器 | 返回字段 |
|--------|-----------|---------|
| `GetDnsStats` | `ctx->dns_monitor` | `totalQueries`, `totalResponses`, `totalTimeouts`, `totalErrors`, `avgLatencyMs`, `maxLatencyMs`, `timeoutRate` |
| `GetWifiLossStats` | `ctx->wifi_loss_monitor` | 每个接口的 `ifindex`, `rxPkts`, `txPkts`, `txDrops`, `txLossRate` |
| `GetHttpLatencyStats` | `ctx->http_latency_monitor` | `totalTxns`, `p50Ms`, `p95Ms`, `p99Ms`, `maxMs`, `analysis` |
| `GetProcessProfiling` | `ctx->process_net_profiler` | Top 带宽进程（`pid`,`comm`,`txBytes`,`txPackets`,`retrans`）+ Top 重传进程 |

**序列化格式**：字段以 `key:value` 组织，多个字段用 `|` 分隔。监控器不可用时返回 `"XXX monitor not available"`。

**示例返回（GetDnsStats）**：
```
totalQueries:123|totalResponses:120|totalTimeouts:2|totalErrors:1|avgLatencyMs:35|maxLatencyMs:120|timeoutRate:1.62602
```

## 四、客户端 C API

客户端动态库 `libweaknet.so` 新增 4 个 C 接口，封装 D-Bus 方法调用：

```c
bool weaknet_get_dns_stats(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size);
bool weaknet_get_wifi_loss_stats(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size);
bool weaknet_get_http_latency_stats(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size);
bool weaknet_get_process_profiling(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size);
```

**用法示例**：
```c
#include "weaknet_client.h"

char buf[1024], err[256];
if (!weaknet_init()) { /* 初始化失败 */ }
if (weaknet_get_dns_stats(buf, sizeof(buf), err, sizeof(err))) {
    printf("DNS 统计: %s\n", buf);
} else {
    fprintf(stderr, "查询失败: %s\n", err);
}
weaknet_cleanup();
```

**返回**：`true` 表示成功，`buffer` 填充 D-Bus 返回的字符串；`false` 表示失败，`error_buffer` 填充错误信息。

## 五、实现说明

- **统一入口**：客户端 `WeakNetClient` 内部通过 `requestStringData(methodName, label, ...)` 辅助函数统一处理 D-Bus 方法调用与字符串解析，4 个查询方法复用同一逻辑，避免重复。
- **遍历订阅**：`GetWifiLossStats` 和 `GetProcessProfiling` 返回多个条目，handler 内部用 `|` 拼接所有条目，客户端可解析分隔。
- **监控器可用性**：handler 先检查 `ctx->xxx_monitor && xxx_monitor->isAvailable()`，避免空指针；监控器未加载/不可用时返回友好提示而非报错。

## 六、验证状态

- ✅ 服务端 `weaknet-dbus-server` 在 ARM64 容器编译通过，二进制包含 `GetDnsStats` 等 4 个方法常量
- ✅ 客户端 `libweaknet.so` 在 ARM64 容器编译通过，动态库包含 `weaknet_get_dns_stats` 等 4 个导出符号
- ⚠️ 运行时行为（D-Bus 实际返回正确数据）未做端到端验证，需在 ARM64 开发板（Radxa Cubie A7A）上验证

## 七、与既有 eBPF 闭环的关系

| change | 环节 | 状态 |
|--------|------|------|
| `integrate-ebpf-monitor-lifecycle` | eBPF 采集 + 生命周期 | ✅ 完整闭环（evaluation + 归档） |
| `ebpf-monitor-dbus-exports`（本功能） | D-Bus 出口 + 客户端 API | ✅ 代码实现，未走 evaluation |

本功能补齐了 eBPF 监控链路的数据出口层，使 6 个 eBPF 程序从"采集"到"客户端可查"形成完整能力。

## 八、注意事项

- 本功能代码已提交，**不套用 OpenSpec change 追认流程**（见 CLAUDE.md 铁律）。
- 若未来需要将本功能纳入受管验收，需 `git revert caa62ff` 后走完整 freeze → 记录 evidence → evaluation 流程。
- 运行时验证依赖 ARM64 真机，当前环境仅完成容器编译验证。

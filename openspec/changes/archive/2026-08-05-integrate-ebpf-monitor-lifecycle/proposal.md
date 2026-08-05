# Proposal: integrate-ebpf-monitor-lifecycle

## Why

当前系统有 6 个 eBPF 程序，但它们在 `server.cpp` 的启动链上集成非常不完整：

| eBPF 程序 | 所属类 | 有独立线程？ | 在 server.cpp 启动？ | 被谁拉取数据？ |
|---|---|---|---|---|
| `tcp_retransmit.bpf.c` | `TcpLossMonitor` | ✅ 有 | ✅ server.cpp 508 | 独立线程 |
| `flow_rate.bpf.c` | `ProcessNetProfiler` | ❌ 无 | ❌ 未引用 | 无人拉取 |
| `dns_monitor.bpf.c` | `DnsMonitor` | ❌ 无 | ❌ 未引用 | 无人拉取 |
| `wifi_packet_loss.bpf.c` | `WifiPacketLossMonitor` | ❌ 无 | ❌ 未引用 | 无人拉取 |
| `http_latency.bpf.c` | `HttpLatencyMonitor` | ❌ 无 | ❌ 未引用 | 无人拉取 |
| `a2dp_media.bpf.c` | `BtAudioAnalyzer` | ❌ 无独立线程 | ❌ 间接（bt_monitor 内部 initPhase2） | bt_monitor 内部拉取 |

这造成三个问题：

1. **孤儿监控器**：4 个监控器编译进二进制、加载 BPF 程序，但 `server.cpp` 从未调用其 `init()` 和 `getStats()`，数据无人消费，白白消耗内核资源。
2. **生命周期各自为政**：`init/stop/available` 状态分散在各监控器内部，`start_server()` 无法统一管理启动顺序、降级和优雅关闭。`ServerContext` 中没有对应成员指针，无法在析构时统一释放。
3. **启动顺序不可控**：`event_manager` 内部按需加载部分 BPF 监控器，但无显式依赖声明。比如 `flow_rate.bpf.c` 重用了 `tcp_retransmit.bpf.c` 的探针 `tcp_retransmit_skb`，但加载顺序无保证。

## What

1. **`ServerContext` 新增成员指针**：`DnsMonitor*`、`WifiPacketLossMonitor*`、`HttpLatencyMonitor*`、`ProcessNetProfiler*`，各持一份 `unique_ptr`，生命周期与 `ServerContext` 绑定。
2. **`server.cpp` 新增 `start_xxx_thread()` 函数**：为 4 个孤儿监控器 + 一个明确的 eBPF 初始化阶段，统一在 `start_server()` 中按依赖顺序启动。
3. **同探针共享义务明确化（方案二）**：`tcp_retransmit` 和 `flow_rate` 虽都挂 `kprobe/tcp_retransmit_skb`，但账本结构、聚合键（前者按 `tcp_conn_key` 算丢包率，后者按 PID+连接算每进程流量）与消费者完全不同，**不做技术合并**。引入技术合并需跨 `bpf_object` 的 link 协调、会显著增加成本且破坏各自的账本结构，收益仅是省去每次重传包的一次低开销 kprobe。本变更只把双消费者 init 绑定到同一 `ServerContext` 生命周期以消除**隐式重复与无序加载**，并显式声明为架构债，不作为合并目标。
4. **`stop_xxx()` + 优雅关闭**：`start_server()` 中 `Looper::run()` 退出后，按逆序 `stop()` 各监控器，释放 BPF 资源。

## 变更范围（生产代码）

- `server/include/server.hpp` — `ServerContext` 新增成员
- `server/src/server.cpp` — 新增 `start_xxx` 线程函数 + 启动顺序编排
- `server/src/event_manager.cpp` — 移除按需加载孤儿 BPF 的逻辑（如有）
- 各监控器 `init()` 改为传入 `ServerContext` 引用，而非硬编码 BPF 路径
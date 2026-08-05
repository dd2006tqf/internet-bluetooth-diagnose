# WeakNet Server Spec — Delta for integrate-ebpf-monitor-lifecycle

## ADDED Requirements

### Requirement: eBPF监控器生命周期统一管理
服务端 MUST 将此前未被 `server.cpp` 启动的 eBPF 监控器（`DnsMonitor`、`WifiPacketLossMonitor`、`HttpLatencyMonitor`、`ProcessNetProfiler`）纳入 `ServerContext` 统一生命周期，在 `start_server()` 中显式 init/start，退出时统一 stop，消除孤儿与无序加载。

#### Scenario: ServerContext 持有 eBPF 监控器成员
- **WHEN** `ServerContext` 被创建
- **THEN** MUST 持有 `DnsMonitor`、`WifiPacketLossMonitor`、`HttpLatencyMonitor`、`ProcessNetProfiler` 的实例成员，生命周期与上下文绑定，退出时统一释放

#### Scenario: server.cpp 启动并停止 eBPF 监控器
- **WHEN** `start_server()` 执行
- **THEN** MUST 为上述每个监控器调用对应启动函数并按依赖顺序启动；MUST 在 `Looper::run()` 退出后调用各监控器 `stop()` 释放 BPF 资源

### Requirement: 同探针双消费者共享义务明确化
服务端 MUST 将挂载同一内核探针（`tcp_retransmit_skb`）的 `TcpLossMonitor`（丢包率）与 `ProcessNetProfiler`（每进程流量）声明为共享义务明确化（方案二）：各自独立加载、账本不同、**不做技术合并**，仅绑定同一 `ServerContext` 生命周期以消除隐式重复与无序加载，并显式记录为架构设计决策。

#### Scenario: 同探针不合并、绑定同一生命周期
- **WHEN** `tcp_retransmit_skb` 同时被 `TcpLossMonitor` 与 `ProcessNetProfiler` 各自独立加载
- **THEN** MUST 保持独立加载不作技术合并，并将双消费者 init 绑定到同一 `ServerContext` 生命周期，消除隐式重复加载与无序启动，确保每个加载的 eBPF 监控器都有 `server.cpp` 启动线程消费其数据并经既有 D-Bus 路径出口

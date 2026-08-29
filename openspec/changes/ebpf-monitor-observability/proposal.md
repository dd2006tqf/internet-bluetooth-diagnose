## Why

当前 6 个 eBPF 用户态采集器缺少统一的可观测契约，生产诊断无法快速判断采集器是否已挂载、是否降级、读取是否失败及自身开销。本 change 统一现有六个采集器的健康和性能观测能力，并接入真实 D-Bus 查询入口，为后续新增探针提供稳定基础。

## What Changes

- 统一 `DnsMonitor`、`WifiPacketLossMonitor`、`HttpLatencyMonitor`、`ProcessNetProfiler`、`TcpRetransMonitor`、`BtAudioAnalyzer` 的观测接口。
- 增加生命周期状态、健康快照、错误计数、采样时间和性能指标。
- 为 `BtAudioAnalyzer` 区分 attached、fallback 和 error。
- 新增 `GetEbpfMonitorHealth` D-Bus 方法，返回六个采集器的 JSON 快照。
- 将 `TcpRetransMonitor` 作为 `tcp_retransmit.bpf.c` 的真实用户态消费者接入 `ServerContext` 和服务端启动路径；现有 `TcpLossMonitor` 保留为系统 TCP 统计组件。
- 增加单元测试、D-Bus 消费者测试及文档更新。
- 清理未使用的 `EbpfMonitorBase` 草稿，避免重复 ownership 框架。

本 change 不新增 TCP 连接、TLS、ICMP、队列或 TC 探针，不改变现有 D-Bus 方法协议。

## Capabilities

### New Capabilities
- `ebpf-observability`: 六个 eBPF 采集器的统一健康、状态和性能观测。

### Modified Capabilities
- `weaknet-server`: 增加 eBPF 健康查询及 `TcpRetransMonitor` 的生产生命周期接入。

## Impact

影响服务端监控器、`ServerContext`、D-Bus 服务、客户端代表性消费者、构建配置、测试和文档。新增 `GetEbpfMonitorHealth` 为兼容性 D-Bus 方法，不影响已有调用方。不增加第三方依赖。

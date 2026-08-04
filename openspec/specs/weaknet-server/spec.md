# weaknet-server Specification

## Purpose
TBD - created by archiving change fix-ping-error-logging. Update Purpose after archive.
## Requirements
### Requirement: ping 错误日志

`ping()` 函数在遇到错误时 **MUST** 记录详细的错误信息，包括错误原因和相关上下文。

#### Scenario: 添加 LOG(ERROR) 记录错误信息

当 `ping()` 函数遇到错误时，**MUST** 使用 `LOG(ERROR)` 记录具体的错误信息，包括 `strerror(errno)`、接口名、主机名等上下文。

### Requirement: 降级模式日志告警
MUST 在流量分析启动后检查降级模式状态并输出 WARNING 日志。
#### Scenario: 降级模式触发 WARNING 日志
- **WHEN** 流量分析启动后 TrafficAnalyzer 处于降级模式
- **THEN** MUST 输出 LOG_WARNING 日志包含接口名和降级状态信息

### Requirement: Wi-Fi丢包归因
服务端 MUST 通过 eBPF 探针区分发送丢包和接收丢包，精确定位网络故障点。

#### Scenario: eBPF收发丢包探针实现
- **WHEN** 服务端启动且内核支持 eBPF
- **THEN** MUST 挂载收发路径 tracepoint（netif_receive_skb / net_dev_queue / net_dev_xmit），统计每接口的收发/丢弃/重试包数

#### Scenario: 用户态监控接口
- **WHEN** WifiPacketLossMonitor 被调用 getStats()
- **THEN** MUST 返回所有接口的收发/丢弃/重传统计，并支持分析指定接口的丢包归因

### Requirement: 系统集成
丢包归因 MUST 集成到网络质量评估和构建系统。

#### Scenario: 评估器集成和构建更新
- **WHEN** 服务端编译
- **THEN** MUST 编译新的 wifi_packet_loss.bpf.o，server/Makefile 包含新的编译规则

### Requirement: 进程网络画像
服务端 MUST 通过 eBPF 探针统计每个进程的网络流量，定位占带宽或大量重传的进程。

#### Scenario: eBPF进程统计探针
- **WHEN** 服务端启动且内核支持 eBPF
- **THEN** MUST 在现有 flow_rate.bpf.c 中新增 process_stats Map，按 PID 记录进程名、发送字节数、包数、重传次数

#### Scenario: 用户态监控接口
- **WHEN** ProcessNetProfiler 被调用 getTopBandwidth() 或 getTopRetransmit()
- **THEN** MUST 返回按带宽或重传排序的进程统计列表

### Requirement: 进程画像集成
进程级画像 MUST 集成到构建系统。

#### Scenario: 构建更新
- **WHEN** 服务端编译
- **THEN** MUST 编译新增的 process_net_profiler.cpp，server/Makefile 包含新的源文件

### Requirement: TCP重传追踪
服务端 MUST 通过 eBPF 探针实时追踪每个 TCP 连接的重传事件。

#### Scenario: eBPF重传探针实现
- **WHEN** 服务端启动且内核支持 eBPF
- **THEN** MUST 挂载 kprobe/tcp_retransmit_skb 探针，记录每次重传的源/目的 IP:Port、PID、重传次数

#### Scenario: 用户态监控接口
- **WHEN** TcpRetransMonitor 被调用 pollEvents()
- **THEN** MUST 返回自上次调用以来的所有重传事件

### Requirement: DNS监控
服务端 MUST 通过 eBPF 探针监控 DNS 解析延迟，检测超时和失败。

#### Scenario: eBPF DNS探针实现
- **WHEN** 服务端启动且内核支持 eBPF
- **THEN** MUST 挂载 kprobe/udp_sendmsg 和 kprobe/udp_recvmsg 探针，过滤端口 53 的 UDP 包

#### Scenario: 用户态监控接口
- **WHEN** DnsMonitor 被调用 pollCompleted()
- **THEN** MUST 返回已完成的 DNS 查询记录，包含延迟、超时状态和响应码

### Requirement: TCP DNS 重传集成
TCP 重传和 DNS 监控 MUST 集成到构建系统。

#### Scenario: 构建更新
- **WHEN** 服务端编译
- **THEN** MUST 编译新的 tcp_retransmit.bpf.o 和 dns_monitor.bpf.o

### Requirement: HTTP请求延迟监控
服务端 MUST 通过 eBPF 探针监控 HTTP 请求/响应的 TTFB（首字节延迟），区分应用慢与网络慢。

#### Scenario: eBPF HTTP延迟探针实现
- **WHEN** 服务端启动且内核支持 eBPF
- **THEN** MUST 挂载 kprobe/tcp_sendmsg 和 kprobe/tcp_recvmsg 探针，提取 HTTP 首部数据，计算请求发送到响应接收的 TTFB

#### Scenario: 用户态监控接口
- **WHEN** HttpLatencyMonitor 被调用 getGlobalStats()
- **THEN** MUST 返回 TTFB 分位数（P50/P95/P99）和按目标主机聚合的延迟统计

### Requirement: HTTP延迟集成
HTTP 延迟监控 MUST 集成到构建系统。

#### Scenario: 构建更新
- **WHEN** 服务端编译
- **THEN** MUST 编译新增的 http_latency.bpf.o，server/Makefile 包含新的编译规则


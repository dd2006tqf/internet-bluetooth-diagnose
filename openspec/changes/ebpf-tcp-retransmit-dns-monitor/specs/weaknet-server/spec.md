# WeakNet Server Spec — Delta for ebpf-tcp-retransmit-dns-monitor

## ADDED Requirements
### Requirement: TCP重传追踪
服务端 MUST 通过 eBPF 探针实时追踪每个 TCP 连接的重传事件，提供比 netlink dump 更高效的丢包率计算。

#### Scenario: eBPF重传探针实现
- **WHEN** 服务端启动且内核支持 eBPF
- **THEN** MUST 挂载 kprobe/tcp_retransmit_skb 探针，记录每次重传的源/目的 IP:Port、PID、重传次数

#### Scenario: 用户态监控接口
- **WHEN** TcpRetransMonitor 被调用 pollEvents()
- **THEN** MUST 返回自上次调用以来的所有重传事件

#### Scenario: 丢包率计算替代
- **WHEN** TcpRetransMonitor 可用时
- **THEN** MUST 优先使用 eBPF 数据计算丢包率，net_tcp.cpp 作为降级方案

### Requirement: DNS监控
服务端 MUST 通过 eBPF 探针监控 DNS 解析延迟，检测超时和失败。

#### Scenario: eBPF DNS探针实现
- **WHEN** 服务端启动且内核支持 eBPF
- **THEN** MUST 挂载 kprobe/udp_sendmsg 和 kprobe/udp_recvmsg 探针，过滤端口 53 的 UDP 包

#### Scenario: 用户态监控接口
- **WHEN** DnsMonitor 被调用 pollCompleted()
- **THEN** MUST 返回已完成的 DNS 查询记录，包含延迟、超时状态和响应码

### Requirement: 系统集成
新 eBPF 监控器 MUST 集成到现有的网络质量评估和构建系统中。

#### Scenario: 评估器集成和构建更新
- **WHEN** 服务端编译
- **THEN** MUST 编译新的 .bpf.o 文件，server/Makefile 包含新的编译规则

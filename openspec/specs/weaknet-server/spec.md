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
- **THEN** MUST 编译新的 wifi_packet_loss.bpf.o，并由 CMake eBPF 目标纳入构建图

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
- **THEN** MUST 编译新增的 process_net_profiler.cpp，并由 CMake 服务端目标纳入构建图

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
- **THEN** MUST 编译新增的 http_latency.bpf.o，并由 CMake eBPF 目标纳入构建图

### Requirement: 服务端启动时创建带时间戳的日志文件

系统 MUST 在服务端启动时，在 `server/log/` 目录下创建一个以启动时间戳命名的文本日志文件。

#### Scenario: 服务端启动时创建日志文件

- **GIVEN** 服务端尚未启动，`server/log/` 目录不存在
- **WHEN** 服务端调用 `start_server()` 启动
- **THEN** 系统 SHALL 创建 `server/log/` 目录，并打开文件 `server/log/server_YYYYMMDD_HHMMSS.log` 开始写入日志

#### Scenario: 日志文件命名格式

- **GIVEN** 服务端启动时间为 2026-08-16 18:30:45
- **WHEN** 服务端创建日志文件
- **THEN** 日志文件名 SHALL 为 `server/log/server_20260816_183045.log`

### Requirement: 日志文件内容格式

日志文件中的每一行 MUST 包含时间戳、日志级别、模块名和消息内容。

#### Scenario: INFO 级别日志格式

- **GIVEN** 服务端输出一条 INFO 级别的日志，模块为 DBUS，消息为 "connected to session bus"
- **WHEN** 该日志被写入时间戳日志文件
- **THEN** 日志文件中对应行 SHALL 为 `[YYYY-MM-DD HH:MM:SS.ffffff] [INFO] [DBUS] connected to session bus`

#### Scenario: ERROR 级别日志格式

- **GIVEN** 服务端输出一条 ERROR 级别的日志，模块为 PING，消息为 "socket() failed"
- **WHEN** 该日志被写入时间戳日志文件
- **THEN** 日志文件中对应行 SHALL 为 `[YYYY-MM-DD HH:MM:SS.ffffff] [ERROR] [PING] socket() failed`

### Requirement: 服务终止时关闭日志文件

系统 MUST 在服务端收到终止信号（SIGINT/SIGTERM）时，停止写入日志并关闭文件。

#### Scenario: 收到 SIGINT 信号时关闭日志

- **GIVEN** 服务端正在运行，日志文件已打开
- **WHEN** 服务端收到 SIGINT 信号
- **THEN** 系统 SHALL 刷新日志文件缓冲区，关闭文件流，并正常退出

#### Scenario: 收到 SIGTERM 信号时关闭日志

- **GIVEN** 服务端正在运行，日志文件已打开
- **WHEN** 服务端收到 SIGTERM 信号
- **THEN** 系统 SHALL 刷新日志文件缓冲区，关闭文件流，并正常退出

#### Scenario: 正常退出时关闭日志

- **GIVEN** 服务端正在运行，日志文件已打开
- **WHEN** 服务端正常退出（`start_server()` 返回）
- **THEN** 系统 SHALL 关闭日志文件流

### Requirement: 日志与 glog 同步

日志文件的内容 MUST 与 glog 输出同步，不得遗漏任何日志条目。

#### Scenario: glog 输出同步到文件

- **GIVEN** glog 输出一条日志到 stderr
- **WHEN** 同一条日志被处理
- **THEN** 该日志 SHALL 同时写入时间戳日志文件

#### Scenario: glog 文件日志同步

- **GIVEN** glog 输出一条日志到 `./logs/server/` 目录
- **WHEN** 同一条日志被处理
- **THEN** 该日志 SHALL 同时写入时间戳日志文件


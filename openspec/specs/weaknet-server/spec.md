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


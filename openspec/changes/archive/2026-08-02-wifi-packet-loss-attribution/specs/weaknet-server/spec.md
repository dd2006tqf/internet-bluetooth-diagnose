# WeakNet Server Spec — Delta for wifi-packet-loss-attribution

## ADDED Requirements
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

# WeakNet Server Spec — Delta for http-request-latency-monitor

## ADDED Requirements
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

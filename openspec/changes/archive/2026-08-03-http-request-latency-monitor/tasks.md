# Tasks: http-request-latency-monitor

- [x] 1 实现 HTTP 请求/响应延迟追踪 eBPF 程序
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `HTTP请求延迟监控` | `eBPF HTTP延迟探针实现`
  - Verify: `build`
  - 实现 `server/src/http_latency.bpf.c`，挂载 kprobe/tcp_sendmsg + tcp_recvmsg，提取 HTTP 首部计算 TTFB

- [x] 2 实现 HttpLatencyMonitor 用户态读取
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `HTTP请求延迟监控` | `用户态监控接口`
  - Verify: `build`
  - 实现 `http_latency_monitor.hpp/.cpp`，从 BPF Map 读取 HTTP 事务并计算 TTFB 分位数

- [x] 3 集成到 Makefile 和构建系统
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `HTTP延迟集成` | `构建更新`
  - Verify: `build`
  - 修改 server/Makefile 添加源文件和编译规则

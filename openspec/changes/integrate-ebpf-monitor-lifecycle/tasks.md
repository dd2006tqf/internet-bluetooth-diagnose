# Tasks: integrate-ebpf-monitor-lifecycle

- [x] 1 ServerContext 新增 eBPF 监控器成员
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `eBPF监控器生命周期统一管理` | `ServerContext 持有 eBPF 监控器成员`
  - Verify: `build`
  - 在 `server/include/server.hpp` 中为 `DnsMonitor`、`WifiPacketLossMonitor`、`HttpLatencyMonitor`、`ProcessNetProfiler` 新增成员实例，支持 RAII 生命周期管理。

- [x] 2 server.cpp 启动并停止 eBPF 监控器
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `eBPF监控器生命周期统一管理` | `server.cpp 启动并停止 eBPF 监控器`
  - Verify: `build`
  - 新增各监控器的 `start_xxx()` 函数，在 `start_server()` 中按依赖顺序启动；在 `Looper::run()` 退出后调用各监控器 `stop()`。每个启动的监控器有真实线程读取数据并经既有 D-Bus 路径出口。

- [x] 3 同探针共享义务明确化（方案二）
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `同探针双消费者共享义务明确化` | `同探针不合并、绑定同一生命周期`
  - Verify: `build`
  - `tcp_retransmit_skb` 被 `TcpLossMonitor`（丢包率，`tcp_conn_key` 聚合）与 `ProcessNetProfiler`（每进程流量含重传，PID+连接聚合）各自独立加载，账本与消费者不同，**不做技术合并**。将双消费者 init 绑定同一 `ServerContext` 生命周期并显式声明为架构债，消除隐式重复与无序加载。

# Tasks: ebpf-tcp-retransmit-dns-monitor

- [x] 1 实现 TCP 重传追踪 eBPF 程序
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `TCP重传追踪` | `eBPF重传探针实现`
  - Verify: `build`
  - 实现 `server/src/tcp_retransmit.bpf.c`，挂载 kprobe/tcp_retransmit_skb，记录重传事件到 BPF Map

- [x] 2 实现 TcpRetransMonitor 用户态读取
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `TCP重传追踪` | `用户态监控接口`
  - Verify: `build`
  - 实现 `tcp_retransmit_monitor.hpp/.cpp`，从 BPF Map 读取重传事件和统计

- [x] 3 修改 tcp_loss_monitor 使用新数据源
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `TCP重传追踪` | `丢包率计算替代`
  - Verify: `build`
  - 修改 `net_tcp.cpp`，当 BPF 可用时优先使用 TcpRetransMonitor

- [x] 4 实现 DNS 监控 eBPF 程序
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `DNS监控` | `eBPF DNS探针实现`
  - Verify: `build`
  - 实现 `server/src/dns_monitor.bpf.c`，过滤端口 53 的 UDP 包

- [x] 5 实现 DnsMonitor 用户态读取
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `DNS监控` | `用户态监控接口`
  - Verify: `build`
  - 实现 `dns_monitor.hpp/.cpp`，从 BPF Map 读取 DNS 查询记录

- [x] 6 集成到网络质量评估和部署
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `系统集成` | `评估器集成和构建更新`
  - Verify: `build`
  - 修改 server/Makefile 添加编译规则，修改 net_traffic.cpp 加载新 BPF 对象

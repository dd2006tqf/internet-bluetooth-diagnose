# Tasks: ebpf-tcp-retransmit-dns-formalize

- [x] 1 验证 TCP 重传追踪代码
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `TCP重传追踪` | `eBPF重传探针实现`
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `TCP重传追踪` | `用户态监控接口`
  - Verify: `build`
  - 已有代码：tcp_retransmit.bpf.c + tcp_retransmit_monitor，容器内编译验证

- [x] 2 验证 DNS 监控代码
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `DNS监控` | `eBPF DNS探针实现`
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `DNS监控` | `用户态监控接口`
  - Verify: `build`
  - 已有代码：dns_monitor.bpf.c + dns_monitor，容器内编译验证

- [x] 3 验证 Makefile 编译规则
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `TCP DNS 重传集成` | `构建更新`
  - Verify: `build`
  - 已有 Makefile 规则，容器内完整编译验证

# Proposal: ebpf-tcp-retransmit-dns-monitor

## Why
当前 TCP 丢包监控通过 netlink dump 全量连接实现（`net_tcp.cpp`），每次打开 socket → dump 所有连接 → 关闭 socket，开销大且只能看到汇总的丢包率，无法定位到具体进程和连接。同时 DNS 解析延迟完全不可观测，无法区分"网络慢"和"DNS 慢"。

## What
新增两个 eBPF 程序：
1. **TCP 重传追踪**（`tcp_retransmit.bpf.c`）：挂载 `kprobe/tcp_retransmit_skb`，实时捕获每个 TCP 连接的重传事件，记录源/目的 IP:Port、PID、重传次数、当前 cwnd/srtt
2. **DNS 监控**（`dns_monitor.bpf.c`）：挂载 `kprobe/udp_sendmsg` + `kprobe/udp_recvmsg`，过滤端口 53 的 UDP 包，计算 DNS 解析延迟，检测超时和失败

同时修改 `net_traffic.cpp` 和 `tcp_loss_monitor.cpp` 以支持从新的 BPF Map 读取数据，提供比当前 netlink 方案更高效的 TCP 丢包率计算。

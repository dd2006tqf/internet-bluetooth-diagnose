# Proposal: ebpf-tcp-retransmit-dns-formalize

## Why
TCP 重传追踪和 DNS 监控的代码已经写好并通过编译验证，但因之前的锁继承 bug 被废弃，未正式纳入 OpenSpec 规范。这些功能需要正式的 proposal/design/specs/tasks 以完成归档，纳入项目规范并保留完整的工作流追溯。

## What
为已存在的 TCP 重传追踪（tcp_retransmit.bpf.c + TcpRetransMonitor）和 DNS 监控（dns_monitor.bpf.c + DnsMonitor）代码补全 OpenSpec 工作流文档，正式归档：
1. 补写 proposal.md / design.md / specs / tasks
2. 记录 evidence（这些代码已编译通过）
3. 走完评估和归档流程

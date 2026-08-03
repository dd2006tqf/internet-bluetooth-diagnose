# Proposal: process-network-profiling

## Why
当前流量分析只能看到**每个连接**的带宽占用和重传，但无法回答"是哪个进程/应用在占带宽、谁在大量重传"这个最基础的问题。运维排查时需要知道：某个高流量连接是哪个 PID 发起的、占多少、现在重传了多少。

## What
在现有 `flow_rate.bpf.c` 的基础上增强，从"按连接统计"升级为"按 连接+进程 统计"：
1. 在 BPF Map 中增加 PID → 进程名（comm）映射
2. 流量统计数据结构从 `conn_key → flow_data` 升级为 `(conn_key) → {pid, comm, bytes, packets}`
3. 新增按进程聚合的统计 Map，可直接查询"每个进程占用的总带宽/包数"
4. 用户态新增 `ProcessNetProfiler` 接口，查询进程级流量画像

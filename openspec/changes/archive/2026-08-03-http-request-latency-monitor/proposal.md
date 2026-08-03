# Proposal: http-request-latency-monitor

## Why
当前系统能监控网络层指标（RTT、丢包率、流量），但无法回答"到底是服务器响应慢，还是网络传输慢"这个最常见的问题。弱网环境下，用户感知到的"网页打开慢"可能来自：网络传输延迟、TCP 拥塞、服务器响应慢。没有 HTTP 请求级的数据，无法区分。

## What
新增 eBPF 程序，挂载 `kprobe/tcp_sendmsg` 和 `kprobe/tcp_recvmsg`，从 sk_buff 提取 HTTP/1.x 请求和响应头部数据，计算：
1. **TTFB（Time To First Byte）**：从发出请求到收到第一个响应字节的延迟
2. **请求/响应大小**：每个 HTTP 事务的字节数
3. **每个目标主机的 HTTP 事务延迟分布**（P50/P95/P99）

区分"应用慢 vs 网络慢"：TTFB 高说明对端响应慢；TTFB 低但后续字节传输慢说明网络带宽不足。

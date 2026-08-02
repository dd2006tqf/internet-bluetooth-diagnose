# Proposal: wifi-packet-loss-attribution

## Why
当前系统只能看到 Wi-Fi 信号弱（RSSI），但无法判断丢包发生在发射端还是接收端，也无法区分是 Wi-Fi 链路问题还是上游路由器问题。这让"网络慢"的诊断无法精确定位到故障点。

## What
新增 eBPF 程序，通过挂载内核网络栈的收发路径 tracepoint，统计每个接口的发送/接收/丢弃包数：
- `tracepoint/net/netif_receive_skb` — 接收路径，统计每接口 rx 包数/字节
- `tracepoint/net/net_dev_queue` — 发送路径，统计每接口 tx 包数
- 结合网卡驱动层的报错计数，区分发送丢包 vs 接收丢包

用户态监控器从 BPF Map 聚合各接口收发统计，计算发送/接收丢包率，提供给网络质量评估和 D-Bus 信号推送。

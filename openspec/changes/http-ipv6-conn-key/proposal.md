# Proposal: http-ipv6-conn-key

## Why

`get_conn_key()` 中 `if (family != AF_INET) return k;` 跳过了 IPv6 连接，导致所有 IPv6 HTTP 请求被过滤。开发板 curl 解析到 IPv6 地址（`240e:97c:2f:1::5c`），请求走 IPv6 → `family=AF_INET6` → key 全零 → `http_txn_stats` 无数据 → totalTxns=0。

## What

在 `http_latency.bpf.c` 的 `get_conn_key()` 中增加 IPv6 支持：
- 读取 `skc_rcv_saddr6` / `skc_daddr6`（128 位地址）
- 由于 `tcp_conn_key` 当前是 4 字节 `saddr/daddr`，需要扩展为支持 IPv6
- 同时影响 `dns_monitor.bpf.c` 的 IPv6 问题（DNS 同样可能走 IPv6）

## 非目标
- 不改 lifecycle / WiFi / 其他模块

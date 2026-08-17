# Proposal: http-ipv6-conn-key

## Why

`get_conn_key()` 跳过 IPv6（`if (family != AF_INET) return k`），导致 curl 解析到 IPv6 地址时 HTTP 请求全部被过滤，totalTxns=0。

## What

将 `tcp_conn_key` 从纯 IPv4 扩展为 IPv4+IPv6，`get_conn_key` 新增 AF_INET6 路径。

## 非目标
- 不改其他模块

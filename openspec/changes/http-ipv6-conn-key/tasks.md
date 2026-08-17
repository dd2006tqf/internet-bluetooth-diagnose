# Tasks: http-ipv6-conn-key

- [x] 1 扩展 tcp_conn_key 为 IPv6，get_conn_key 支持 AF_INET6
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `HTTP 延迟监控支持 IPv6 连接` | `IPv6 HTTP 请求被正确识别、记录和配对`
  - Verify: `build`
  - tcp_conn_key.saddr/daddr 改为 __u32[4]；get_conn_key 读 skc_v6_rcv_saddr/skc_v6_daddr

# Tasks: http-ipv6-conn-key

- [ ] 1 扩展 tcp_conn_key 为 IPv6，get_conn_key 支持 AF_INET6
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `HTTP 延迟监控支持 IPv6 连接` | `IPv6 HTTP 请求被正确识别和记录`
  - Verify: `build`
  - tcp_conn_key.saddr/daddr 改为 16 字节地址；get_conn_key 读 skc_rcv_saddr6/skc_daddr6；BPF 编译验证

- [ ] 2 真机验证 IPv6 HTTP 请求/响应配对
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `HTTP 延迟监控支持 IPv6 连接` | `IPv6 HTTP 请求与响应配对`
  - Verify: `build`
  - 部署到开发板，curl 触发 IPv6 流量，验证 totalTxns>0

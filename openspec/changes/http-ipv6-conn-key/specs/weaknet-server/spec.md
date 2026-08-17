# http-ipv6-conn-key Specification

## ADDED Requirements

### Requirement: HTTP 延迟监控支持 IPv6 连接

HTTP 请求延迟监控 **MUST** 支持 IPv6 连接，通过 IPv6 地址正确构建连接键，使得 IPv6 HTTP 请求/响应能够配对。

#### Scenario: IPv6 HTTP 请求被正确识别和记录

- **WHEN** 服务端启动且内核支持 eBPF
- **THEN** MUST 能够识别 IPv6 TCP 连接上的 HTTP 请求（GET/POST/PUT/DELETE/HEAD），并正确记录到 BPF Map 中

#### Scenario: IPv6 HTTP 请求与响应配对

- **WHEN** IPv6 HTTP 请求已被记录
- **THEN** MUST 能够通过 IPv6 地址匹配对应的响应，计算 TTFB

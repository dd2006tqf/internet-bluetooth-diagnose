# http-ipv6-conn-key Specification

## ADDED Requirements

### Requirement: HTTP 延迟监控支持 IPv6 连接

HTTP 请求延迟监控 **MUST** 支持 IPv6 连接，通过 IPv6 地址正确构建连接键。

#### Scenario: IPv6 HTTP 请求被正确识别、记录和配对

- **WHEN** curl 走 IPv6
- **THEN** MUST 记录到 http_txn_stats 并匹配对应响应计算 TTFB

# WeakNet Server Spec
## ADDED Requirements
### Requirement: 降级模式日志告警
MUST 在流量分析启动后检查降级模式状态并输出 WARNING 日志。
#### Scenario: 降级模式触发 WARNING 日志
- **WHEN** 流量分析启动后 TrafficAnalyzer 处于降级模式
- **THEN** MUST 输出 LOG_WARNING 日志包含接口名和降级状态信息

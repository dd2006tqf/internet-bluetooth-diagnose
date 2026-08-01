# WeakNet Server Spec — Delta for add-missing-logs

## ADDED Requirements
### Requirement: 错误路径日志
服务端和客户端所有错误路径 MUST 记录 LOG_ERROR 日志。

#### Scenario: 零日志文件添加基础日志
- **WHEN** traffic_anomaly_detector.cpp、bt_audio_fusion.cpp、net_tcp.cpp、serializer.cpp、net_info.cpp 中函数遇到错误
- **THEN** MUST 使用 LOG_ERROR 记录错误信息

#### Scenario: D-Bus 错误路径日志
- **WHEN** dbus_service.cpp 中方法返回 false 时
- **THEN** MUST 使用 LOG_ERROR 记录失败原因

#### Scenario: 客户端 C API 日志
- **WHEN** client.cpp 中 C API 函数遇到错误
- **THEN** MUST 使用 LOG_ERROR 记录错误信息

### Requirement: 入口/出口日志
关键函数 MUST 记录入口和出口日志。

#### Scenario: Safe 方法入口/出口日志
- **WHEN** weak_netmgr.cpp 中 Safe 系列方法被调用
- **THEN** MUST 记录入口和出口日志（与 updateRttAndStateSafe 一致）

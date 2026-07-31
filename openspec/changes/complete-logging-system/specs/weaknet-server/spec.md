# weaknet-server Specification Delta

## ADDED Requirements

### Requirement: 日志格式化宏正确性

`LOG_INFO_F` 和 `LOG_ERROR_F` 宏 **MUST** 支持 printf 风格的格式化字符串，通过 `snprintf` 将格式化后的字符串传递给 glog。

#### Scenario: LOG_INFO_F 正确格式化参数

当调用 `LOG_INFO_F("server", "count=%d name=%s", 42, "test")` 时，日志输出 **MUST** 包含 `count=42 name=test`。

#### Scenario: LOG_ERROR_F 正确格式化参数

当调用 `LOG_ERROR_F("network", "errno=%d msg=%s", errno, strerror(errno))` 时，日志输出 **MUST** 包含格式化后的 `errno` 和 `strerror` 结果。

### Requirement: 日志系统磁盘满保护

`Logger::init` **MUST** 启用 `FLAGS_stop_logging_if_full_disk` 防止磁盘满时 glog 崩溃。

#### Scenario: 磁盘满保护已启用

当 `Logger::init` 成功返回后，`FLAGS_stop_logging_if_full_disk` **MUST** 为 `true`。

### Requirement: 日志清理策略

`Logger` 类 **MUST** 提供 `cleanOldLogs` 静态函数，删除超过指定天数的日志文件。

#### Scenario: cleanOldLogs 清理旧日志

当调用 `cleanOldLogs(log_dir, max_age_days)` 时，修改时间超过 `max_age_days` 天的日志文件 **MUST** 被删除，未超期的文件 **MUST** 保留。

#### Scenario: cleanOldLogs 目录不存在时不报错

当 `log_dir` 不存在时，`cleanOldLogs` **MUST** 安全返回而不抛出异常。

### Requirement: 直接输出替换为日志宏

`server/src/` 和 `client/` 中的 `std::cout`、`std::cerr`、`printf`、`fprintf` 直接输出 **MUST** 替换为对应的日志宏（`LOG_INFO`、`LOG_ERROR` 等）。用于字符串格式化的 `snprintf` 不在此范围内。

#### Scenario: net_wifiriss.cpp 使用日志宏

`net_wifiriss.cpp` 中的所有 `std::cerr` **MUST** 替换为 `LOG_ERROR` 或 `LOG_INFO`，并使用 `LogModule::NETWORK` 模块标识。

#### Scenario: tcp_loss_monitor.cpp 使用日志宏

`tcp_loss_monitor.cpp` 中的 `std::printf` **MUST** 替换为 `LOG_INFO`，并使用 `LogModule::TCP_LOSS` 模块标识。

#### Scenario: using_iface.cpp 使用日志宏

`using_iface.cpp` 中的 `std::cout` 和 `std::cerr` **MUST** 替换为 `LOG_INFO` 或 `LOG_ERROR`，并使用 `LogModule::INTERFACE` 模块标识。

#### Scenario: dbus_service.cpp 使用日志宏

`dbus_service.cpp` 中的 `std::printf` **MUST** 替换为 `LOG_INFO`，并使用 `LogModule::DBUS` 模块标识。

#### Scenario: net_traffic.cpp 使用日志宏

`net_traffic.cpp` 中的 `fprintf` **MUST** 替换为 `LOG_ERROR`，并使用 `LogModule::NETWORK` 模块标识。

### Requirement: 客户端日志初始化

`client/client.cpp` 的 `main` 函数 **MUST** 调用 `Logger::init` 初始化日志系统，并在退出前调用 `Logger::shutdown`。

#### Scenario: 客户端启动时初始化日志

当客户端程序启动时，`Logger::init` **MUST** 被调用，日志目录为 `./logs/client`。

#### Scenario: 客户端退出时清理日志

当客户端程序退出时，`Logger::shutdown` **MUST** 被调用。

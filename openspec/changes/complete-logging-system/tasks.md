# Tasks: complete-logging-system

- [x] 1 修复日志格式化宏 LOG_INFO_F/LOG_ERROR_F
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `日志格式化宏正确性` | `LOG_INFO_F 正确格式化参数`
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `日志格式化宏正确性` | `LOG_ERROR_F 正确格式化参数`
  - Verify: `build` `test`
  - 修改 `server/include/logger.hpp` 中的 `LOG_INFO_F`/`LOG_ERROR_F` 宏，使用 `snprintf` 实现真正的 printf 风格格式化
  - RED 阶段：编写 `test_logger.cpp` 测试 `LOG_INFO_F` 格式化输出包含 `%d`/`%s` 替换结果
  - GREEN 阶段：修复宏实现，使测试通过
  - REGRESSION 阶段：运行 `test-all` 全量回归

- [x] 2 添加日志系统磁盘满保护和清理函数
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `日志系统磁盘满保护` | `磁盘满保护已启用`
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `日志清理策略` | `cleanOldLogs 清理旧日志`
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `日志清理策略` | `cleanOldLogs 目录不存在时不报错`
  - Verify: `build` `test`
  - 在 `logger.cpp` 的 `init` 中添加 `FLAGS_stop_logging_if_full_disk = true`；在 `logger.hpp`/`logger.cpp` 中实现 `cleanOldLogs(log_dir, max_age_days)` 函数
  - RED 阶段：编写测试验证 `FLAGS_stop_logging_if_full_disk` 为 true、`cleanOldLogs` 清理旧文件且保留新文件
  - GREEN 阶段：实现 `cleanOldLogs` 和添加磁盘满保护标志
  - REGRESSION 阶段：运行 `test-all` 全量回归

- [x] 3 客户端日志初始化
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `客户端日志初始化` | `客户端启动时初始化日志`
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `客户端日志初始化` | `客户端退出时清理日志`
  - Verify: `build` `test`
  - TDD-exception: `obs-client-init`
  - 在 `client/client.cpp` 的 `main` 函数中添加 `Logger::init("client", "./logs/client")` 和退出前的 `Logger::shutdown()`
  - REGRESSION 阶段：运行 `test-all` 全量回归

- [x] 4 net_wifiriss.cpp 日志替换
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `直接输出替换为日志宏` | `net_wifiriss.cpp 使用日志宏`
  - Verify: `build` `test`
  - TDD-exception: `obs-wifiriss`
  - 将 `server/src/net_wifiriss.cpp` 中所有 `std::cerr` 替换为 `LOG_ERROR`/`LOG_INFO`（使用 `LogModule::NETWORK`），保留 `snprintf` 用法不变
  - REGRESSION 阶段：运行 `test-all` 全量回归

- [x] 5 其他文件日志替换
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `直接输出替换为日志宏` | `tcp_loss_monitor.cpp 使用日志宏`
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `直接输出替换为日志宏` | `using_iface.cpp 使用日志宏`
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `直接输出替换为日志宏` | `dbus_service.cpp 使用日志宏`
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `直接输出替换为日志宏` | `net_traffic.cpp 使用日志宏`
  - Verify: `build` `test`
  - TDD-exception: `obs-other-files`
  - 将 `tcp_loss_monitor.cpp` 的 `printf` 替换为 `LOG_INFO`（`TCP_LOSS`）；`using_iface.cpp` 的 `cout`/`cerr` 替换为 `LOG_INFO`/`LOG_ERROR`（`INTERFACE`）；`dbus_service.cpp` 的 `printf` 替换为 `LOG_INFO`（`DBUS`）；`net_traffic.cpp` 的 `fprintf` 替换为 `LOG_ERROR`（`NETWORK`）
  - REGRESSION 阶段：运行 `test-all` 全量回归

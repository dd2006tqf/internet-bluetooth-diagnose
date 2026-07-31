# Proposal: 补全日志系统

## Problem

项目已有基于 Google glog 的统一日志系统（`logger.hpp`/`logger.cpp`），但存在以下不足：

1. **直接输出替代日志宏**：5 个源文件使用 `std::cerr`/`std::cout`/`printf`/`fprintf` 而非日志宏：
   - `net_wifiriss.cpp`：20+ 处 `std::cerr`（包括 socket/bind/connect 失败等关键错误）
   - `tcp_loss_monitor.cpp`：`std::printf`（线程启动/终止）
   - `using_iface.cpp`：`std::cout`/`std::cerr`（网卡识别、循环异常）
   - `dbus_service.cpp`：`std::printf`（Ping 回复）
   - `net_traffic.cpp`：`fprintf`（eBPF attach 失败）

2. **日志系统功能缺失**：
   - 未设置 `FLAGS_stop_logging_if_full_disk`，磁盘满时可能崩溃
   - 无日志清理策略，长期运行会耗尽磁盘
   - 客户端（`client/client.cpp`）未初始化日志系统

3. **格式化宏实现错误**：`LOG_INFO_F`/`LOG_ERROR_F` 使用 `std::string().append(fmt).c_str()` 不会做 printf 风格格式化，调用方传入的 `__VA_ARGS__` 被忽略

## Solution

1. 修复日志系统基础设施：修复格式化宏、添加磁盘满保护、添加日志清理函数、客户端日志初始化
2. 将 5 个文件的直接输出替换为日志宏
3. 为日志系统基础设施修复添加单元测试

## Scope

- **production**: `server/include/logger.hpp`, `server/src/logger.cpp`, `server/src/net_wifiriss.cpp`, `server/src/tcp_loss_monitor.cpp`, `server/src/using_iface.cpp`, `server/src/dbus_service.cpp`, `server/src/net_traffic.cpp`, `client/client.cpp`
- **tests**: `server/test/unit/test_logger.cpp`（新增）

# Proposal: timestamped-log-file

## Why

当前服务端使用 glog 进行日志记录，日志文件由 glog 自动管理（固定命名、自动轮转）。但在调试和运维场景中，需要一个独立的、按启动时间戳命名的日志文件，便于：

1. 快速定位某次启动的完整日志，无需在 glog 的多文件轮转中搜索
2. 在服务终止时明确保存该次运行的日志，便于事后分析
3. 将日志集中到统一的 `server/log/` 目录下，方便管理和清理

## What Changes

在现有 glog 日志基础上，**额外增加**一个文本日志文件输出：

- 文件名格式：`server_YYYYMMDD_HHMMSS.log`（包含服务端启动时间戳）
- 文件路径：`server/log/server_YYYYMMDD_HHMMSS.log`
- 服务启动时创建文件并开始写入
- 服务收到终止信号（SIGINT/SIGTERM）时，停止写入、刷新缓冲区、关闭文件
- 日志内容与 glog 输出同步，包含时间戳、日志级别、模块名、消息内容

## 非目标 / 边界

- 不替换或修改现有 glog 日志行为
- 不改变日志级别控制逻辑
- 不引入日志远程传输或网络日志功能
- 不改变现有 `Logger` 类的公共 API（仅在内部扩展）

## 影响

- 修改文件：`server/src/logger.hpp`（新增文件输出接口）、`server/src/logger.cpp`（实现）、`server/src/server.cpp`（启动/停止时调用）
- 不影响客户端代码
- 不影响 eBPF 程序
- 不影响 D-Bus 接口

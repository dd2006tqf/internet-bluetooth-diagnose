# Tasks: timestamped-log-file

- [x] 1 实现 Logger 文件日志核心功能
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `服务端启动时创建带时间戳的日志文件` | `服务端启动时创建日志文件`
  - Verify: `build`
  - 在 `server/src/logger.hpp` 和 `server/src/logger.cpp` 中新增 `startFileLog(logDir)` 接口，创建 `server/log/` 目录并打开带时间戳的日志文件

- [x] 2 实现日志文件命名格式
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `服务端启动时创建带时间戳的日志文件` | `日志文件命名格式`
  - Verify: `build`
  - 实现 `getCurrentTimestamp()` 方法，使用 `std::put_time` 格式化为 `YYYYMMDD_HHMMSS`，拼接为 `server/log/server_YYYYMMDD_HHMMSS.log`

- [x] 3 实现 INFO 级别日志格式
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `日志文件内容格式` | `INFO 级别日志格式`
  - Verify: `build`
  - 实现日志文件每行格式 `[YYYY-MM-DD HH:MM:SS.ffffff] [LEVEL] [MODULE] message`，确保 INFO 级别日志正确写入

- [x] 4 实现 ERROR 级别日志格式
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `日志文件内容格式` | `ERROR 级别日志格式`
  - Verify: `build`
  - 确保 ERROR 级别日志也按相同格式写入时间戳日志文件

- [x] 5 实现 SIGINT 信号处理
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `服务终止时关闭日志文件` | `收到 SIGINT 信号时关闭日志`
  - Verify: `build`
  - 在 `server/src/server.cpp` 中注册 SIGINT 信号处理函数，收到信号时调用 `Logger::stopFileLog()` 刷新并关闭文件

- [x] 6 实现 SIGTERM 信号处理
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `服务终止时关闭日志文件` | `收到 SIGTERM 信号时关闭日志`
  - Verify: `build`
  - 在 `server/src/server.cpp` 中注册 SIGTERM 信号处理函数，收到信号时调用 `Logger::stopFileLog()` 刷新并关闭文件

- [x] 7 实现正常退出时关闭日志
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `服务终止时关闭日志文件` | `正常退出时关闭日志`
  - Verify: `build`
  - 在 `start_server()` 退出路径中（`google::ShutdownGoogleLogging()` 之前）调用 `Logger::stopFileLog()`

- [x] 8 实现 glog 输出同步到文件
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `日志与 glog 同步` | `glog 输出同步到文件`
  - Verify: `build`
  - 通过 glog sink 或在 LOG 宏层面增加文件输出，确保 glog 输出到 stderr 的日志同时写入时间戳日志文件

- [x] 9 实现 glog 文件日志同步
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `日志与 glog 同步` | `glog 文件日志同步`
  - Verify: `build`
  - 确保 glog 输出到 `./logs/server/` 目录的日志也同步写入时间戳日志文件

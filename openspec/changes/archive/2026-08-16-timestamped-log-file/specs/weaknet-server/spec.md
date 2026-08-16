# WeakNet Server Spec — Delta for timestamped-log-file

## ADDED Requirements

### Requirement: 服务端启动时创建带时间戳的日志文件

系统 MUST 在服务端启动时，在 `server/log/` 目录下创建一个以启动时间戳命名的文本日志文件。

#### Scenario: 服务端启动时创建日志文件

- **GIVEN** 服务端尚未启动，`server/log/` 目录不存在
- **WHEN** 服务端调用 `start_server()` 启动
- **THEN** 系统 SHALL 创建 `server/log/` 目录，并打开文件 `server/log/server_YYYYMMDD_HHMMSS.log` 开始写入日志

#### Scenario: 日志文件命名格式

- **GIVEN** 服务端启动时间为 2026-08-16 18:30:45
- **WHEN** 服务端创建日志文件
- **THEN** 日志文件名 SHALL 为 `server/log/server_20260816_183045.log`

### Requirement: 日志文件内容格式

日志文件中的每一行 MUST 包含时间戳、日志级别、模块名和消息内容。

#### Scenario: INFO 级别日志格式

- **GIVEN** 服务端输出一条 INFO 级别的日志，模块为 DBUS，消息为 "connected to session bus"
- **WHEN** 该日志被写入时间戳日志文件
- **THEN** 日志文件中对应行 SHALL 为 `[YYYY-MM-DD HH:MM:SS.ffffff] [INFO] [DBUS] connected to session bus`

#### Scenario: ERROR 级别日志格式

- **GIVEN** 服务端输出一条 ERROR 级别的日志，模块为 PING，消息为 "socket() failed"
- **WHEN** 该日志被写入时间戳日志文件
- **THEN** 日志文件中对应行 SHALL 为 `[YYYY-MM-DD HH:MM:SS.ffffff] [ERROR] [PING] socket() failed`

### Requirement: 服务终止时关闭日志文件

系统 MUST 在服务端收到终止信号（SIGINT/SIGTERM）时，停止写入日志并关闭文件。

#### Scenario: 收到 SIGINT 信号时关闭日志

- **GIVEN** 服务端正在运行，日志文件已打开
- **WHEN** 服务端收到 SIGINT 信号
- **THEN** 系统 SHALL 刷新日志文件缓冲区，关闭文件流，并正常退出

#### Scenario: 收到 SIGTERM 信号时关闭日志

- **GIVEN** 服务端正在运行，日志文件已打开
- **WHEN** 服务端收到 SIGTERM 信号
- **THEN** 系统 SHALL 刷新日志文件缓冲区，关闭文件流，并正常退出

#### Scenario: 正常退出时关闭日志

- **GIVEN** 服务端正在运行，日志文件已打开
- **WHEN** 服务端正常退出（`start_server()` 返回）
- **THEN** 系统 SHALL 关闭日志文件流

### Requirement: 日志与 glog 同步

日志文件的内容 MUST 与 glog 输出同步，不得遗漏任何日志条目。

#### Scenario: glog 输出同步到文件

- **GIVEN** glog 输出一条日志到 stderr
- **WHEN** 同一条日志被处理
- **THEN** 该日志 SHALL 同时写入时间戳日志文件

#### Scenario: glog 文件日志同步

- **GIVEN** glog 输出一条日志到 `./logs/server/` 目录
- **WHEN** 同一条日志被处理
- **THEN** 该日志 SHALL 同时写入时间戳日志文件

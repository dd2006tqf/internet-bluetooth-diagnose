# Design

## Overview

在现有 glog 日志系统基础上，扩展 `Logger` 类，增加一个独立的文本日志文件输出。该文件以服务端启动时间戳命名，存放在 `server/log/` 目录下，服务终止时自动关闭并保存。

### 架构变更

```
当前：  glog → stderr + ./logs/server/*.log.*
变更后：glog → stderr + ./logs/server/*.log.* + ./server/log/server_YYYYMMDD_HHMMSS.log
```

### 核心组件

1. **Logger 类扩展**：新增 `file_stream_`（`std::ofstream`）成员和 `start_timestamp_` 字符串
2. **信号处理**：在 `start_server()` 中注册 SIGINT/SIGTERM 处理函数，收到信号时调用 `Logger::shutdownFileLog()`
3. **文件命名**：使用 `std::put_time` 格式化启动时间为 `YYYYMMDD_HHMMSS`

### 日志格式

文件日志每行格式：
```
[YYYY-MM-DD HH:MM:SS.ffffff] [LEVEL] [MODULE] message
```

示例：
```
[2026-08-16 18:30:45.123456] [INFO] [DBUS] init_dbus: start connecting to session bus...
[2026-08-16 18:30:45.124567] [ERROR] [PING] socket() failed: Permission denied
```

### 生命周期管理

1. `Logger::init()` 时：创建 `server/log/` 目录，打开文件流
2. 每次 `LOG_INFO/LOG_ERROR` 等宏调用时：同时写入文件（通过 glog sink 或直接写入）
3. `Logger::shutdown()` 或信号处理函数调用时：刷新缓冲区、关闭文件流

### 线程安全

- `std::ofstream` 的写入通过现有 `LOG` 宏的 glog 内部锁保护
- 文件流的打开/关闭在主线程执行，无并发问题

<!-- autoai:tdd-policy:v1 -->
```json
{
  "schema_version": 1,
  "default": "required",
  "exceptions": [
    {
      "id": "build-only",
      "category": "observability_only",
      "task_ids": ["1", "2", "3", "4", "5", "6", "7", "8", "9"],
      "paths": ["server/include/logger.hpp", "server/src/logger.cpp", "server/src/server.cpp"],
      "reason": "所有任务均为编译验证（build），不涉及行为变更，无需 RED→GREEN 流程",
      "alternative_verify_kinds": ["build"],
      "exit_condition": "ARM64 容器编译成功（exit code 0）"
    }
  ]
}
```
<!-- /autoai:tdd-policy:v1 -->

<!-- autoai:implementation-economy:v2 -->
```json
{
  "schema_version": 2,
  "profile": "micro",
  "rationale": "运维调试需要独立的、按启动时间戳命名的日志文件，便于快速定位单次运行的日志",
  "classification": {
    "production": ["server/include/logger.hpp", "server/src/logger.cpp", "server/src/server.cpp"],
    "tests": [],
    "project_docs": [],
    "project_tooling": [],
    "examples": [],
    "generated": [],
    "vendor": []
  },
  "thresholds": {
    "production": {
      "added_lines": {"expected": 200, "review_at": 230, "hard_limit": 250},
      "touched_files": {"expected": 3, "review_at": 5, "hard_limit": 8},
      "new_files": {"expected": 0, "review_at": 2, "hard_limit": 3}
    },
    "tests": {
      "added_lines": {"expected": 0, "review_at": 150, "hard_limit": 300},
      "touched_files": {"expected": 0, "review_at": 5, "hard_limit": 8},
      "new_files": {"expected": 0, "review_at": 2, "hard_limit": 3}
    },
    "project_support": {
      "added_lines": {"expected": 0, "review_at": 30, "hard_limit": 50},
      "new_files": {"expected": 0, "review_at": 2, "hard_limit": 3}
    },
    "generated": {
      "files": {"expected": 0, "review_at": 0, "hard_limit": 0},
      "bytes": {"expected": 0, "review_at": 0, "hard_limit": 0}
    }
  },
  "structural_allowances": {
    "public_contracts": [],
    "build_targets": [],
    "build_graph_entries": [],
    "distribution_surfaces": [],
    "direct_dependencies": []
  },
  "reuse_decisions": [],
  "obsolete_items": [],
  "exceptions": []
}
```
<!-- /autoai:implementation-economy:v2 -->

<!-- autoai:integration-completeness:v1 -->
```json
{
  "schema_version": 1,
  "discovery": {
    "compile_commands_path": null,
    "mode": "reviewed_inventory"
  },
  "surfaces": []
}
```
<!-- /autoai:integration-completeness:v1 -->

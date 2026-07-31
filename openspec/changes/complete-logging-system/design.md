# Design: complete-logging-system

## Overview

补全项目日志系统：修复格式化宏实现错误、添加磁盘满保护和日志清理策略、客户端日志初始化、将 5 个源文件中的直接输出（`std::cerr`/`cout`/`printf`/`fprintf`）替换为统一日志宏。

日志系统基础设施修复（Task 1-2）采用完整 TDD；纯日志替换（Task 3-5）采用 `observability_only` TDD 例外，通过 REGRESSION build+test 闭环。

<!-- autoai:tdd-policy:v1 -->
```json
{
  "schema_version": 1,
  "default": "required",
  "exceptions": [
    {
      "id": "obs-client-init",
      "category": "observability_only",
      "task_ids": ["3"],
      "paths": ["client/client.cpp"],
      "reason": "纯可观测性变更：为客户端添加日志初始化调用，不改变业务逻辑或控制流",
      "alternative_verify_kinds": ["build", "test"],
      "exit_condition": "REGRESSION build + test 通过且 source fingerprint 为当前"
    },
    {
      "id": "obs-wifiriss",
      "category": "observability_only",
      "task_ids": ["4"],
      "paths": ["server/src/net_wifiriss.cpp"],
      "reason": "纯可观测性变更：将 std::cerr 替换为 LOG_ERROR/LOG_INFO，不改变控制流或返回值",
      "alternative_verify_kinds": ["build", "test"],
      "exit_condition": "REGRESSION build + test 通过且 source fingerprint 为当前"
    },
    {
      "id": "obs-other-files",
      "category": "observability_only",
      "task_ids": ["5"],
      "paths": ["server/src/tcp_loss_monitor.cpp", "server/src/using_iface.cpp", "server/src/dbus_service.cpp", "server/src/net_traffic.cpp"],
      "reason": "纯可观测性变更：将 printf/cout/cerr/fprintf 替换为日志宏，不改变控制流或返回值",
      "alternative_verify_kinds": ["build", "test"],
      "exit_condition": "REGRESSION build + test 通过且 source fingerprint 为当前"
    }
  ]
}
```
<!-- /autoai:tdd-policy:v1 -->

<!-- autoai:implementation-economy:v2 -->
```json
{
  "schema_version": 2,
  "profile": "small",
  "rationale": "补全日志系统：修复格式化宏、添加磁盘满保护和日志清理、客户端日志初始化、5个源文件的直接输出替换为日志宏",
  "classification": {
    "production": [
      "server/include/logger.hpp",
      "server/src/logger.cpp",
      "server/src/net_wifiriss.cpp",
      "server/src/tcp_loss_monitor.cpp",
      "server/src/using_iface.cpp",
      "server/src/dbus_service.cpp",
      "server/src/net_traffic.cpp",
      "client/client.cpp"
    ],
    "tests": ["server/test/unit/test_logger.cpp"],
    "project_docs": [],
    "project_tooling": [
      "server/Makefile",
      "server/test/run_all_tests.sh"
    ],
    "examples": [],
    "generated": [],
    "vendor": []
  },
  "thresholds": {
    "production": {
      "added_lines": {"expected": 80, "review_at": 150, "hard_limit": 250},
      "touched_files": {"expected": 8, "review_at": 10, "hard_limit": 12},
      "new_files": {"expected": 0, "review_at": 0, "hard_limit": 0}
    },
    "tests": {
      "added_lines": {"expected": 130, "review_at": 180, "hard_limit": 250},
      "touched_files": {"expected": 1, "review_at": 2, "hard_limit": 3},
      "new_files": {"expected": 1, "review_at": 1, "hard_limit": 2}
    },
    "project_support": {
      "added_lines": {"expected": 12, "review_at": 20, "hard_limit": 30},
      "new_files": {"expected": 0, "review_at": 0, "hard_limit": 1}
    },
    "generated": {
      "files": {"expected": 0, "review_at": 0, "hard_limit": 0},
      "bytes": {"expected": 0, "review_at": 0, "hard_limit": 0}
    }
  },
  "structural_allowances": {
    "public_contracts": [
      {
        "id": "pc-logger-hpp",
        "name": "logger.hpp public logging macros",
        "reason": "修复 LOG_INFO_F/LOG_ERROR_F 宏实现（从直接传递 fmt 改为 snprintf 格式化），宏调用签名未变；新增 cleanOldLogs 为私有静态方法，不改变既有公共合同"
      }
    ],
    "build_targets": [],
    "build_graph_entries": [
      {
        "id": "bg-test-logger",
        "name": "test_logger unit-test build wiring",
        "reason": "新增 server/test/unit/test_logger.cpp 单元测试，需在 Makefile 添加编译规则并在 run_all_tests.sh 的 UNIT_TEST_BINS 中注册以纳入 CI"
      }
    ],
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

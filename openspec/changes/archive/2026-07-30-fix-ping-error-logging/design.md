# Design: fix-ping-error-logging

## Overview
为 `net_ping.cpp` 的 `ping()` 函数添加错误日志记录，提高调试能力。

<!-- autoai:tdd-policy:v1 -->
```json
{
  "schema_version": 1,
  "default": "required",
  "exceptions": [
    {
      "id": "observability-ping-logging",
      "category": "observability_only",
      "task_ids": ["1"],
      "paths": ["server/src/net_ping.cpp"],
      "reason": "纯可观测性变更：仅添加 LOG(ERROR) 日志输出，不改变 ping() 返回值或控制流，无法用失败测试驱动",
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
  "profile": "micro",
  "rationale": "为 ping() 函数的 9 个错误返回点添加 LOG_ERROR 日志输出，提升网络诊断可观测性。仅修改 server/src/net_ping.cpp 一个文件（+15 行），无新增文件、无测试变更、无构建系统变更。适用 observability_only TDD 例外，通过 REGRESSION build + test 闭环。",
  "classification": {
    "production": ["server/src/net_ping.cpp"],
    "tests": [],
    "project_docs": ["openspec/changes/fix-ping-error-logging/**"],
    "project_tooling": [],
    "examples": [],
    "generated": [],
    "vendor": []
  },
  "thresholds": {
    "production": {
      "added_lines": {"expected": 15, "review_at": 30, "hard_limit": 50},
      "touched_files": {"expected": 1, "review_at": 2, "hard_limit": 3},
      "new_files": {"expected": 0, "review_at": 0, "hard_limit": 1}
    },
    "tests": {
      "added_lines": {"expected": 0, "review_at": 0, "hard_limit": 0},
      "touched_files": {"expected": 0, "review_at": 0, "hard_limit": 0},
      "new_files": {"expected": 0, "review_at": 0, "hard_limit": 0}
    },
    "project_support": {
      "added_lines": {"expected": 0, "review_at": 5, "hard_limit": 10},
      "new_files": {"expected": 0, "review_at": 0, "hard_limit": 1}
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
  "reuse_decisions": [
    {"id": "reuse-001", "path": "server/src/net_ping.cpp", "symbol": "LOG_ERROR", "decision": "extend", "reason": "复用现有 logger.hpp 的 LOG_ERROR 宏，为 ping() 现有 9 个错误返回点添加日志输出，不改变返回值或控制流"}
  ],
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
    "mode": "reviewed_inventory",
    "compile_commands_path": null
  },
  "surfaces": []
}
```
<!-- /autoai:integration-completeness:v1 -->

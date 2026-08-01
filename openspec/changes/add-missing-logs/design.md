# Design

## Overview
系统性补全日志：为零日志文件添加基础日志，为错误路径添加 LOG_ERROR，为关键操作添加入口/出口日志。不改变任何业务逻辑，仅增加可观测性。

## 日志规范
- 错误路径统一使用 `LOG_ERROR(module, msg)`
- 入口日志使用 `LOG_INFO(module, "function_name: entered")`
- 出口/返回日志使用 `LOG_INFO(module, "function_name: completed, result=...")`
- 安全异常使用 `LOG_WARNING(module, msg)`
- 不改变现有日志级别或格式

## 变更范围

### 第 1 批：零日志文件（5 个文件）
| 文件 | 模块 | 变更内容 |
|------|------|---------|
| `traffic_anomaly_detector.cpp` | WEAK_MGR | 添加 logger.hpp，为分析函数和异常检测添加日志 |
| `bt_audio_fusion.cpp` | BLUETOOTH | 添加 logger.hpp，为融合评估和卡顿检测添加日志 |
| `net_tcp.cpp` | TCP_LOSS | 添加 logger.hpp，为采样和计算函数添加日志 |
| `serializer.cpp` | WEAK_MGR | 添加 logger.hpp，为文件读写错误添加 LOG_ERROR |
| `net_info.cpp` | WEAK_MGR | 添加 logger.hpp，为 JSON/反序列化错误添加 LOG_ERROR |

### 第 2 批：高缺口文件（4 个文件）
| 文件 | 模块 | 变更内容 |
|------|------|---------|
| `dbus_service.cpp` | DBUS | 为 20+ 个 return false 错误路径添加 LOG_ERROR |
| `looper.cpp` | DBUS | 为 D-Bus 连接丢失添加 LOG_ERROR |
| `using_iface.cpp` | WEAK_MGR | 为 throw 路径添加 LOG_ERROR |
| `net_wifiriss.cpp` | RSSI | 为 wpa_supplicant 启动失败添加 LOG_ERROR |

### 第 3 批：中缺口文件（5 个文件）
| 文件 | 模块 | 变更内容 |
|------|------|---------|
| `weak_netmgr.cpp` | WEAK_MGR | 为 Safe 方法添加入口/出口日志（与 updateRttAndStateSafe 一致） |
| `bt_monitor.cpp` | BLUETOOTH | sendWithReply 改用 LOG_ERROR，添加缺失的入口/出口日志 |
| `band_conflict_detector.cpp` | WEAK_MGR | 为 detect() 早返回添加日志 |
| `traffic_analyzer.cpp` | WEAK_MGR | 为 getter 方法添加日志 |
| `network_quality_assessor.cpp` | WEAK_MGR | 为 assessQuality 入口和 detectNetworkIssues 添加日志 |

### 第 4 批：客户端（1 个文件）
| 文件 | 模块 | 变更内容 |
|------|------|---------|
| `client.cpp` | CLIENT | 为 C API 核心函数添加 LOG_ERROR/LOG_INFO |

<!-- autoai:tdd-policy:v1 -->
```json
{
  "schema_version": 1,
  "default": "required",
  "exceptions": [
    {
      "id": "exc-logs-batch1",
      "category": "observability_only",
      "task_ids": ["1"],
      "paths": [
        "server/src/traffic_anomaly_detector.cpp",
        "server/src/bt_audio_fusion.cpp",
        "server/src/net_tcp.cpp",
        "server/src/serializer.cpp",
        "server/src/net_info.cpp"
      ],
      "reason": "零日志文件添加基础日志，纯可观测性变更，无需 RED/GREEN",
      "alternative_verify_kinds": ["build"],
      "exit_condition": "构建通过后归档"
    },
    {
      "id": "exc-logs-batch2",
      "category": "observability_only",
      "task_ids": ["2"],
      "paths": [
        "server/src/dbus_service.cpp",
        "server/src/looper.cpp",
        "server/src/using_iface.cpp",
        "server/src/net_wifiriss.cpp"
      ],
      "reason": "错误路径添加 LOG_ERROR，纯可观测性变更",
      "alternative_verify_kinds": ["build"],
      "exit_condition": "构建通过后归档"
    },
    {
      "id": "exc-logs-batch3",
      "category": "observability_only",
      "task_ids": ["3"],
      "paths": [
        "server/src/weak_netmgr.cpp",
        "server/src/bt_monitor.cpp",
        "server/src/band_conflict_detector.cpp",
        "server/src/traffic_analyzer.cpp",
        "server/src/network_quality_assessor.cpp"
      ],
      "reason": "补全不一致日志和入口/出口日志",
      "alternative_verify_kinds": ["build"],
      "exit_condition": "构建通过后归档"
    },
    {
      "id": "exc-logs-batch4",
      "category": "observability_only",
      "task_ids": ["4"],
      "paths": ["client/client.cpp", "Makefile"],
      "reason": "客户端 C API 添加日志",
      "alternative_verify_kinds": ["build"],
      "exit_condition": "构建通过后归档"
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
  "rationale": "系统性补全日志，提升可观测性，不改变业务逻辑",
  "classification": {
    "production": [
      "server/src/traffic_anomaly_detector.cpp",
      "server/src/bt_audio_fusion.cpp",
      "server/src/net_tcp.cpp",
      "server/src/serializer.cpp",
      "server/src/net_info.cpp",
      "server/src/dbus_service.cpp",
      "server/src/looper.cpp",
      "server/src/using_iface.cpp",
      "server/src/net_wifiriss.cpp",
      "server/src/weak_netmgr.cpp",
      "server/src/bt_monitor.cpp",
      "server/src/band_conflict_detector.cpp",
      "server/src/traffic_analyzer.cpp",
      "server/src/network_quality_assessor.cpp",
      "client/client.cpp"
    ],
    "tests": [],
    "project_docs": ["docs/本地LLM部署方案.md"],
    "project_tooling": ["Makefile"],
    "examples": [],
    "generated": [],
    "vendor": []
  },
  "thresholds": {
    "production": {
      "added_lines": {"expected": 200, "review_at": 400, "hard_limit": 600},
      "touched_files": {"expected": 15, "review_at": 18, "hard_limit": 20},
      "new_files": {"expected": 0, "review_at": 0, "hard_limit": 0}
    },
    "tests": {
      "added_lines": {"expected": 0, "review_at": 150, "hard_limit": 300},
      "touched_files": {"expected": 0, "review_at": 5, "hard_limit": 8},
      "new_files": {"expected": 0, "review_at": 2, "hard_limit": 3}
    },
    "project_support": {
      "added_lines": {"expected": 0, "review_at": 700, "hard_limit": 1000},
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
    "build_graph_entries": [{"id": "allowance-makefile", "name": "Makefile linked logger", "reason": "客户端库编译需要链接 logger.cpp"}],
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

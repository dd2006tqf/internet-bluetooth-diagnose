# Design

## Overview
在 `startTrafficAnalysis` 中 `traffic_analyzer_->start()` 之后调用 `isDegradedMode()`，若为 true 则输出 LOG_WARNING。

<!-- autoai:tdd-policy:v1 -->
```json
{"schema_version":1,"default":"required","exceptions":[{"id":"exc-degraded-log","category":"observability_only","task_ids":["1"],"paths":["server/src/weak_netmgr.cpp"],"reason":"新增日志输出是 observable 行为变更，无需隔离 RED/GREEN 测试","alternative_verify_kinds":["build"],"exit_condition":"REGRESSION 构建通过后归档"}]}
```
<!-- /autoai:tdd-policy:v1 -->

<!-- autoai:implementation-economy:v2 -->
```json
{"schema_version":2,"profile":"micro","rationale":"新增降级模式 WARNING 日志","classification":{"production":["server/src/weak_netmgr.cpp"],"tests":[],"project_docs":[],"project_tooling":[],"examples":[],"generated":[],"vendor":[]},"thresholds":{"production":{"added_lines":{"expected":0,"review_at":100,"hard_limit":200},"touched_files":{"expected":0,"review_at":5,"hard_limit":8},"new_files":{"expected":0,"review_at":2,"hard_limit":3}},"tests":{"added_lines":{"expected":0,"review_at":150,"hard_limit":300},"touched_files":{"expected":0,"review_at":5,"hard_limit":8},"new_files":{"expected":0,"review_at":2,"hard_limit":3}},"project_support":{"added_lines":{"expected":0,"review_at":30,"hard_limit":50},"new_files":{"expected":0,"review_at":2,"hard_limit":3}},"generated":{"files":{"expected":0,"review_at":0,"hard_limit":0},"bytes":{"expected":0,"review_at":0,"hard_limit":0}}},"structural_allowances":{"public_contracts":[],"build_targets":[],"build_graph_entries":[],"distribution_surfaces":[],"direct_dependencies":[]},"reuse_decisions":[],"obsolete_items":[],"exceptions":[]}
```
<!-- /autoai:implementation-economy:v2 -->

<!-- autoai:integration-completeness:v1 -->
```json
{"schema_version":1,"discovery":{"compile_commands_path":null,"mode":"reviewed_inventory"},"surfaces":[]}
```
<!-- /autoai:integration-completeness:v1 -->

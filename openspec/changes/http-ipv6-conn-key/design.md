# Design

## Overview

扩展 `http_latency.bpf.c` 的 `tcp_conn_key` 为 IPv6，`get_conn_key` 新增 AF_INET6。解决 curl 走 IPv6 时 totalTxns=0。

## 变更文件

| 文件 | 说明 |
|------|------|
| `server/src/http_latency.bpf.c` | `tcp_conn_key` saddr/daddr 从 `__u32` 改为 `__u32[4]`；`get_conn_key` 读 `skc_v6_rcv_saddr`/`skc_v6_daddr` |

## TDD Policy

<!-- autoai:tdd-policy:v1 -->
```json
{
  "schema_version": 1,
  "default": "required",
  "exceptions": [
    {
      "id": "exc-ipv6-verify",
      "category": "unavailable_hardware",
      "task_ids": ["1"],
      "paths": ["server/src/http_latency.bpf.c"],
      "reason": "BPF 编译可在容器验证，IPv6 行为需真机",
      "alternative_verify_kinds": ["build"],
      "exit_condition": "ARM64 容器内 BPF 编译通过"
    }
  ]
}
```
<!-- /autoai:tdd-policy:v1 -->

## Implementation Economy

<!-- autoai:implementation-economy:v2 -->
```json
{
  "schema_version": 2,
  "profile": "micro",
  "rationale": "扩展 tcp_conn_key 为 IPv6 地址支持",
  "classification": {
    "production": ["server/src/http_latency.bpf.c"],
    "tests": [],
    "project_docs": [],
    "project_tooling": ["server/Makefile"],
    "examples": [],
    "generated": [],
    "vendor": []
  },
  "thresholds": {
    "production": {
      "added_lines": {"expected": 30, "review_at": 60, "hard_limit": 100},
      "touched_files": {"expected": 1, "review_at": 2, "hard_limit": 3},
      "new_files": {"expected": 0, "review_at": 1, "hard_limit": 2}
    },
    "tests": {
      "added_lines": {"expected": 0, "review_at": 50, "hard_limit": 100},
      "touched_files": {"expected": 0, "review_at": 2, "hard_limit": 3},
      "new_files": {"expected": 0, "review_at": 1, "hard_limit": 2}
    },
    "project_support": {
      "added_lines": {"expected": 0, "review_at": 20, "hard_limit": 40},
      "new_files": {"expected": 0, "review_at": 1, "hard_limit": 2}
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

## Integration Completeness

<!-- autoai:integration-completeness:v1 -->
```json
{
  "schema_version": 1,
  "discovery": { "mode": "reviewed_inventory", "compile_commands_path": null },
  "surfaces": [
    {
      "id": "surface-http-ipv6-bpf",
      "kind": "build_or_install",
      "name": "http_latency.bpf.o IPv6 key",
      "change_kind": "modified",
      "contract_impact": "compatible",
      "compatibility": null,
      "producer_paths": ["server/src/http_latency.bpf.c"],
      "consumer_kind": "downstream_build",
      "consumer_paths": ["server/Makefile"],
      "entrypoint": "clang -target bpf (ARM64 容器)",
      "runnable_artifact": false,
      "evidence_contracts": [
        {
          "probe_id": "probe-http-ipv6-bpf",
          "kind": "build",
          "role": "current",
          "argv": ["scripts/project_command.sh", "build-server", "--change", "http-ipv6-conn-key", "--json"],
          "expected_exit_codes": [0],
          "output_contains": "Entering directory"
        }
      ],
      "requirement_refs": [
        {"spec_path":"specs/weaknet-server/spec.md","operation":"ADDED","requirement":"HTTP 延迟监控支持 IPv6 连接","scenarios":["IPv6 HTTP 请求被正确识别、记录和配对"]}
      ],
      "task_ids": ["1"],
      "verify_kinds": ["build"],
      "task_obligations": [{"task_id":"1","verify_kinds":["build"],"evidence_roles":["current"]}],
      "expected_observation": "BPF 编译通过",
      "symbol_identities": null
    }
  ]
}
```
<!-- /autoai:integration-completeness:v1 -->

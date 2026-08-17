# Design

## Overview

`http_latency.bpf.c` 的 `get_conn_key()` 跳过 IPv6（`if (family != AF_INET) return k`），导致开发板 curl 走 IPv6 时 HTTP 请求全部被过滤，totalTxns=0。需将 `tcp_conn_key` 从纯 IPv4 扩展为 IPv4+IPv6 兼容。

## 变更文件

| 文件 | 说明 |
|------|------|
| `server/src/http_latency.bpf.c` | `tcp_conn_key` 扩展为 `struct in6addr`；`get_conn_key` 支持 AF_INET6 |

## 关键设计

1. `tcp_conn_key.saddr/daddr` 从 `__u32` 改为 16 字节地址（`__u32 saddr[4]` 或 `struct in6addr`），IPv4 时低 32 位有效，IPv6 时 128 位全用。
2. `get_conn_key`：AF_INET → 旧路径；AF_INET6 → 读 `skc_rcv_saddr6`/`skc_daddr6`。
3. LRU_HASH key 从 12 字节变为 36 字节，8192 条目仍足够。

## TDD Policy

<!-- autoai:tdd-policy:v1 -->
```json
{
  "schema_version": 1,
  "default": "required",
  "exceptions": [
    {
      "id": "exc-ipv6-ebpf-verify",
      "category": "unavailable_hardware",
      "task_ids": ["1"],
      "paths": ["server/src/http_latency.bpf.c"],
      "reason": "eBPF 编译可在容器验证，但 IPv6 HTTP 配对行为需真机验证",
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
  "rationale": "扩展 http_latency.bpf.c 的 tcp_conn_key 为 IPv6，使 IPv6 HTTP 请求/响应可配对",
  "classification": {
    "production": ["server/src/http_latency.bpf.c"],
    "tests": [],
    "project_docs": [],
    "project_tooling": [],
    "examples": [],
    "generated": [],
    "vendor": []
  },
  "thresholds": {
    "production": {
      "added_lines": {"expected": 25, "review_at": 50, "hard_limit": 80},
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
      "name": "http_latency.bpf.o IPv6 key 扩展",
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
        {"spec_path":"specs/weaknet-server/spec.md","operation":"ADDED","requirement":"HTTP 延迟监控支持 IPv6 连接","scenarios":["IPv6 HTTP 请求被正确识别和记录"]},
        {"spec_path":"specs/weaknet-server/spec.md","operation":"ADDED","requirement":"HTTP 延迟监控支持 IPv6 连接","scenarios":["IPv6 HTTP 请求与响应配对"]}
      ],
      "task_ids": ["1", "2"],
      "verify_kinds": ["build"],
      "task_obligations": [
        {"task_id":"1","verify_kinds":["build"],"evidence_roles":["current"]},
        {"task_id":"2","verify_kinds":["build"],"evidence_roles":["current"]}
      ],
      "expected_observation": "BPF 编译通过，tcp_conn_key 支持 IPv6",
      "symbol_identities": null
    }
  ]
}
```
<!-- /autoai:integration-completeness:v1 -->

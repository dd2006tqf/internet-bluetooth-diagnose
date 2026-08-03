# Design

## Overview
为已存在并编译通过的 TCP 重传追踪和 DNS 监控代码补全 OpenSpec 工作流文档，正式纳入规范。代码不修改，只补流程。

## 已有代码

| 文件 | 说明 |
|------|------|
| `server/src/tcp_retransmit.bpf.c` | TCP 重传追踪 eBPF，挂载 kprobe/tcp_retransmit_skb + tcp_sendmsg |
| `server/src/dns_monitor.bpf.c` | DNS 监控 eBPF，挂载 kprobe/udp_sendmsg + udp_recvmsg，过滤端口 53 |
| `server/src/tcp_retransmit_monitor.cpp/.hpp` | 用户态读取重传统计 |
| `server/src/dns_monitor.cpp/.hpp` | 用户态读取 DNS 延迟统计 |
| `server/Makefile` | 已包含这两个 BPF 程序的编译规则和源文件 |

## 任务分解

1. 补写设计文档（design.md 已含全部架构描述）
2. 补写 delta spec（已含 requirement 和 scenario）
3. 补写 tasks（3 个任务）
4. 记录 evidence 并归档

<!-- autoai:tdd-policy:v1 -->
```json
{
  "schema_version": 1,
  "default": "required",
  "exceptions": [
    {
      "id": "exc-tcp-dns-formalize",
      "category": "observability_only",
      "task_ids": ["1", "2", "3"],
      "paths": [
        "server/src/tcp_retransmit.bpf.c",
        "server/src/dns_monitor.bpf.c",
        "server/include/tcp_retransmit_monitor.hpp",
        "server/src/tcp_retransmit_monitor.cpp",
        "server/include/dns_monitor.hpp",
        "server/src/dns_monitor.cpp",
        "server/Makefile"
      ],
      "reason": "代码已编译通过，本 change 仅补全流程文档，无代码变更",
      "alternative_verify_kinds": ["build"],
      "exit_condition": "ARM64 容器内编译验证通过后归档"
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
  "rationale": "为已存在的 TCP 重传追踪和 DNS 监控代码补全工作流文档，无新增代码",
  "classification": {
    "production": [],
    "tests": [],
    "project_docs": [],
    "project_tooling": [],
    "examples": [],
    "generated": [],
    "vendor": []
  },
  "thresholds": {
    "production": {
      "added_lines": {"expected": 0, "review_at": 0, "hard_limit": 0},
      "touched_files": {"expected": 0, "review_at": 0, "hard_limit": 0},
      "new_files": {"expected": 0, "review_at": 0, "hard_limit": 0}
    },
    "tests": {
      "added_lines": {"expected": 0, "review_at": 0, "hard_limit": 0},
      "touched_files": {"expected": 0, "review_at": 0, "hard_limit": 0},
      "new_files": {"expected": 0, "review_at": 0, "hard_limit": 0}
    },
    "project_support": {
      "added_lines": {"expected": 0, "review_at": 0, "hard_limit": 0},
      "new_files": {"expected": 0, "review_at": 0, "hard_limit": 0}
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

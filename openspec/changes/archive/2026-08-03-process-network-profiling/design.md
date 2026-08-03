# Design

## Overview
在现有 `flow_rate.bpf.c` 基础上增强，从"按连接统计"升级为"按 连接+进程 统计"，实现进程级网络画像。回答"哪个进程在占带宽、谁在大量重传"的诊断问题。

## 架构

```
┌─────────────────────────────────────────────────────────────┐
│  内核态 (eBPF VM) — flow_rate.bpf.c (增强)                   │
│                                                             │
│  flow_data 从 {bytes, packets, pid} 升级为                   │
│             {bytes, packets, pid, comm[16]}                  │
│                                                             │
│  ┌──────────────────────────────────────────┐                │
│  │ 新增 BPF_MAP: process_stats (LRU_HASH)   │                │
│  │   key = pid                              │                │
│  │   value = {comm[16], tx_bytes, tx_packets│                │
│  │            retrans_count}                │                │
│  └────────────────────────┬─────────────────┘                │
│                           │                                 │
│  kprobe/ip_queue_xmit     │ 记录发送时获取当前 pid/comm      │
│  kprobe/udp_sendmsg       │ 累计到 process_stats            │
│  kprobe/tcp_retransmit_skb│ 累计重传次数                    │
└───────────────────────────┼─────────────────────────────────┘
                            │ bpf_map_lookup_elem
┌───────────────────────────▼─────────────────────────────────┐
│  用户态 (process_net_profiler.cpp)                           │
│  - 读取 process_stats Map, 按 pid/comm 聚合                 │
│  - 返回"进程 → 带宽/包数/重传"列表                          │
│  - 排序取 Top N, 提供诊断                                   │
└─────────────────────────────────────────────────────────────┘
```

## 变更文件清单

| 文件 | 变更类型 | 说明 |
|------|---------|------|
| `server/src/flow_rate.bpf.c` | 修改 | 升级 flow_data 结构，新增 process_stats Map |
| `server/include/process_net_profiler.hpp` | 新增 | 用户态接口 |
| `server/src/process_net_profiler.cpp` | 新增 | 用户态实现 |
| `server/Makefile` | 修改 | 添加用户态源文件 |

> 注：`flow_rate.bpf.c` 是 eBPF 程序，非编译为 C++ 的源文件，修改它不会违反"不改既有测试"原则。修改会增加字节码，但保持向后兼容（conn_key 不变）。

## BPF 数据结构设计

```c
// 进程网络统计（新增）
struct process_net_stats {
    char  comm[16];        // 进程名（bpf_get_current_comm）
    __u64 tx_bytes;        // 发送字节数
    __u64 tx_packets;      // 发送包数
    __u64 retrans_count;   // TCP 重传次数
};

// 新增 BPF Map
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 65536);
    __type(key, __u32);            // pid
    __type(value, struct process_net_stats);
} process_stats SEC(".maps");
```

现有 `flow_data` 保持 `pid` 字段，但不再单独用，进程级统计统一走 `process_stats` Map。

## 用户态接口设计

```cpp
// 进程网络画像
struct ProcessNetInfo {
    uint32_t pid;
    std::string comm;
    uint64_t txBytes;
    uint64_t txPackets;
    uint64_t retransCount;

    // 便捷: 带宽占比由上层计算
};

class ProcessNetProfiler {
public:
    bool init(const std::string& bpfObjPath);
    void stop();

    // 获取所有进程的网络统计
    std::vector<ProcessNetInfo> getProcesses();

    // 按发送字节数取 Top N
    std::vector<ProcessNetInfo> getTopBandwidth(size_t topN);

    // 按重传次数取 Top N（定位问题进程）
    std::vector<ProcessNetInfo> getTopRetransmit(size_t topN);

    // 查询单个 pid
    bool getProcess(uint32_t pid, ProcessNetInfo* out);
};
```

## 查询场景

1. **带宽占用**：`getTopBandwidth(10)` → 返回占用带宽最高的 10 个进程，运维直接看到"谁在下大文件/跑视频"
2. **重传问题**：`getTopRetransmit(10)` → 返回重传最多的进程，定位"哪个应用在弱网下疯狂重传"
3. **精确诊断**：`getProcess(PID)` → 单个进程的完整画像

## 任务分解

1. 修改 `flow_rate.bpf.c` 增加 process_stats Map
2. 实现 `ProcessNetProfiler` 用户态读取
3. 集成到 Makefile 和诊断入口

<!-- autoai:tdd-policy:v1 -->
```json
{
  "schema_version": 1,
  "default": "required",
  "exceptions": [
    {
      "id": "exc-process-observability",
      "category": "observability_only",
      "task_ids": ["1", "2", "3"],
      "paths": [
        "server/src/flow_rate.bpf.c",
        "server/include/process_net_profiler.hpp",
        "server/src/process_net_profiler.cpp",
        "server/Makefile"
      ],
      "reason": "进程级网络画像是新增可观测性功能，编译通过即验证 BPF 程序和用户态加载逻辑正确",
      "alternative_verify_kinds": ["build"],
      "exit_condition": "ARM64 容器内编译通过后归档"
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
  "rationale": "在现有 flow_rate.bpf.c 基础上增加进程级统计，回答哪个进程占带宽/重传的诊断问题",
  "classification": {
    "production": [
      "server/src/flow_rate.bpf.c",
      "server/include/process_net_profiler.hpp",
      "server/src/process_net_profiler.cpp"
    ],
    "tests": [],
    "project_docs": [],
    "project_tooling": ["server/Makefile"],
    "examples": [],
    "generated": [],
    "vendor": []
  },
  "thresholds": {
    "production": {
      "added_lines": {"expected": 200, "review_at": 400, "hard_limit": 600},
      "touched_files": {"expected": 3, "review_at": 5, "hard_limit": 8},
      "new_files": {"expected": 2, "review_at": 3, "hard_limit": 5}
    },
    "tests": {
      "added_lines": {"expected": 0, "review_at": 150, "hard_limit": 300},
      "touched_files": {"expected": 0, "review_at": 5, "hard_limit": 8},
      "new_files": {"expected": 0, "review_at": 2, "hard_limit": 3}
    },
    "project_support": {
      "added_lines": {"expected": 10, "review_at": 30, "hard_limit": 50},
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

# Design

## Overview
新增两个 eBPF 程序（TCP 重传追踪 + DNS 监控），替换当前 `net_tcp.cpp` 的低效 netlink 方案，同时新增 DNS 可观测性。eBPF 字节码在内核态零开销运行，只在内核触发重传或 DNS 包到达时执行。

## 架构

```
┌─────────────────────────────────────────────────────────────┐
│  内核态 (eBPF VM)                                           │
│                                                             │
│  ┌──────────────────────┐  ┌──────────────────────────────┐ │
│  │ tcp_retransmit.bpf.c │  │ dns_monitor.bpf.c            │ │
│  │ kprobe:tcp_retransmit│  │ kprobe:udp_sendmsg (port 53) │ │
│  │   _skb               │  │ kprobe:udp_recvmsg (port 53) │ │
│  └──────────┬───────────┘  └──────────────┬───────────────┘ │
│             │                              │                 │
│  ┌──────────▼───────────┐  ┌──────────────▼───────────────┐ │
│  │ BPF_MAP:             │  │ BPF_MAP:                     │ │
│  │ retrans_events (LRU) │  │ dns_queries (LRU)            │ │
│  │ retrans_stats (PERCPU)│ │ dns_stats (PERCPU)           │ │
│  └──────────┬───────────┘  └──────────────┬───────────────┘ │
└─────────────┼──────────────────────────────┼────────────────┘
              │ bpf_map_lookup_elem          │ bpf_map_lookup_elem
┌─────────────▼──────────────────────────────▼────────────────┐
│  用户态 (net_traffic.cpp)                                    │
│                                                             │
│  ┌──────────────────────┐  ┌──────────────────────────────┐ │
│  │ TcpRetransMonitor    │  │ DnsMonitor                   │ │
│  │ - 读取 retrans_events │  │ - 读取 dns_queries           │ │
│  │ - 聚合为连接级统计    │  │ - 计算解析延迟               │ │
│  │ - 提供给网络质量评估  │  │ - 检测超时/失败              │ │
│  └──────────────────────┘  └──────────────────────────────┘ │
└─────────────────────────────────────────────────────────────┘
```

## 变更文件清单

| 文件 | 变更类型 | 说明 |
|------|---------|------|
| `server/src/tcp_retransmit.bpf.c` | 新增 | TCP 重传追踪 eBPF 程序 |
| `server/src/dns_monitor.bpf.c` | 新增 | DNS 监控 eBPF 程序 |
| `server/include/tcp_retransmit_monitor.hpp` | 新增 | 用户态 TCP 重传监控接口 |
| `server/src/tcp_retransmit_monitor.cpp` | 新增 | 用户态 TCP 重传监控实现 |
| `server/include/dns_monitor.hpp` | 新增 | 用户态 DNS 监控接口 |
| `server/src/dns_monitor.cpp` | 新增 | 用户态 DNS 监控实现 |
| `server/Makefile` | 修改 | 添加新 BPF 程序编译规则和用户态源文件 |
| `server/src/net_traffic.cpp` | 修改 | 添加新 BPF 对象加载 |
| `server/src/net_tcp.cpp` | 修改 | 标记为 deprecated，添加降级到新方案的路径 |

## BPF 数据结构设计

### TCP 重传追踪

```c
// 连接标识
struct tcp_conn_key {
    __u32 saddr;
    __u32 daddr;
    __u16 sport;
    __u16 dport;
};

// 重传事件（LRU_MAP，用户态轮询消费）
struct tcp_retrans_event {
    __u32 pid;
    __u32 tgid;
    __u64 timestamp_ns;
    __u32 segs_out;
    __u32 segs_retrans;
    __u32 sstate;  // TCP 状态
};

// 连接统计（PERCPU_HASH，高效聚合）
struct tcp_retrans_stats {
    __u64 total_retrans;
    __u64 total_segs;
    __u32 current_state;
};
```

### DNS 监控

```c
// DNS 查询标识
struct dns_query_key {
    __u32 saddr;
    __u32 daddr;
    __u16 sport;
    __u32 ts;      // 时间戳低 32 位
};

// DNS 查询记录
struct dns_query_record {
    __u64 send_time_ns;
    __u64 recv_time_ns;  // 0 表示未收到响应
    __u32 reply_len;
    __u8  rcode;         // DNS 响应码
    __u8  is_response;   // 0=请求 1=响应
};
```

## 用户态接口设计

```cpp
// TCP 重传监控
class TcpRetransMonitor {
public:
    bool init();
    void stop();
    
    // 获取自上次调用以来的重传事件
    std::vector<TcpRetransEvent> pollEvents();
    
    // 获取所有连接的重传统计
    struct RetransStats {
        uint64_t total_retrans;
        uint64_t total_segs;
        double   loss_rate;  // = total_retrans / total_segs
    };
    std::map<TcpConnKey, RetransStats> getStats();
    
    // 计算丢包率（替代 TcpLossMonitor::compute）
    double computeLossRate();
};

// DNS 监控
class DnsMonitor {
public:
    bool init();
    void stop();
    
    struct DnsRecord {
        std::string query_ip;
        uint64_t latency_ms;
        bool     is_timeout;
        uint8_t  rcode;
    };
    
    // 获取已完成的 DNS 查询
    std::vector<DnsRecord> pollCompleted();
    
    // 获取平均 DNS 延迟
    double getAvgLatencyMs();
};
```

## 与现有系统的集成

### TCP 重传替代 net_tcp.cpp

1. `TcpRetransMonitor` 在 BPF 层直接统计每个连接的重传次数和总段数
2. 用户态通过 `bpf_map_lookup_elem` 读取 `PERCPU_HASH` Map，零拷贝
3. `computeLossRate()` 直接从 Map 计算，无需 netlink dump
4. `tcp_loss_monitor.cpp` 保留作为降级方案（当 BPF 不可用时）

### DNS 监控与网络质量评估

1. `DnsMonitor` 统计 DNS 延迟的 P50/P95/P99
2. `NetworkQualityAssessor` 新增 DNS 延迟作为评分维度
3. DNS 超时事件通过 `EventManager` 推送给客户端

## 任务分解

1. 实现 `tcp_retransmit.bpf.c` + 编译规则
2. 实现 `TcpRetransMonitor` 用户态读取
3. 修改 `tcp_loss_monitor.cpp` 使用新数据源
4. 实现 `dns_monitor.bpf.c` + 编译规则
5. 实现 `DnsMonitor` 用户态读取
6. 集成到 `NetworkQualityAssessor`
7. 更新部署脚本

<!-- autoai:tdd-policy:v1 -->
```json
{
  "schema_version": 1,
  "default": "required",
  "exceptions": [
    {
      "id": "exc-bpf-observability",
      "category": "observability_only",
      "task_ids": ["1", "2", "3", "4", "5", "6"],
      "paths": [
        "server/src/tcp_retransmit.bpf.c",
        "server/src/dns_monitor.bpf.c",
        "server/include/tcp_retransmit_monitor.hpp",
        "server/src/tcp_retransmit_monitor.cpp",
        "server/include/dns_monitor.hpp",
        "server/src/dns_monitor.cpp",
        "server/Makefile",
        "server/src/net_tcp.cpp"
      ],
      "reason": "eBPF 监控器是新增可观测性功能，编译通过即验证 BPF 程序和用户态加载逻辑正确",
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
  "rationale": "新增两个 eBPF 程序和对应的用户态监控器，替换低效的 netlink TCP 丢包方案，新增 DNS 可观测性",
  "classification": {
    "production": [
      "server/src/tcp_retransmit.bpf.c",
      "server/src/dns_monitor.bpf.c",
      "server/include/tcp_retransmit_monitor.hpp",
      "server/src/tcp_retransmit_monitor.cpp",
      "server/include/dns_monitor.hpp",
      "server/src/dns_monitor.cpp",
      "server/src/net_traffic.cpp",
      "server/src/net_tcp.cpp"
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
      "added_lines": {"expected": 800, "review_at": 1200, "hard_limit": 1500},
      "touched_files": {"expected": 8, "review_at": 10, "hard_limit": 12},
      "new_files": {"expected": 6, "review_at": 8, "hard_limit": 10}
    },
    "tests": {
      "added_lines": {"expected": 200, "review_at": 400, "hard_limit": 600},
      "touched_files": {"expected": 2, "review_at": 5, "hard_limit": 8},
      "new_files": {"expected": 1, "review_at": 2, "hard_limit": 3}
    },
    "project_support": {
      "added_lines": {"expected": 50, "review_at": 100, "hard_limit": 150},
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

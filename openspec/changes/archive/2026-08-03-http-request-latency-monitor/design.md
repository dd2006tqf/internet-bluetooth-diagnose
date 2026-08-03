# Design

## Overview
新增 eBPF 程序，挂载 `kprobe/tcp_sendmsg` 和 `kprobe/tcp_recvmsg`，从 sk_buff 提取 HTTP/1.x 请求和响应头部数据，计算 TTFB（首字节延迟），区分"应用慢 vs 网络慢"。

## 架构

```
┌─────────────────────────────────────────────────────────────┐
│  内核态 (eBPF VM) — http_latency.bpf.c                       │
│                                                             │
│  kprobe/tcp_sendmsg:                                        │
│    - 提取 sk_buff 前 64 字节                                │
│    - 检测 HTTP/1.x 请求 (GET/POST/PUT/DELETE)               │
│    - 记录: (saddr:sport→daddr:dport) → send_time_ns         │
│                                                             │
│  kprobe/tcp_recvmsg:                                        │
│    - 提取 sk_buff 前 64 字节                                │
│    - 检测 HTTP 响应 (HTTP/1.1 200, HTTP/1.1 404, ...)       │
│    - 计算 TTFB = recv_time_ns - send_time_ns               │
│    - 写入 BPF_MAP: http_txn_stats                          │
│                                                             │
│  ┌──────────────────────────────────────┐                   │
│  │ BPF_MAP: http_txn_stats (LRU_HASH)   │                   │
│  │  key = {saddr,daddr,sport,dport}      │                   │
│  │  value = {send_ns, recv_ns,           │                   │
│  │           req_bytes, resp_bytes,      │                   │
│  │           status_code, is_request}    │                   │
│  └────────────────────┬─────────────────┘                   │
└───────────────────────┼─────────────────────────────────────┘
                        │ bpf_map_lookup_elem
┌───────────────────────▼─────────────────────────────────────┐
│  用户态 (http_latency_monitor.cpp)                           │
│  - 读取 http_txn_stats Map                                  │
│  - 聚合 TTFB 分位数 (P50/P95/P99)                           │
│  - 按目标主机聚合                                           │
│  - 区分"应用慢 vs 网络慢"                                   │
└─────────────────────────────────────────────────────────────┘
```

## 变更文件清单

| 文件 | 变更类型 | 说明 |
|------|---------|------|
| `server/src/http_latency.bpf.c` | 新增 | HTTP 请求/响应延迟追踪 eBPF 程序 |
| `server/include/http_latency_monitor.hpp` | 新增 | 用户态接口 |
| `server/src/http_latency_monitor.cpp` | 新增 | 用户态实现 |
| `server/Makefile` | 修改 | 添加 BPF 编译规则和源文件 |

## BPF 数据结构设计

```c
// HTTP 事务标识
struct http_txn_key {
    __u32 saddr;
    __u32 daddr;
    __u16 sport;
    __u16 dport;
};

// HTTP 事务记录
struct http_txn_record {
    __u64 send_ns;       // 请求发送时间
    __u64 recv_ns;       // 响应接收时间
    __u32 req_bytes;     // 请求大小
    __u32 resp_bytes;    // 响应大小
    __u16 status_code;   // HTTP 状态码 (从响应头提取)
    __u8  is_request;    // 1=请求方向, 0=响应方向
    __u8  padding;
};
```

## HTTP 首部检测策略

由于 eBPF 内存读取有限制，采用以下策略识别 HTTP：

```c
// 检查 sk_buff 前 16 字节是否包含 HTTP 关键词
static __always_inline int check_http_request(void *data_start) {
    // 读取 8 字节: "GET /", "POST /", "PUT /", "DELETE "
    char prefix[16] = {};
    if (bpf_probe_read_kernel(prefix, sizeof(prefix), data_start) < 0) return 0;
    if (prefix[0]=='G' && prefix[1]=='E' && prefix[2]=='T' && prefix[3]==' ') return 1;
    if (prefix[0]=='P' && prefix[1]=='O' && prefix[2]=='S' && prefix[3]=='T') return 1;
    if (prefix[0]=='P' && prefix[1]=='U' && prefix[2]=='T' && prefix[3]==' ') return 1;
    return 0;
}

static __always_inline int check_http_response(void *data_start) {
    char prefix[12] = {};
    if (bpf_probe_read_kernel(prefix, sizeof(prefix), data_start) < 0) return 0;
    // "HTTP/1.0 " 或 "HTTP/1.1 "
    if (prefix[0]=='H' && prefix[1]=='T' && prefix[2]=='T' && prefix[3]=='P') return 1;
    return 0;
}

static __always_inline __u16 extract_status_code(void *data_start) {
    // 格式: "HTTP/1.1 200 OK\r\n"
    char buf[16] = {};
    if (bpf_probe_read_kernel(buf, sizeof(buf), data_start + 9) < 0) return 0;
    // buf 前 3 字节应为 status code 的 ASCII
    __u16 code = 0;
    if (buf[0] >= '1' && buf[0] <= '5')
        code = (buf[0]-'0')*100 + (buf[1]-'0')*10 + (buf[2]-'0');
    return code;
}
```

## 用户态接口设计

```cpp
// 单次 HTTP 事务
struct HttpTxnInfo {
    std::string srcIp, dstIp;
    uint16_t srcPort, dstPort;
    uint64_t ttfbNs;        // TTFB（首字节延迟）
    uint32_t reqBytes, respBytes;
    uint16_t statusCode;
    std::string dstHost;    // 通过反向 DNS 或连接表获取
};

// 聚合统计
struct HttpLatencyStats {
    uint64_t p50Ns, p95Ns, p99Ns;  // TTFB 分位数
    uint64_t maxNs, totalTxns;
    std::string analysis;           // "主要网络慢" / "主要应用慢" / "正常"
};

class HttpLatencyMonitor {
public:
    bool init(const std::string& bpfObjPath);
    void stop();

    // 获取最近完成的 HTTP 事务
    std::vector<HttpTxnInfo> getRecentTxns(size_t limit);

    // 按目标 IP 聚合 TTFB 统计
    std::map<std::string, HttpLatencyStats> getByDstHost();

    // 全局 TTFB 分位数
    HttpLatencyStats getGlobalStats();
};
```

## TTFB 分析逻辑

```
recv_time_ns - send_time_ns = TTFB
TTFB 高 (> 500ms): 
  - 若 recv 后立刻有大量数据（resp_bytes >> 0）→ 网络慢（TCP 拥塞或带宽不足）
  - 若 recv 后只有少量数据或状态码异常 → 服务器应用慢
  - 若 status_code >= 500 → 服务器错误，不是网络问题
```

## 任务分解

1. 实现 `http_latency.bpf.c` + 编译规则
2. 实现 `HttpLatencyMonitor` 用户态读取
3. 集成到 Makefile

<!-- autoai:tdd-policy:v1 -->
```json
{
  "schema_version": 1,
  "default": "required",
  "exceptions": [
    {
      "id": "exc-http-observability",
      "category": "observability_only",
      "task_ids": ["1", "2", "3"],
      "paths": [
        "server/src/http_latency.bpf.c",
        "server/include/http_latency_monitor.hpp",
        "server/src/http_latency_monitor.cpp",
        "server/Makefile"
      ],
      "reason": "HTTP 延迟监控是新增可观测性功能，编译通过即验证 BPF 程序和用户态加载逻辑正确",
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
  "rationale": "新增 HTTP 请求级延迟监控，区分应用慢与网络慢，计算 TTFB 分位数",
  "classification": {
    "production": [
      "server/src/http_latency.bpf.c",
      "server/include/http_latency_monitor.hpp",
      "server/src/http_latency_monitor.cpp"
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
      "added_lines": {"expected": 350, "review_at": 600, "hard_limit": 800},
      "touched_files": {"expected": 3, "review_at": 5, "hard_limit": 8},
      "new_files": {"expected": 3, "review_at": 5, "hard_limit": 8}
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

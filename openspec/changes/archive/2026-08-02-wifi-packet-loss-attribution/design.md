# Design

## Overview
新增 eBPF 程序，通过挂载内核网络栈的收发路径 tracepoint，统计每个接口的发送/接收/丢弃包数，区分发送丢包与接收丢包，精确定位是 Wi-Fi 链路问题还是上游路由器问题。

## 架构

```
┌─────────────────────────────────────────────────────────────┐
│  内核态 (eBPF VM)                                           │
│                                                             │
│  ┌──────────────────────────────────────┐                   │
│  │ wifi_packet_loss.bpf.c               │                   │
│  │                                      │                   │
│  │ tracepoint/net/netif_receive_skb     │  rx 统计          │
│  │   → 统计每接口接收包数/字节           │                   │
│  │                                      │                   │
│  │ tracepoint/net/net_dev_queue         │  tx 统计          │
│  │   → 统计每接口发送包数/字节           │                   │
│  │                                      │                   │
│  │ tracepoint/net/net_dev_xmit          │  tx 重试/丢弃      │
│  │   → 统计发送重试和丢弃               │                   │
│  └────────────────────┬─────────────────┘                   │
│                       │                                     │
│  ┌────────────────────▼─────────────────┐                   │
│  │ BPF_MAP: packet_stats (PERCPU_HASH)  │                   │
│  │  key = ifindex                       │                   │
│  │  value = {rx_pkts, rx_bytes,         │                   │
│  │            tx_pkts, tx_bytes,        │                   │
│  │            tx_drops, tx_retries}     │                   │
│  └────────────────────┬─────────────────┘                   │
└───────────────────────┼─────────────────────────────────────┘
                        │ bpf_map_lookup_elem
┌───────────────────────▼─────────────────────────────────────┐
│  用户态 (wifi_packet_loss_monitor.cpp)                       │
│  - 读取 packet_stats Map                                   │
│  - 计算发送/接收丢包率                                       │
│  - 提供给 NetworkQualityAssessor                           │
│  - 通过 EventManager 推送丢包归因事件                       │
└─────────────────────────────────────────────────────────────┘
```

## 变更文件清单

| 文件 | 变更类型 | 说明 |
|------|---------|------|
| `server/src/wifi_packet_loss.bpf.c` | 新增 | Wi-Fi/网卡收发丢包追踪 eBPF 程序 |
| `server/include/wifi_packet_loss_monitor.hpp` | 新增 | 用户态接口 |
| `server/src/wifi_packet_loss_monitor.cpp` | 新增 | 用户态实现 |
| `server/Makefile` | 修改 | 添加 BPF 编译规则和源文件 |

## BPF 数据结构设计

```c
// 接口收发统计（key = ifindex）
struct iface_packet_stats {
    __u64 rx_pkts;       // 接收包数
    __u64 rx_bytes;      // 接收字节数
    __u64 tx_pkts;       // 发送包数
    __u64 tx_bytes;      // 发送字节数
    __u64 tx_drops;      // 发送丢弃数（netdev_xmit 返回 NETDEV_TX_BUSY 或失败）
    __u64 tx_retries;    // 发送重试数
};
```

## 挂点选择

| 挂点 | 作用 | 可选性 |
|------|------|--------|
| `tracepoint/net/netif_receive_skb` | 记录接收包 | 内核 4.14+，稳定 |
| `tracepoint/net/net_dev_queue` | 记录发送入队 | 内核 4.14+，稳定 |
| `tracepoint/net/net_dev_xmit` | 记录发送完成/重试/丢弃 | 内核 4.14+，稳定 |

用 tracepoint 而非 kprobe 的优势：
1. 参数结构稳定（基于内核 tracepoint ABI，不受函数签名变化影响）
2. tracepoint 本身就是为监控设计的，开销小
3. 网卡驱动层 net_dev_xmit 能捕获具体的发送失败原因

## 用户态接口设计

```cpp
// 接口收发统计
struct IfacePacketStats {
    uint32_t ifindex;
    uint64_t rxPkts, rxBytes;
    uint64_t txPkts, txBytes;
    uint64_t txDrops, txRetries;

    // 发送丢包率 = txDrops / txPkts
    double txLossRate() const {
        if (txPkts == 0) return 0.0;
        return (txDrops * 100.0) / txPkts;
    }
};

// Wi-Fi/网卡丢包归因监控器
class WifiPacketLossMonitor {
public:
    bool init(const std::string& bpfObjPath);
    void stop();

    // 获取所有接口的收发统计
    std::map<uint32_t, IfacePacketStats> getStats();

    // 获取指定接口的丢包归因
    struct LossAttribution {
        std::string ifaceName;
        double rxLossRate;   // 接收丢包率
        double txLossRate;   // 发送丢包率
        uint64_t txRetries;  // 发送重试
        std::string analysis; // "主要发送丢包" / "主要接收丢包" / "连接正常"
    };
    LossAttribution analyze(uint32_t ifindex, const std::string& ifaceName);
};
```

## 丢包归因逻辑

```
rxPkts 持续增长但应用层 rx_bytes 增长缓慢 → 接收丢包（Wi-Fi 链路噪声/AP 拥塞）
txDrops / txPkts 比例高 → 发送丢包（本机网卡队列满/AP 不可达）
txRetries 高 → 无线重传（Wi-Fi 信号弱/干扰）
三者结合 → 判断故障点：
  - 发送丢包高 + 接收正常 → 本机到 AP 上行问题
  - 接收丢包高 + 发送正常 → AP 到本机下行问题
  - 收发都高 → 链路整体拥塞或信号差
```

## 任务分解

1. 实现 `wifi_packet_loss.bpf.c` + 编译规则
2. 实现 `WifiPacketLossMonitor` 用户态读取
3. 集成到 `NetworkQualityAssessor` 和事件推送

<!-- autoai:tdd-policy:v1 -->
```json
{
  "schema_version": 1,
  "default": "required",
  "exceptions": [
    {
      "id": "exc-wifi-observability",
      "category": "observability_only",
      "task_ids": ["1", "2", "3"],
      "paths": [
        "server/src/wifi_packet_loss.bpf.c",
        "server/include/wifi_packet_loss_monitor.hpp",
        "server/src/wifi_packet_loss_monitor.cpp",
        "server/Makefile"
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
  "rationale": "新增 Wi-Fi 收发丢包归因 eBPF 程序，区分发送/接收丢包，定位网络故障点",
  "classification": {
    "production": [
      "server/src/wifi_packet_loss.bpf.c",
      "server/include/wifi_packet_loss_monitor.hpp",
      "server/src/wifi_packet_loss_monitor.cpp"
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
      "added_lines": {"expected": 20, "review_at": 50, "hard_limit": 80},
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

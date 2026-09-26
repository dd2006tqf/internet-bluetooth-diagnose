# Proposal: ebpf-map-sizing-eviction

## Why

`flow_rate`/`tcp_retransmit`/`http_latency`/`dns_monitor`/`tcp_conn_stats`/`skb_drop`/`a2dp` 的 LRU_HASH `max_entries` 是 BPF 源码里的编译期常量（65536/8192/1024 等）。三类后果：

1. **资源不适配**：8GB Radxa A7A 上 65536 项浪费内存；1GB 网关上被撑爆；高并发微服务主机上又不够。
2. **统计失真不可见**：LRU 驱逐后 key 重建导致 per-key 计数器回退，用户态 delta 出现负值；现有 `CounterNormalizer` 只保护了 `wifi_loss` 一条路径。
3. **flow_rate 统计口径缺陷**：`NetTrafficAnalyzer::sampleTopFlows` 每次采样前 `bpf_map_delete_elem` 清空 `current_sec`（`net_traffic.cpp`），突发流量下扫描间隔内被 LRU 淘汰的流永久丢失；`getRealTimeStats` 只统计 top-1000 bps 之和，不代表真实吞吐。

## What

本变更分三阶段实现（P0 容量定标、P1 驱逐可见性、P2 flow_rate 累计口径修复），全部走 libbpf open→load 间隙 `bpf_map__set_max_entries`，新增只读 `GetEbpfMapStats` 出口，不新增 SLE key、不改真值表。

### P0：Map 容量运行时定标
- 新头文件 `server/include/utils/bpf_map_sizing.hpp`（纯逻辑、x86 可测）：`MapSizingConfig`/`MapSizeSpec`/`ResolvedMapSize`/`resolveMonitorMaps()`/`totalPhysicalRamBytes()`；
- 新实现 `server/src/utils/bpf_map_sizing.cpp`：libbpf-guarded `applyMapSizingPlan()` + 各 monitor `MapSizeSpec` 登记表；
- 各 `*_monitor.cpp` 在 open→load 间隙调用 `applyMapSizingPlan`；`init()` 签名增加 `const MapSizingPlan&`；
- `flow_rate` 的 `current_sec`/`process_stats` sizing 归 `process_profiler` 配置块统一管辖，`initForInterface(iface, plan)` 加可选 plan。

### P1：驱逐可见性
- 新头文件 `server/include/utils/per_key_counter_tracker.hpp`：模板 `PerKeyCounterTracker`，统计 `new/disappeared/reset/entries/watermark/eviction_limited`；
- 接入 `tcp_retransmit`/`process_net_profiler`/`http_latency`/`dns_monitor` 的 `getStats()`；
- `EbpfMonitorMetrics` 增加 `mapKeyResets`/`mapKeysDisappeared`/`mapWatermark`；
- 新 `MetricId`：`EBPF_MAP_WATERMARK_PCT`/`EBPF_MAP_RESET_KEYS`/`EBPF_MAP_DISAPPEARED_KEYS`；
- 新 D-Bus 只读方法 `GetEbpfMapStats`（不 root）；`eviction_limited` 仅进 `status` 字符串。

### P2：flow_rate 累计口径修复
- `NetTrafficAnalyzer::sampleTopFlows` 不再删 `current_sec`；
- `getRealTimeStats()` 改为全量 per-key delta 汇总 `totalBps`；
- `clearHistory()` 重置 tracker。

### 配置与出口
- `weaknet_config` 在 7 个 eBPF monitor 块加 `map_sizing_mode/entries/ram_budget_bp` 平铺键；
- `weaknet-cli ebpf-maps` 新子命令；
- `config.yaml`、`docs/架构设计.md`、`docs/weaknet_cli_usage.md` 同步。

## 非目标

- SQLite 生命周期/rollup；
- PMTU/ICMP、TCP-health、TLS/QUIC 新探针；
- PERCPU 迁移、map-in-map、内核驱逐回调；
- map_sizing 热生效免重启（走 `RestartMonitor`）；
- `tools/com.example.WeakNet.conf`、`scripts/` harness 修改；
- `wifi_loss`/`tcp_connect` sizing（固定小 map，无 LRU 压力）。

## 影响

- 涉及：`server/include/utils/`（新增）、`server/src/utils/`（新增）、`server/include/weaknet_config.hpp`、`server/src/weaknet_config.cpp`、`server/include/metrics/metric_types.hpp`、`server/include/ebpf_monitor_interface.hpp`、`server/src/dbus_service.cpp`、`server/include/common.hpp`、`server/src/monitor_ebpf_plugins.cpp`、`server/src/monitor_plugins.cpp`、8 个 `*_monitor.cpp` + `.hpp`、`server/src/net_traffic.cpp`、`server/src/traffic_analyzer.cpp`、`client/weaknet_client.h`、`client/client.cpp`、`client/weaknet_cli.cpp`、`config.yaml`、`docs/`、`tools/weaknet-test-full.sh`。
- 冻结边界：v1 真值表、Coverage 规则、Stabilizer、`capability_level_negative`、OverallPolicy vote、schema v2 顶层字段均不动；`EbpfMonitorMetrics` 仅追加 3 字段，`EbpfMonitorHealth` 不变。

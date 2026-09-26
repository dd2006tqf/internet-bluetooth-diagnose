# Proposal: ebpf-map-sizing-core

## Why

`tcp_retransmit`/`http_latency`/`dns_monitor`/`tcp_conn_stats`/`skb_drop`/`flow_rate` 的
LRU_HASH `max_entries` 是 BPF 源码里的编译期常量（65536/8192/1024 等）。两类后果：

1. **资源不适配**：8GB 的 Radxa A7A 上 65536 项浪费内存；1GB 网关上被撑爆；高并发主机上又不够。
2. **统计失真不可见**：LRU 驱逐后 key 重建导致 per-key 累计计数器回退，用户态做 delta 会
   得到负增量或伪造尖峰；现有 `CounterNormalizer` 只保护了 `wifi_loss` 一条路径。

本变更交付**定标框架 + 配置接线 + 驱逐追踪器**三块地基，使容量可按物理内存与配置解算、
并使驱逐压力可观测。不改变任何 SLE 评价语义。

## What

### 1. 定标框架（`bpf_map_sizing`）
- 纯逻辑 `resolveMonitorMaps(specs, cfg, ram_bytes)`：`fixed` 直接用配置条数；
  `auto` 按物理内存万分比预算分摊到该监控器各 map，再 `clamp` 到 `[min,max]`；
  非法 mode / RAM 不可用 / 预算为 0 → 全部回退 `default_entries`（与 v1 编译期常量一致）。
- `applyMapSizingPlan(obj, plan)`：在 `bpf_object__open` → `bpf_object__load` 间隙
  调用 `bpf_map__set_max_entries`（map 一旦 load 就无法 resize）。
- 各监控器 `MapSizeSpec` 表，`default_entries` 严格等于 v1 常量。

### 2. 配置接线
7 个 eBPF 监控器块新增扁平键 `map_sizing_mode` / `map_sizing_entries` /
`map_sizing_ram_budget_bp`（YAML 解析器只支持两层缩进，不能用嵌套块）；
接入 `applyMonitorField`（文件解析）、`applyMonitorParam`（D-Bus 调参）、
`serializeMonitorJson`（查询）三处。**刻意不进** TRIAL 白名单：map 已 load 后无法
resize，TRIAL 的"试改-回滚"语义会给出成功假象。

### 3. 逐 key 驱逐追踪（`PerKeyCounterTracker`）
用户态保留上一轮 key→value 快照做差分，分类 new / disappeared / reset，
并据水位与消失量给出 `eviction_limited`，为后续"观测受限降级证据"提供原料。

### 4. 接线到生产路径
- 6 个监控器（`tcp_retrans` / `process_profiler` / `http_latency` / `dns` /
  `tcp_conn` / `skb_drop`）的 `init(path, plan)` 增加 plan 参数并在 open→load
  间隙应用；plugin `start()` 负责解算并传入。
- `tcp_retransmit` / `process_net_profiler` / `http_latency` / `dns_monitor`
  的 `getStats()` 接入 `PerKeyCounterTracker`，用当轮快照更新并暴露 `PerKeyStats`。

## 非目标（明确排除，附理由）

- **a2dp（`active_sessions`/`bt_traffic`）定标**：唯一生产者是 `bt_monitor`，
  其 `initPhase2` 需透传 plan，涉及 `bt_monitor.hpp/.cpp`。本期不纳入。
- **flow_rate（`current_sec`/`process_stats`）在 traffic 插件路径的定标**：
  `traffic` 插件（order 10）先于 `process_profiler`（order 20）加载 flow_rate.bpf.o，
  其调用链经 `WeakNetMgr::startTrafficAnalysis` → `TrafficAnalyzer::start`，
  涉及 `weaknetmgr.hpp/.cpp`、`traffic_analyzer.hpp/.cpp`。在默认插件顺序下
  plan 到不了首个加载点，本期不纳入。
- **`GetEbpfMapStats` D-Bus 出口**：属 P1 后续增量。本变更内 `PerKeyStats`
  由各 monitor 持有并可直接查询方法读取，已有生产消费者。
- **`MetricId` 驱逐指标发布、CLI 子命令**：属 P1 后续增量。
- **`server/src/net_traffic.cpp` 的累计口径改造**：属 P2 后续增量。
- **SQLite 生命周期**：与本变更无关。

## 影响

- 新增：`server/include/utils/bpf_map_sizing.hpp`、`server/src/utils/bpf_map_sizing.cpp`、
  `server/include/utils/per_key_counter_tracker.hpp`、两个 gtest 文件。
- 修改：`weaknet_config.hpp/.cpp`、`monitor_ebpf_plugins.cpp`、
  6 个 monitor 的 `.hpp/.cpp`、`server/test/CMakeLists.txt`、`config.yaml`。
- 冻结边界：SLE 真值表、Coverage 规则、StateStabilizer、OverallPolicy vote 路径、
  AssessmentSnapshot schema v2 顶层字段——全部不动。

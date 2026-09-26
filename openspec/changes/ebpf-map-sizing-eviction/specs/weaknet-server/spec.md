# WeakNet Server Spec — Delta for ebpf-map-sizing-eviction

## ADDED Requirements

### Requirement: eBPF Map 容量运行时定标
服务端 MUST 在 `bpf_object__open_file` 与 `bpf_object__load` 之间支持按物理内存与配置对可调整的 eBPF Map `max_entries` 进行运行时定标，替代当前 BPF 源码中的编译期常量。每个受影响监控器 MUST 支持 `auto`（按 RAM 预算分摊）与 `fixed`（固定条数）两种模式，缺省行为 MUST 与 v1 当前常量一致。

#### Scenario: auto 模式按物理内存分摊预算
- **WHEN** `map_sizing_mode=auto` 且提供 `map_sizing_ram_budget_bp`
- **THEN** MUST 将 `ram_bytes × bp/10000` 作为该 monitor 全部可调 map 的总预算，按 `default_entries × (key+value+overhead)` 加权分摊，每项 `clamp(entries, min, max)`

#### Scenario: fixed 模式直接使用配置条数
- **WHEN** `map_sizing_mode=fixed` 且提供 `map_sizing_entries`
- **THEN** MUST 将 `map_sizing_entries` 作为该 monitor 可调 map 的目标条数，并 `clamp` 到各 map 的 `[min,max]` 区间

#### Scenario: 非法输入降级到默认
- **WHEN** `ram_bytes=0`、预算为 0、plan 为空或 `map_sizing_mode` 非法
- **THEN** MUST 回退到各 map 的 `default_entries`（与 v1 编译期常量一致），不得返回 0 或负数

### Requirement: map_sizing 配置键接入运行时调参
服务端 MUST 将 `map_sizing_mode`、`map_sizing_entries`、`map_sizing_ram_budget_bp` 暴露为 `applyMonitorField`/`serializeMonitorJson` 可解析的扁平配置键，并 MUST 拒绝非法 `mode`。这些键 MUST NOT 加入 `isTrialableKeyImpl` 白名单（需 `RestartMonitor` 生效）。

#### Scenario: 运行时修改 map_sizing 键需 restart 生效
- **WHEN** 客户端调用 `SetMonitorParam <mon>.map_sizing_*` 写入合法值
- **THEN** MUST 更新 `WeakNetConfig` 对应字段并推进 `config_generation`；MUST 在 `RestartMonitor <mon>` 后经 plugin `stop()`→`start()`→`init()` 重新 load 生效，不得热改已加载 map

#### Scenario: 非法 mode 被拒绝
- **WHEN** `SetMonitorParam <mon>.map_sizing_mode` 写入 `auto`/`fixed` 以外的值
- **THEN** MUST 拒绝并返回错误

### Requirement: eBPF Map 驱逐可见性
服务端 MUST 对具有"累计值"语义的 map（`retrans_stats`、`process_stats`、`http_txn_stats`、`fd_resolvers` 等）提供 per-key 维度的新建/消失/回退统计，并通过独立只读 D-Bus 方法 `GetEbpfMapStats` 输出每 map 的 `resolved_max`/`current_entries`/`watermark_pct`/`new_keys`/`disappeared_keys`/`reset_keys`/`eviction_limited`。

#### Scenario: per-key 回退被识别并剔除
- **WHEN** 某 key 的累计 value 在两轮扫描间出现倒退（被 LRU 驱逐后重建）
- **THEN** MUST 将该 key 记为 `reset`，其 delta 样本不得进入业务统计，并计入 `reset_keys`

#### Scenario: watermark 与 disappeared 触发 eviction_limited
- **WHEN** 某 map 的 `entries/max_entries >= 0.9` 且窗口内 `disappeared_keys` 超过阈值
- **THEN** MUST 置 `eviction_limited=true`，并将该状态写入 `EbpfMonitorStateSupport::status` 供 `GetEbpfMapStats` 读取；该状态 MUST NOT 改变业务 SLE 判定

### Requirement: flow_rate 统计口径改为累计 delta 模型
服务端 MUST 将 `NetTrafficAnalyzer` 对 `current_sec` 的访问从"采样前删表"改为"累计 + 用户态 delta"模型，消除突发流量下被 LRU 淘汰的流被永久丢弃的口径缺陷。`getRealTimeStats`/`sampleTopFlows`/`detectAnomalies` MUST 基于 per-key delta 而非绝对值计算。

#### Scenario: 突发流量下累计口径不丢流
- **WHEN** 突发流量使 `current_sec` 达到 LRU 上限并在扫描间隔内淘汰旧 key
- **THEN** MUST 通过 per-key last-value tracker 保证已观测到的流量 delta 不丢失，`totalBps` 反映全量流量而非 top-N 截断

### Requirement: 观测出口与兼容性
`GetEbpfMapStats` MUST 为只读方法（不 root 校验），返回各 monitor 的 sizing 配置与 map 运行统计；`EbpfMonitorMetrics` 仅可追加 `mapKeyResets`/`mapKeysDisappeared`/`mapWatermark` 三个平铺字段，`EbpfMonitorHealth` 结构体 MUST 保持不变。

#### Scenario: GetEbpfMapStats 返回 sizing 与驱逐统计
- **WHEN** 客户端调用 `GetEbpfMapStats`
- **THEN** MUST 返回包含 `sizing{mode,entries,ram_budget_bp}` 与 `maps[{map,resolved_max,current_entries,watermark_pct,new_keys,disappeared_keys,reset_keys,eviction_limited}]` 的 JSON

#### Scenario: GetEbpfMonitorHealth 兼容旧客户端
- **WHEN** 旧客户端调用 `GetEbpfMonitorHealth`
- **THEN** MUST 返回原有平铺字段并仅追加 `map_key_resets`/`map_keys_disappeared`/`map_watermark` 三个新字段，不得改变既有字段语义

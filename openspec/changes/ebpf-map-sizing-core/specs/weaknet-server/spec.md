# WeakNet Server Spec — Delta for ebpf-map-sizing-core

## ADDED Requirements

### Requirement: eBPF Map 容量运行时定标
服务端 MUST 支持在 `bpf_object__open` 与 `bpf_object__load` 之间按物理内存与配置解算
可调整 eBPF Map 的 `max_entries`，替代 BPF 源码中的编译期常量。每个参与定标的监控器
MUST 支持 `auto`（按 RAM 预算分摊）与 `fixed`（固定条数）两种模式。

#### Scenario: auto 模式按物理内存分摊预算
- **WHEN** 配置为 `mode=auto` 且给出 `ram_budget_bp`
- **THEN** MUST 以 `ram_bytes × ram_budget_bp / 10000` 为该监控器全部可调 map 的总预算，
  按 `default_entries × (key + value + 内核开销)` 加权分摊，每项 MUST `clamp` 到 `[min_entries, max_entries]`

#### Scenario: fixed 模式直接使用配置条数
- **WHEN** 配置为 `mode=fixed` 且给出 `entries`
- **THEN** MUST 以该 `entries` 为目标条数并 `clamp` 到各 map 的 `[min_entries, max_entries]`

#### Scenario: 非法输入降级到默认
- **WHEN** `ram_bytes` 为 0、预算为 0、`mode` 非法或 plan 为空
- **THEN** MUST 回退到各 map 的 `default_entries`（与 v1 编译期常量逐一相等），
  MUST NOT 返回 0 或超出 `[min_entries, max_entries]` 的值

### Requirement: map_sizing 配置键接入运行时调参
服务端 MUST 将 `map_sizing_mode`、`map_sizing_entries`、`map_sizing_ram_budget_bp`
作为扁平配置键接入文件解析、D-Bus 调参与 JSON 序列化三处，并 MUST 拒绝非法 `mode`。
这三个键 MUST NOT 进入 TRIAL 白名单。

#### Scenario: 扁平键在配置文件中被解析
- **WHEN** 配置文件在某个 eBPF 监控器节下写入上述三个扁平键
- **THEN** MUST 正确落位到该监控器的定标配置；`config.yaml` 随仓库分发的示例
  MUST 同样可被解析且不产生错误

#### Scenario: 运行时修改 map_sizing 键需 restart 生效
- **WHEN** 客户端调用 `SetMonitorParam <mon>.map_sizing_*` 写入合法值
- **THEN** MUST 更新配置并推进配置代次；MUST 在 `RestartMonitor <mon>` 经
  plugin `stop()` → `start()` → `init()` 重新 load 后才生效，MUST NOT 热改已加载 map

#### Scenario: 非法 mode 被拒绝
- **WHEN** 写入 `map_sizing_mode` 为 `auto`/`fixed` 以外的值，或写入超出允许区间的条数/预算
- **THEN** MUST 拒绝并保留原值不变

### Requirement: 定标在 load 前应用且缺省行为与 v1 一致
参与定标的监控器 MUST 在 open 与 load 之间应用解算结果，且未显式配置时
MUST 与 v1 的编译期常量保持一致。

#### Scenario: open 与 load 之间应用容量
- **WHEN** 监控器加载 BPF 对象
- **THEN** MUST 在 `bpf_object__load` 之前对目标 map 调用 `bpf_map__set_max_entries`；
  对象中不存在的同名 map MUST 被跳过而非报错

#### Scenario: 未配置时逐一等于 v1 常量
- **WHEN** 未提供任何 `map_sizing_*` 配置（默认 `mode=auto` 但无可用 RAM 预算信息）
- **THEN** 每个 map 的最终条数 MUST 等于其 `default_entries`，即 v1 的编译期常量

### Requirement: eBPF Map 逐 key 驱逐可见性
服务端 MUST 提供用户态的逐 key 累计计数器差分追踪，识别 LRU 驱逐导致的计数倒退，
并据水位与消失量给出"观测受驱逐限制"的判定。该判定 MUST NOT 改变任何业务 SLE 结论。

#### Scenario: per-key 回退被识别并剔除
- **WHEN** 某 key 的累计值在两轮扫描之间变小（被 LRU 驱逐后重建）
- **THEN** MUST 将该 key 计入 `reset_keys`，使其 delta 样本可被识别并剔除，
  MUST NOT 把它当作正常增长；上轮有本轮无的 key MUST 计入 `disappeared_keys`

#### Scenario: 水位与消失量触发 eviction_limited
- **WHEN** 某 map 的 `entries / max_entries ≥ 90%` 且该窗口 `disappeared_keys` 达到阈值
- **THEN** MUST 置 `eviction_limited = true`

#### Scenario: 单凭高水位不判驱逐受限
- **WHEN** 水位很高但没有任何 key 消失，或大量 key 消失但水位很低
- **THEN** MUST NOT 置 `eviction_limited`（前者只是用满，后者更可能是连接正常结束）

#### Scenario: 容量未知时不作水位判定
- **WHEN** `max_entries` 为 0（容量未知）
- **THEN** MUST NOT 计算水位或判定 `eviction_limited`，避免凭空断言观测受损

#### Scenario: 追踪器接入监控器扫描路径
- **WHEN** `tcp_retransmit` / `process_net_profiler` / `http_latency` / `dns_monitor`
  的 `getStats()` 执行并取得当轮 map 快照
- **THEN** MUST 用该快照更新其 `PerKeyCounterTracker`，并 MUST 暴露最新
  `PerKeyStats` 供后续观测出口读取；该统计 MUST NOT 参与或改变任何 SLE 判定

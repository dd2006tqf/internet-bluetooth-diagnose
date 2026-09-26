# Tasks: ebpf-map-sizing-core

> 规划约束（吸取前一 change 废弃的教训）：
> 1. 每个 task 的 `Verify` kind 必须能构成 RED→GREEN→REGRESSION 闭环；
>    仅声明 `build` 的任务无法起步（RED 只接受 test/behavior），必须走 TDD 例外。
> 2. `classification.production` 必须覆盖每个 task 实际要改的全部文件。
> 3. delta spec 的每个 scenario 都必须至少被一个 task 的 Covers 覆盖。

## 定标框架

- [ ] 1 `server/include/utils/bpf_map_sizing.hpp` + `server/src/utils/bpf_map_sizing.cpp`：纯逻辑 `resolveMonitorMaps`（auto 预算分摊 / fixed 钳位 / 非法输入回落默认）、`getMapSizingSpecs` 各监控器规格表、`totalPhysicalRamBytes`；libbpf 侧 `applyMapSizingPlan`（open→load 间隙 set_max_entries）与 `resolveScopePlan`
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `eBPF Map 容量运行时定标` | `auto 模式按物理内存分摊预算`
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `eBPF Map 容量运行时定标` | `fixed 模式直接使用配置条数`
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `eBPF Map 容量运行时定标` | `非法输入降级到默认`
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `定标在 load 前应用且缺省行为与 v1 一致` | `未配置时逐一等于 v1 常量`
  - Verify: `test`

## 驱逐可见性

- [ ] 2 `server/include/utils/per_key_counter_tracker.hpp`：header-only 模板 `PerKeyCounterTracker`，差分分类 new / disappeared / reset，并按水位与消失量给出 `eviction_limited`（容量未知时不判定；仅高水位或仅大量消失均不判定）
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `eBPF Map 逐 key 驱逐可见性` | `per-key 回退被识别并剔除`
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `eBPF Map 逐 key 驱逐可见性` | `水位与消失量触发 eviction_limited`
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `eBPF Map 逐 key 驱逐可见性` | `单凭高水位不判驱逐受限`
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `eBPF Map 逐 key 驱逐可见性` | `容量未知时不作水位判定`
  - Verify: `test`

## 配置接线

- [ ] 3 `weaknet_config` 扁平三键 `map_sizing_mode` / `map_sizing_entries` / `map_sizing_ram_budget_bp` 接入 7 个 eBPF 监控器块的 `applyMonitorField` / `applyMonitorParam` / `serializeMonitorJson`，排除 TRIAL 白名单；`config.yaml` 补注释示例
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `map_sizing 配置键接入运行时调参` | `扁平键在配置文件中被解析`
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `map_sizing 配置键接入运行时调参` | `运行时修改 map_sizing 键需 restart 生效`
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `map_sizing 配置键接入运行时调参` | `非法 mode 被拒绝`
  - Verify: `test`

## 接线到生产路径

- [ ] 4 6 个监控器 `init(path, plan)` 签名扩展（tcp_retrans / process_profiler / http_latency / dns / tcp_conn / skb_drop）+ `net_traffic.h` 的 `initForInterface(iface, plan)`；各 init 在 `bpf_object__load` 前调用 `applyMapSizingPlan`；`monitor_ebpf_plugins.cpp` 各 plugin `start()` 解算并传入 plan
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `定标在 load 前应用且缺省行为与 v1 一致` | `open 与 load 之间应用容量`
  - Verify: `build` `test`

- [ ] 5 `tcp_retransmit` / `process_net_profiler` / `http_latency` / `dns_monitor` 的 `getStats()` 接入 `PerKeyCounterTracker`，用当轮 map 快照更新并暴露最新 `PerKeyStats`（不参与任何 SLE 判定）
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `eBPF Map 逐 key 驱逐可见性` | `追踪器接入监控器扫描路径`
  - Verify: `build` `test`

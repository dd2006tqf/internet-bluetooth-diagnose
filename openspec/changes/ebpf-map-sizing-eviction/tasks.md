# Tasks: ebpf-map-sizing-eviction

## P0：Map 容量运行时定标

- [ ] 1 `server/include/utils/bpf_map_sizing.hpp` + `server/src/utils/bpf_map_sizing.cpp`：纯逻辑 `resolveMonitorMaps`/`totalPhysicalRamBytes`/`MapSizeSpec` 表 + libbpf `applyMapSizingPlan`
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `eBPF Map 容量运行时定标` | `auto 模式按物理内存分摊预算`
  - Verify: `test`（`test_bpf_map_sizing_gtest.cpp`）

- [ ] 2 `weaknet_config` 扁平 `map_sizing_*` 键接入（applyMonitorField/serializeMonitorJson），排除 trialable
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `map_sizing 配置键接入运行时调参` | `运行时修改 map_sizing 键需 restart 生效`
  - Verify: `test`（`test_weaknet_config_gtest.cpp` 追加用例）

- [ ] 3 `monitor_ebpf_plugins.cpp`/`monitor_plugins.cpp`：plugin `start()` 从 `ctx->cfg.<mon>` resolve plan 传给 `monitor_->init(path, plan)`
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `eBPF Map 容量运行时定标` | `auto 模式按物理内存分摊预算`
  - Verify: `build` `test`（ARM64）

- [ ] 4 `tcp_retrans`/`process_profiler`/`http_latency`/`dns`/`tcp_conn`/`skb_drop`/`bluetooth`（a2dp）各 monitor `init(path, plan)` + open→load 间隙调用 `applyMapSizingPlan`
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `eBPF Map 容量运行时定标` | `fixed 模式直接使用配置条数`
  - Verify: `build` `test`（ARM64）+ 板上 `bpftool map show` 验证

- [ ] 5 `flow_rate` 双加载 sizing 归属：`process_profiler` 配置块统一管辖 `current_sec`/`process_stats`；`initForInterface(iface, plan)` 加可选 plan 参数
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `eBPF Map 容量运行时定标` | `非法输入降级到默认`
  - Verify: `build` `test`（ARM64）

## P1：驱逐可见性

- [ ] 6 `server/include/utils/per_key_counter_tracker.hpp`：模板 `PerKeyCounterTracker`（new/grew/disappeared/reset/entries/watermark/eviction_limited）
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `eBPF Map 驱逐可见性` | `per-key 回退被识别并剔除`
  - Verify: `test`（`test_per_key_counter_tracker_gtest.cpp`）

- [ ] 7 `EbpfMonitorMetrics` +3 字段（`mapKeyResets`/`mapKeysDisappeared`/`mapWatermark`）；`EbpfMonitorHealth` 不动
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `观测出口与兼容性` | `GetEbpfMonitorHealth 兼容旧客户端`
  - Verify: `build`

- [ ] 8 `metric_types.hpp` +3 `MetricId`（`EBPF_MAP_WATERMARK_PCT`/`EBPF_MAP_RESET_KEYS`/`EBPF_MAP_DISAPPEARED_KEYS`）+ descriptor
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `eBPF Map 驱逐可见性` | `watermark 与 disappeared 触发 eviction_limited`
  - Verify: `build`

- [ ] 9 `tcp_retransmit`/`process_net_profiler`/`http_latency`/`dns_monitor` 的 `getStats()` 接入 `PerKeyCounterTracker`，`PerKeyStats` 写入 metrics 与 sizing report
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `eBPF Map 驱逐可见性` | `per-key 回退被识别并剔除`
  - Verify: `build` `test`（ARM64）+ 板上突发冒烟

- [ ] 10 `server.cpp` worker thread publish：`EBPF_MAP_*` 三个 MetricId 按 monitor 发布
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `eBPF Map 驱逐可见性` | `watermark 与 disappeared 触发 eviction_limited`
  - Verify: `build`（ARM64）

- [ ] 11 `GetEbpfMapStats` D-Bus：`common.hpp` kMethod + `dbus_service.cpp` handler（只读、不 root）
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `观测出口与兼容性` | `GetEbpfMapStats 返回 sizing 与驱逐统计`
  - Verify: `test`（`test_ebpf_map_stats_gtest.cpp`）

- [ ] 12 `client/weaknet_client.h`/`client.cpp`：`weaknet_get_ebpf_map_stats`；`client/weaknet_cli.cpp`：`ebpf-maps` 子命令
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `观测出口与兼容性` | `GetEbpfMapStats 返回 sizing 与驱逐统计`
  - Verify: `build`（ARM64）+ 板上 `weaknet-cli ebpf-maps`

- [ ] 13 `eviction_limited` 写入 `EbpfMonitorStateSupport::status`（不进 SLE evidence）
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `eBPF Map 驱逐可见性` | `watermark 与 disappeared 触发 eviction_limited`
  - Verify: `build`（ARM64）

## P2：flow_rate 累计口径修复

- [ ] 14 `NetTrafficAnalyzer` 内置 `PerKeyCounterTracker`（`current_sec` key→bytes）；`sampleTopFlows` 删表循环移除
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `flow_rate 统计口径改为累计 delta 模型` | `突发流量下累计口径不丢流`
  - Verify: `build` `test`（ARM64）

- [ ] 15 `getRealTimeStats()`/`detectAnomalies` 改全量 per-key delta 汇总；`clearHistory()` 重置 tracker
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `flow_rate 统计口径改为累计 delta 模型` | `突发流量下累计口径不丢流`
  - Verify: `build` `test`（ARM64）+ 板上 `iperf3 -P64` 对比

## 收尾

- [ ] 16 `config.yaml` 各 eBPF monitor 加 `map_sizing_*` 注释行
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `map_sizing 配置键接入运行时调参` | `非法 mode 被拒绝`
  - Verify: `build`

- [ ] 17 `docs/架构设计.md`（监控矩阵列、新 D-Bus 方法、sizing 说明）+ `docs/weaknet_cli_usage.md` 同步
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `观测出口与兼容性` | `GetEbpfMapStats 返回 sizing 与驱逐统计`
  - Verify: `build`

- [ ] 18 `tools/weaknet-test-full.sh` 加 `weaknet-cli ebpf-maps` 冒烟行
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `观测出口与兼容性` | `GetEbpfMapStats 返回 sizing 与驱逐统计`
  - Verify: `build`

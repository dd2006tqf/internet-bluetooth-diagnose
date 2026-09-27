# Tasks: ebpf-map-sizing-wiring

> 规划约束（吸取前一 change 被废弃的教训）：
> 1. 每个 task 的 `Covers` 场景名与 `specs/weaknet-server/spec.md` 逐字一致。
> 2. `classification.production` 覆盖每个 task 实际要改的全部文件，含上一轮遗漏的
>    `server/src/traffic_analyzer.cpp`、`server/src/server.cpp`、蓝牙两个实现文件，
>    以及本轮发现的 `server/src/utils/bpf_map_sizing.cpp`。
> 3. 任务 1-3 的效果（板端 map `max_entries`）只能在 ARM64 真机观测，由同一个
>    `observability_only` 例外覆盖；任务 4 有可执行的失败判据，走正常 RED→GREEN→REGRESSION。
>    不声明 `unavailable_hardware`（该类会生成 provisional 义务，使 change 无法归档）。

## flow_rate 定标通路

- [x] 1 把容量计划贯通到 `flow_rate.bpf.o` 的实际加载点：`NetTrafficAnalyzer::initForInterface`
  的 `plan` 参数改为必填（移除 `= {}` 默认值，使遗漏成为编译错误）；
  `TrafficAnalyzer::start` 与 `WeakNetMgr::startTrafficAnalysis` 增加 `plan` 形参并逐层转发；
  `server.cpp` 流量分析线程按 `ctx->cfg.process_profiler.map_sizing` 解算 plan 后下传；
  同步适配受签名影响的既有测试调用点（`test_ebpf.cpp`、`test_traffic_analyzer_gtest.cpp` 4 处）
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `eBPF Map 容量定标贯通到全部加载路径` | `flow_rate 两张 map 经流量分析通路获得定标`
  - Verify: `build` `test`

## 共享 fd 路径的容量记录

- [x] 2 `ProcessNetProfiler::init` 在复用 `TrafficAnalyzer` 已加载 map fd 的分支上，
  于提前返回之前记录 `process_stats` 的 resolved 容量，使 `PerKeyCounterTracker`
  以真实容量作水位比较，恢复 `eviction_limited` 判定；该分支亦传入 plan
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `eBPF Map 容量定标贯通到全部加载路径` | `共享 map fd 路径记录 resolved 容量`
  - Verify: `build` `test`

## 蓝牙定标接线

- [x] 3 `BtAudioAnalyzer::init` 增加 `plan` 形参并在 `bpf_object__open` 与
  `bpf_object__load` 之间调用 `applyMapSizingPlan`；`BtMonitor::initPhase2` 传递 plan；
  其调用点按 `ctx->cfg.bluetooth.map_sizing` 解算，使既有 `bluetooth` 配置键生效，
  `MapSizingScope::Bluetooth` 不再是无调用方的死代码；同步适配
  `test_bt_monitor.cpp` 的 3 处 `initPhase2` 调用点
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `eBPF Map 容量定标贯通到全部加载路径` | `蓝牙两张 map 获得定标`
  - Verify: `build` `test`

## 规格表与源码常量一致性

- [x] 4 修正 `kProcessProfilerSpecs` 中 `process_stats` 的 `default_entries`
  （8192 → 65536，与 `flow_rate.bpf.c` 声明的编译期常量一致），并新增交叉校验测试
  `test_map_sizing_spec_sync_gtest`：遍历全部 `MapSizingScope` 的真实规格表，解析对应
  `.bpf.c` 中该 map 的 `max_entries`（支持字面量与 `#define` 常量）逐项比对
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `Map 定标规格表与内核编译期常量一致` | `规格表默认值与 BPF 源码声明逐一相符`
  - Verify: `test`

# Proposal: ebpf-map-sizing-wiring

## Why

上一轮 change `ebpf-map-sizing-core` 在真机核验时暴露了一个只在开发板上才可见的缺陷：**解算出来的 Map 容量计划没有到达实际的加载点**。

开发板（Radxa Cubie A7A，aarch64，kernel 5.15.147）部署后逐张核对 `bpftool map show`，12 张受管 map 中 10 张的 `max_entries` 与 resolved 公式值逐一吻合（例如 `retrans_stats` 8192 → 173202），但 `flow_rate.bpf.o` 的两张 map 停在 BPF 源码声明值：

| map | 板端实测 | 应 resolved |
|---|---|---|
| `current_sec` | 65536 | 155275 |
| `process_stats` | 65536 | 19409 |

根因不是公式或规格表错误，而是链路断开：`ProcessProfilerPlugin` 确实解算了 plan 并传给 `ProcessNetProfiler::init`，但该函数命中「共享 `TrafficAnalyzer` 的 map fd」分支后 `return true` 提前返回，`applyMapSizingPlan` 只存在于其后的独立加载回退路径，而回退路径永不执行——**plan 被静默丢弃**。

同一处断链还有第二个后果：`impl_->process_stats_max` 只在回退路径被赋值，共享路径下保持初始值 `0`（语义为「容量未知」），而它被直接传给 `PerKeyCounterTracker::update(snapshot, max_entries)`。按追踪器自身语义「容量未知时不作水位判定」，生产环境下 process_profiler 的 `eviction_limited` 检测**静默失效**。

另有同类断链：`MapSizingScope::Bluetooth` 与 `kBluetoothSpecs` 定义了蓝牙两张 map 的规格，且 `config.yaml` 与 `weaknet_config.cpp` 都解析、序列化 `bluetooth.map_sizing` 三键，但 `BtAudioAnalyzer::init(bpfObjectPath)` 不接受 plan、也从不调用 `applyMapSizingPlan`，且没有任何调用方使用 `MapSizingScope::Bluetooth`——**规格表是死代码，配置可读可写却永不生效**。

实现本 change 期间，通过逐项交叉核对规格表与 BPF 源码，发现**第二处缺陷**：`bpf_map_sizing.hpp` 规定 `default_entries` 必须等于该 map 在 `.bpf.c` 中的编译期 `max_entries`，以保证「不配任何定标文件时行为零变化」，但 `process_stats` 的规格表默认值为 **8192**，而 `flow_rate.bpf.c` 声明的是 **65536**——相差 8 倍。14 张受管 map 中仅此一项不符。

该约定原先只写在注释里、无人校验；既有单元测试使用自造 spec（`kSpec{"test_map", ...}`），从不读取真实规格表，因此完全掩盖了这个漂移。

两处缺陷**叠加**在同一个 map 上：板端 `process_stats` 实测 65536，既可能是「未定标」（缺陷一），也可能是「回落到错误的 8192 失败后保留源码默认值」。同一现象有两个成因，必须同时修复才能让板端结果可解释。

## What

把已解算的容量计划贯通到**所有**实际加载路径，并修正规格表与源码常量的漂移：

1. **flow_rate 通路贯通**：`server.cpp` 的流量分析线程持有 `ServerContext`，在该处按 `process_profiler` 配置块解算 plan，经 `WeakNetMgr::startTrafficAnalysis` → `TrafficAnalyzer::start` → `NetTrafficAnalyzer::initForInterface` 一路传递，使 open→load 间隙的 `applyMapSizingPlan` 真正作用于 `current_sec` / `process_stats`。`ProcessNetProfiler` 复用共享 fd 的调用点同样传入 plan。
2. **共享 fd 路径的容量记录**：在共享分支提前返回之前记录该 map 的 resolved 容量，使 `PerKeyCounterTracker` 获得真实容量而非「未知」，恢复驱逐可见性判定。
3. **蓝牙接线**：`BtAudioAnalyzer::init` 接受 plan 并在 open→load 之间应用；`BtMonitor::initPhase2` 传递 plan；其调用点（已持有 `ServerContext`）按 `bluetooth` 配置块解算。消除死代码，使既有配置键真正生效。
4. **规格表一致性**：修正 `process_stats` 的 `default_entries` 为 65536，并新增交叉校验测试（遍历真实规格表，解析对应 `.bpf.c` 的 `max_entries` 逐项比对），把注释里的约定变成可执行断言。

## Non-goals

- 不改变 `resolveMonitorMaps` 的预算分摊算法。
- 不改变各监控器的 `MapSizeSpec` 中 `min`/`max` 边界值（仅修正 `process_stats` 的 `default_entries`）。
- 不新增配置键；`config.yaml` 与 `weaknet_config.cpp` 的解析/序列化已在上一轮完成。
- 不引入板端 Project Command；harness 证据仍只走已审阅的 x86 Project Profile 命令。

## 验收边界

surface 验证「plan 正确贯通到加载点」这一**代码事实**（`build-server` / `test-all` 探针），板端 `bpftool map show` 核对由人工执行并记录在交付说明中，不作为 harness 证据——因为已审阅的 Project Profile 中没有板端命令，板端 argv 无法通过 `bindSurfaceProbes` 的逐字契约校验。

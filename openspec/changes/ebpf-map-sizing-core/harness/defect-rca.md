# Defect RCA — ebpf-map-sizing-core

## 2026-09-27T08:56:09Z

- Symptom: 板端真机核验时，flow_rate.bpf.o 的 `current_sec` 与 `process_stats` 两张 map 的
  `max_entries` 仍为 BPF 源码声明值 65536，未按 auto 预算定标（应分别为 155275 / 19409）；
  其余 10 张受管 map 的实测值均与 resolved 公式值逐一吻合。
- Reproducer: 部署后 `sudo bpftool map show | grep -A1 -E 'name (current_sec|process_stats)'`，
  对照 `resolveMonitorMaps(kProcessProfilerSpecs, {mode:"auto",ram_budget_bp:50}, 8112516*1024)`
  的返回值；板端 MemTotal 8112516 kB，预算 41536081 B。
- Root cause: 解算出的 plan 被静默丢弃，而非公式或规格表错误。链路有三处：
  1) `net_traffic.h:88` 的 `initForInterface(ifaceName, plan = {})` 为 plan 提供空默认值；
  2) 两个调用方都没传 plan —— `traffic_analyzer.cpp:75`（真正的 flow_rate 加载者）与
     `process_net_profiler.cpp:118`（共享 fd 路径）；
  3) `ProcessProfilerPlugin`（`monitor_ebpf_plugins.cpp:161-163`）确实解算了 plan 并传给
     `ProcessNetProfiler::init`，但该函数在共享 fd 分支 `return true` 提前返回，
     `applyMapSizingPlan` 只存在于其后的「独立加载回退路径」，而该回退路径永不执行。
  规划缺陷（同源）：Planner 把需要真机验证的 `surface-bpf-map-sizing-framework` 与
  `surface-per-key-counter-tracker` 分给 Task 4/5，同时给这两个任务声明
  `unavailable_hardware` 例外，却未规划其恢复路径；且 `classification.production`
  未包含 `server/src/traffic_analyzer.cpp`，导致修复面越界。
- Direct fix: 将 `ProcessProfilerPlugin` 解算的 plan 贯通到共享加载路径 —— 由
  `TrafficAnalyzer::start()` 接收并转发 plan，`NetTrafficAnalyzer::initForInterface`
  在两个调用点均收到非空 plan，使 open→load 间隙的 `applyMapSizingPlan` 对
  flow_rate 两张 map 实际生效。附带修 `MapSizingScope::Bluetooth` 死代码
  （规格表存在、配置三键可读可写，但无任何调用方，故永不生效）。
- Regression evidence: 待新 change 记录（本条为规划期发现的缺陷，原 change 已废弃）。
- Reusable Trigger/Check rule: 「配置已解析 / 参数已解算」不等于「已作用到目标对象」。
  任何 `applyXxx(obj, plan)` 必须在**所有**加载路径（含共享 fd / 单例复用 / 提前返回
  分支）上调用；规划期须核对每个 `Verify` 的义务在**实际执行**的那条分支上闭合，
  而非在一条被短路的分支上。真机核验必须以目标对象的可观测属性（`bpftool map show`
  的 `max_entries`）为准，不能只依赖单元测试对纯函数 `resolveMonitorMaps` 的覆盖。


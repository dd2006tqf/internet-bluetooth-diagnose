# Defect RCA — ebpf-map-sizing-wiring

## 2026-09-27T20:24:15Z

- Symptom: 开发板部署后，`flow_rate.bpf.o` 的 current_sec / process_stats 与
  `a2dp_media.bpf.o` 的 active_sessions / bt_traffic 四张 map 的 `max_entries`
  停留在 BPF 源码声明的编译期常量（65536 / 65536 / 64 / 64），未按 auto 预算定标；
  同时 process_profiler 的逐 key 驱逐判定因容量「未知」而静默失效。
- Reproducer: 部署到开发板后 `sudo bpftool map show | grep -A1 -E
  'name (current_sec|process_stats|active_sessions|bt_traffic)'`，与
  `resolveScopePlan(scope, {auto,50}, 8112516*1024)` 的返回值比对。
- Root cause: 解算出的容量计划在三条加载路径上被丢弃：
  1) `NetTrafficAnalyzer::initForInterface` 的 plan 形参有 `= {}` 默认值，而
     `TrafficAnalyzer::start` / `WeakNetMgr::startTrafficAnalysis` 均无 plan 形参，
     `server.cpp` 流量分析线程也不传配置——真正加载 flow_rate 的调用点收到空计划；
  2) `ProcessNetProfiler::init` 命中「共享 TrafficAnalyzer 的 map fd」分支后提前
     `return true`，`applyMapSizingPlan` 只存在于其后永不执行的独立加载回退路径；
  3) `BtAudioAnalyzer::init` 不接受 plan、也从不调用 `applyMapSizingPlan`，
     `MapSizingScope::Bluetooth` 无任何调用方——蓝牙规格表是死代码，
     而 `config.yaml` 的 bluetooth.map_sizing 三键可读可写却永不生效。
  第二处缺陷（同一 change 内追加发现）：`kProcessProfilerSpecs` 中 process_stats 的
  `default_entries` 为 8192，而 `flow_rate.bpf.c` 声明的是 65536，违反
  「未配置时行为与 v1 零变化」不变式；该约定只写在注释里、无人校验，
  且既有单元测试使用自造 spec（不读真实规格表），完全掩盖了漂移。
- Direct fix: 移除 `initForInterface` 的 plan 默认值使遗漏成为编译错误，plan 沿
  server.cpp → WeakNetMgr → TrafficAnalyzer 逐层转发；共享 fd 分支提前返回前记录
  resolved 容量（未命中回退规格表默认值）；`BtAudioAnalyzer::init` / `BtMonitor::initPhase2`
  增加 plan 形参并在 open→load 之间应用；修正 process_stats 的 default_entries 为 65536，
  并新增 `test_map_sizing_spec_sync_gtest` 交叉校验真实规格表与 `.bpf.c` 常量。
- Regression evidence: x86 build-server + test-all 42/42 全绿（含新增交叉校验测试，
  其 RED 为 EXPECT_EQ 8192 ≠ 65536 的真实断言失败）；ARM64 容器编译通过；
  板端人工核验（2026-09-27，开发板 192.168.2.77，服务 20:11:08 UTC 重启）：
  current_sec=86353、process_stats=86353、active_sessions=512、bt_traffic=512，
  与 resolved 公式值（86353/86353/512/512，末两者钳位到 max）**逐项吻合**，
  四张 map 的实测值均已脱离源码编译期常量，证明 plan 真实作用于加载点。
- Reusable Trigger/Check rule: 「配置已解析 / 参数已解算」不等于「已作用到目标对象」。
  任何 `applyXxx(obj, plan)` 必须在**所有**加载路径（含共享 fd、单例复用、提前返回分支）
  上调用；形参**不要留空默认值**——`= {}` 会让遗漏静默通过，把纪律变成编译期约束才可靠。
  此外，两份代码各写同一常量（规格表 vs 内核源码）时必须有可执行的交叉校验断言，
  注释里的约定等于没有约定；测试若使用自造数据而非真实规格表，会把这类漂移整体掩盖。


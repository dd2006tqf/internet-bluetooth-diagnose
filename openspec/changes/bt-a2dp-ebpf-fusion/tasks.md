# Tasks — Bluetooth Monitor A2DP + eBPF Fusion

> Project Profile command IDs（见 `.ai-harness/project-profile.json`）：
> - `build-server` → `make -C server`
> - `test-all` → `bash server/test/run_all_tests.sh`
> - `toolchain-probe` → `g++ --version`
>
> 任务状态：`[x]` 已实现且直接验证通过；`[ ]` 待完成。每个任务以 `- Covers:` 声明覆盖的 delta 需求+场景，以 `- Verify:` 声明验证种类（`build`/`test`/`behavior`/`static`），由 `task_verify.sh --project-command <id>` 或 `evaluator_check.sh --run --project-command <id>` 执行。
>
> 本 change 由 `蓝牙监控优化实现方案.md` v2.0 迁移而来。T1-T9 代码已落地（既有 commit 证据），T10（集成测试 + RAG）为剩余主要工作。已完成任务的 TDD RED→GREEN→REGRESSION 证据将在 Evaluator 阶段独立补齐到 `harness/verification.json`。

## Phase 1a — 频段冲突检测（M1）

- [x] 1 band_conflict_detector 模块
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `Band Conflict Detection` | `完全正相关输入`
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `Band Conflict Detection` | `样本不足降级`
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `Band Conflict Detection` | `频段冲突确认`
  - Verify: `build`, `test`
  - Operation: 新建 `server/include/band_conflict_detector.hpp` + `server/src/band_conflict_detector.cpp`
  - Requirement: REQ-BAND-CONFLICT（Pearson 计算、样本不足降级、置信度输出）
  - TDD: RED（`test_band_conflict_detector.cpp` 边界用例）→ GREEN → REFACTOR
  - Evidence: commit `27b05ee`；`server/test/unit/test_band_conflict_detector.cpp`

- [ ] 2 network_quality_thread 接入冲突检测
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `Band Conflict Detection` | `周期调用`
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `Event Routing` | `频段冲突事件`
  - Verify: `build`, `test`
  - Operation: 修改 `server/src/server.cpp`，`network_quality_thread`（15s）内 feed + detect + emit
  - Requirement: REQ-BAND-CONFLICT、REQ-EVENT-ROUTING
  - TDD: RED（周期调用断言）→ GREEN → REGRESSION（既有 network_quality 测试不回归）
  - Evidence: commit `27b05ee`

## Phase 1b — A2DP D-Bus + 距离估算（M2）

- [ ] 3 NetInfo 蓝牙字段 + 序列化
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `NetInfo Bluetooth Extension` | `新增字段`
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `NetInfo Bluetooth Extension` | `向后兼容`
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `NetInfo Bluetooth Extension` | `序列化往返`
  - Verify: `build`, `test`
  - Operation: 修改 `server/include/net_info.hpp` + `server/src/net_info.cpp`，新增蓝牙字段，扩展 `toJson`/`fromJson`/`isValid`/`needsUpdate`
  - Requirement: REQ-NETINFO-EXT
  - TDD: RED（序列化往返测试）→ GREEN → REFACTOR
  - Evidence: `server/test/unit/test_net_info.cpp`

- [ ] 4 BtMonitor refreshAudioTransports + 评分
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `A2DP Audio Quality Monitoring` | `音频质量评分`
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `A2DP Audio Quality Monitoring` | `严重延迟扣分`
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `A2DP Audio Quality Monitoring` | `MediaTransport1 接口缺失降级`
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `A2DP Audio Quality Monitoring` | `三层采集机制`
  - Verify: `build`, `test`
  - Operation: 修改 `server/include/bt_monitor.hpp` + `server/src/bt_monitor.cpp`，新增 `BtAudioTransport`/`BtAudioQuality`、`refreshAudioTransports`、`getAudioQuality`、`hasMediaTransportInterface`、`calculateAudioScore`
  - Requirement: REQ-A2DP-QUALITY
  - TDD: RED（评分边界 0/100/500/2000/5000ms）→ GREEN
  - Evidence: 既有 BtMonitor 实现

- [ ] 5 BtMonitor 距离估算 + 校准
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `Device Distance Estimation` | `1 米处估算`
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `Device Distance Estimation` | `RSSI 为零降级`
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `Device Distance Estimation` | `校准接口`
  - Verify: `build`, `test`
  - Operation: 修改 `bt_monitor.hpp/cpp`，新增 `estimateDistance`、`calibrateDistance`，`BtDeviceInfo` 增 `estimatedDistance`/`calibratedTxPower`
  - Requirement: REQ-DISTANCE
  - TDD: RED（1m/5m 估算区间）→ GREEN
  - Evidence: 既有 BtMonitor 实现

## Phase 2 — eBPF 融合层（M3）

- [x] 6 a2dp_media.bpf.c + Makefile
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `eBPF Bluetooth Traffic Collection` | `设备级聚合`
  - Verify: `build`, `static`
  - Operation: 新建 `server/src/bpf/a2dp_media.bpf.c`（libbpf + CO-RE + vmlinux.h，`active_sessions`/`bt_stats` maps，kprobe 挂点）+ `server/Makefile` 编译规则
  - Requirement: REQ-EBPF-TRAFFIC
  - TDD: 不适用（内核态，TDD exception `ebpf-attach-env`，类别 unavailable_hardware）；静态审查 + 编译验证 `clang -target bpf -c`
  - Evidence: commit `27b05ee`/`e684783`

- [x] 7 bt_audio_analyzer 加载/挂点探测/降级
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `eBPF Bluetooth Traffic Collection` | `挂点探测降级`
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `eBPF Bluetooth Traffic Collection` | `启动成功日志`
  - Verify: `build`, `static`
  - Operation: 新建 `server/include/bt_audio_analyzer.hpp` + `server/src/bt_audio_analyzer.cpp`，`start`/`stop`/`isAvailable`/`setSessionActive`/`getStats`，挂点优先级 tracepoint→fentry→kprobe→降级
  - Requirement: REQ-EBPF-TRAFFIC（降级路径）
  - TDD: 不适用（TDD exception `ebpf-attach-env`，类别 unavailable_hardware）；启动日志断言 + 静态审查
  - Evidence: commit `e684783`

- [ ] 8 bt_audio_fusion 状态机/时间加权/stall
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `D-Bus eBPF Fusion` | `正常音频传输`
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `D-Bus eBPF Fusion` | `疑似停滞`
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `D-Bus eBPF Fusion` | `三种活跃时长`
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `D-Bus eBPF Fusion` | `时间加权 Delay`
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `D-Bus eBPF Fusion` | `状态矛盾标记`
  - Verify: `build`, `test`
  - Operation: 新建 `server/include/bt_audio_fusion.hpp` + `server/src/bt_audio_fusion.cpp`，`evaluate` 输出 `effective_active`/`suspected_stall`/三种活跃时长/时间加权 Delay
  - Requirement: REQ-FUSION
  - TDD: RED（`test_bt_audio_fusion.cpp` 状态矩阵）→ GREEN → REFACTOR
  - Evidence: `server/test/unit/test_bt_audio_fusion.cpp`

- [ ] 9 BtMonitor 接入融合层 + BPF Map 开关
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `eBPF Bluetooth Traffic Collection` | `BPF Map 控制开关`
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `Event Routing` | `音频/距离事件`
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `Event Routing` | `eventCounter 原子性`
  - Verify: `build`, `test`
  - Operation: 修改 `bt_monitor.cpp`，`onTransportStateChanged` 写 `active_sessions.enabled`，`refreshDeviceStates` 内调 `BtAudioFusion.evaluate`，音频/距离事件经 EventManager 路由
  - Requirement: REQ-FUSION、REQ-EVENT-ROUTING
  - TDD: RED（融合层接入断言 + eventCounter 并发原子性）→ GREEN → REGRESSION
  - Evidence: commit `e684783`、`08165b8`（eventCounter 原子化修复）

## M4 — 集成测试 + RAG（收尾）

- [ ] 10 集成测试 + RAG 知识库诊断条目
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `RAG Diagnostics` | `频段冲突查询`
  - Covers: `specs/bluetooth-monitoring/spec.md` | `ADDED` | `RAG Diagnostics` | `音频延迟查询`
  - Verify: `test`, `behavior`
  - Operation: ① 扩展 `server/test/run_all_tests.sh` 增加蓝牙全链路集成测试（频段冲突→EventManager→D-Bus 信号）；② 补充 RAG 知识库"频段冲突"/"音频卡顿"/"距离估算"诊断条目
  - Requirement: REQ-EVENT-ROUTING、REQ-RAG
  - Scenarios: AC4.1 全链路事件正确路由；AC4.3 输入"蓝牙卡顿"→RAG 返回频段冲突建议；AC4.4 输入"蓝牙音频延迟高"→返回编解码器/距离建议
  - TDD: RED（集成测试用例 + RAG 查询断言）→ GREEN → REGRESSION
  - Evidence: 待提交
  - Subtasks:
    - 10.1 蓝牙全链路集成测试（频段冲突/A2DP/融合层事件路由）
    - 10.2 RAG 知识库诊断条目（频段冲突、音频卡顿、距离估算）
    - 10.3 24h 稳定性验证（内存/CPU 监控）

## 归档前置

以下为 archive 前置条件（非任务，由 Evaluator/archive 流程保证，不进入 tasks 计数）：

- `integration_surface_check.sh --plan-check` 通过
- TDD exception `ebpf-attach-env` 替代验证证据归档（启动日志样本 + 静态审查记录）
- OBS-1 处置：`蓝牙监控优化实现方案.md` 移入 `docs/archive/` 或删除
- Evaluator 独立验收 Pass（fresh evidence）

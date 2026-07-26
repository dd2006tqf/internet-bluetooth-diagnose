# Design — Bluetooth Monitor A2DP + eBPF Fusion

> 调研基线：`openspec/specs/`（空，本 change 为首个能力规格）、`.ai-harness/project-profile.json`（`weaknet-server` 模块，`make -C server` 构建、`bash server/test/run_all_tests.sh` 测试）、`server/include/bt_monitor.hpp`、`server/src/bt_monitor.cpp`、`server/src/bpf/flow_rate.bpf.c`、`server/src/net_traffic.cpp`、`server/src/event_manager.cpp`、`server/src/server.cpp`、`server/Makefile`、`server/test/unit/`。本 change 由 `蓝牙监控优化实现方案.md` v2.0 迁移而来，设计原文已评审定稿。

## Goals / Non-Goals

见 `proposal.md`。补充设计层非目标：不追求 eBPF 会话到 `MediaTransport1` 对象的精确映射（首版设备级聚合）。

## Alternatives

A2DP 音频质量监控存在两条真实路线，频段冲突与距离估算无路线分歧：

| 维度 | 路线一：纯 D-Bus 轮询 | 路线二：eBPF + D-Bus 融合 |
| ---- | -------------------- | ------------------------- |
| 数据语义 | BlueZ 上报状态（传输层语义） | 状态 + 内核真实流量（数据路径实证） |
| 区分"active 但卡顿" | ❌ | ✅（active + 无流量 = 疑似停滞） |
| 区分"暂停 vs 停滞" | ❌ | ✅（短时无流量=暂停，长时=停滞） |
| 丢包/发送空洞感知 | ❌ | ✅ |
| 实施成本 | 低（4-5 天） | 高（8-12 天） |
| 内核依赖 | 无 | 内核 5.10+、root、可用蓝牙挂点 |
| ABI 稳定性风险 | 低 | 高（kprobe/tracepoint 非稳定 ABI） |
| 降级路径 | 不可用则跳过 | eBPF 不可用自动退化为路线一 |

### 选定方案

**路线二（eBPF + D-Bus 融合）为目标架构，分阶段交付。** 核心理由：

1. 路线一致命短板：纯 D-Bus 无法区分"正在播放但卡顿"与"正常播放"——这是音频质量监控的核心诉求。BlueZ `State=active` 仅表示传输会话被音频服务获取，不代表声音真在流动。
2. 路线二的降级路径天然覆盖路线一：eBPF 不可用时自动退化为纯 D-Bus，路线二是路线一的超集。
3. 项目已有 eBPF 基建可复用（见 Reuse）。
4. 风险可控：启动期挂点探测（tracepoint→fentry→kprobe→纯 D-Bus）+ 启动日志明示降级原因，将 ABI 不稳定风险隔离在 eBPF 子模块内。

频段冲突与距离估算无有意义备选，单一路径 sufficient，不强行造对比。

## Chosen Approach

三层职责划分：

| 层 | 职责 | 回答的问题 |
| --- | ---- | ---------- |
| D-Bus 层（语义层） | `MediaTransport1` 获取会话状态/编解码器/传输层 Delay | "BlueZ 认为当前是什么状态" |
| eBPF 层（数据层） | 挂载内核 L2CAP 发送路径，统计字节/包/间隔/错误 | "内核数据路径实际有没有持续发送" |
| 融合层（判断层） | 组合 D-Bus 状态与 eBPF 流量，输出 `effective_active`/`suspected_stall` 等可解释结论 | "当前音频传输是否真正正常" |

关键算法：

- 频段冲突：维护 Wi-Fi/蓝牙 RSSI 各 30 样本历史，前 20 样本算基线，两者同时低于基线 10dBm 以上标记疑似，Pearson > 0.7 确认，置信度 `min(wifiDrop,btDrop)/20*50 + correlation*50`。
- 距离估算：对数路径损耗模型 `RSSI(d) = RSSI(d0) - 10*n*log10(d/d0)`，默认 `txPower=-59dBm`、`n=2.5`（室内）。
- 融合判据：`effective_active = (dbusState==active) && (bytesDuringWindow > minBytes)`；`suspected_stall = (dbusState==active) && (now - lastPacketTime > stallThreshold)`；仅在 active 且有流量窗口累计时间加权 Delay。
- eBPF 始终加载，通过 BPF Map 控制 `enabled` 开关，不每次状态变化都卸载/重载。

线程调度（不新增线程）：频段冲突 feed+detect 在 `network_quality_thread`（15s）；距离估算 + A2DP D-Bus 刷新 + eBPF map 读取 + 融合评估在蓝牙监测线程 `refreshDeviceStates()` 内（3s）；D-Bus PropertiesChanged 事件驱动；低频校准轮询 10-30s 兜底。

## TDD Applicability

- **纯算法函数（Pearson、距离估算、音频评分、融合判据）**：TDD 适用，RED→GREEN→REFACTOR→REGRESSION。已有单元测试 `server/test/unit/test_band_conflict_detector.cpp`、`test_bt_audio_fusion.cpp` 等覆盖。
- **D-Bus 采集与降级**：依赖 BlueZ 运行时，TDD 部分适用——纯逻辑用单测，接口存在性/降级路径用 `dbusmock` 模拟 + 手动场景测试。
- **eBPF 挂点探测与降级**：依赖内核版本与 root，TDD 不适用（环境强耦合）。
  - **Exception ID: EX-1**
  - Scope: eBPF attach/降级路径（`BtAudioAnalyzer::start` 的 tracepoint/fentry/kprobe 探测分支）。
  - Reason: 测试环境内核/蓝牙挂点不可控，无法稳定复现 attach 成功与各分支失败。
  - Alternative verification: 启动日志断言（"eBPF mode: enabled"/"fallback: D-Bus only, reason: ..."）+ 低内核环境手动降级测试 + 静态代码审查挂点优先级顺序。
  - Exit condition: 获得 root + 内核 ≥5.10 + 可用蓝牙挂点的可控测试机后，补 `BPF_MAP` 控制开关与 `getStats` 集成测试。
  - User approval: 迁移自既有方案 v2.0 第 8.1 节，已隐式接受。

<!-- autoai:tdd-policy:v1 -->
```json
{
  "schema_version": 1,
  "default": "required",
  "exceptions": [
    {
      "id": "ebpf-attach-env",
      "category": "unavailable_hardware",
      "task_ids": ["6", "7"],
      "paths": [
        "server/src/bpf/a2dp_media.bpf.c",
        "server/include/bt_audio_analyzer.hpp",
        "server/src/bt_audio_analyzer.cpp"
      ],
      "reason": "eBPF 内核态挂点探测与降级路径依赖内核版本、root 权限与可用蓝牙挂点，测试环境不可控，无法稳定复现 attach 成功与各分支失败",
      "alternative_verify_kinds": ["build", "static"],
      "exit_condition": "获得 root + 内核>=5.10 + 可用蓝牙挂点的可控测试机后，补 BPF Map 控制开关与 getStats 集成测试"
    }
  ]
}
```
<!-- /autoai:tdd-policy:v1 -->

## Implementation Economy v2 Budget

<!-- autoai:implementation-economy:v2 -->
```json
{
  "schema_version": 2,
  "profile": "medium",
  "rationale": "蓝牙监控 A2DP+eBPF 融合变更，迁移自方案 v2.0。T1-T9 既有实现（频段冲突/A2DP/距离/eBPF/融合层），T10 收尾（集成测试+RAG）。预算 17 人日，已耗 13.5，剩余 3.5（M4）。",
  "classification": {
    "production": ["server/src/**", "server/include/**", "server/bin/**"],
    "tests": ["server/test/**"],
    "project_docs": [],
    "project_tooling": ["server/Makefile", "AI-assisted analysis/**", "scripts/**", ".gitignore", "test_cross_compile.sh"],
    "examples": [],
    "generated": [],
    "vendor": []
  },
  "thresholds": {
    "production": {
      "added_lines": { "expected": 1500, "review_at": 3000, "hard_limit": 6000 },
      "touched_files": { "expected": 10, "review_at": 20, "hard_limit": 40 },
      "new_files": { "expected": 5, "review_at": 10, "hard_limit": 20 }
    },
    "tests": {
      "added_lines": { "expected": 300, "review_at": 800, "hard_limit": 2000 },
      "touched_files": { "expected": 2, "review_at": 5, "hard_limit": 12 },
      "new_files": { "expected": 1, "review_at": 3, "hard_limit": 8 }
    },
    "project_support": {
      "added_lines": { "expected": 20, "review_at": 300, "hard_limit": 800 },
      "new_files": { "expected": 1, "review_at": 5, "hard_limit": 15 }
    },
    "generated": {
      "files": { "expected": 0, "review_at": 3, "hard_limit": 10 },
      "bytes": { "expected": 0, "review_at": 50000, "hard_limit": 200000 }
    }
  },
  "structural_allowances": {
    "public_contracts": [
      { "id": "pc-dbus-signal", "name": "NetworkQualityChanged D-Bus signal", "reason": "新增 band_conflict/bt_distance/bt_audio_quality 载荷字段，向后兼容" },
      { "id": "pc-netinfo-json", "name": "NetInfo JSON serialization", "reason": "新增蓝牙字段，向后兼容" }
    ],
    "build_targets": [
      { "id": "bt-server", "name": "weaknet-dbus-server", "reason": "主服务二进制" },
      { "id": "bt-unit-tests", "name": "unit test binaries", "reason": "单元测试二进制" }
    ],
    "build_graph_entries": [
      { "id": "bge-ebpf", "name": "a2dp_media.bpf.o compile rule", "reason": "eBPF 对象编译规则" }
    ],
    "distribution_surfaces": [
      { "id": "ds-ebpf-obj", "name": "weaknet-dbus-server", "reason": "eBPF 对象随主包部署到 /usr/lib/weaknet/" }
    ],
    "direct_dependencies": [
      { "id": "dd-libbpf", "name": "libbpf >=1.0", "reason": "eBPF 加载" },
      { "id": "dd-dbus", "name": "dbus-1", "reason": "D-Bus 通信" },
      { "id": "dd-glog", "name": "glog", "reason": "日志" }
    ]
  },
  "reuse_decisions": [
    { "id": "REUSE-1", "path": "server/src/bpf/flow_rate.bpf.c", "symbol": "libbpf+CO-RE+vmlinux.h pattern", "decision": "reuse", "reason": "a2dp_media.bpf.c 复用相同 eBPF 模式" },
    { "id": "REUSE-2", "path": "server/src/net_traffic.cpp", "symbol": "NetTrafficAnalyzer bpf_object__open/load/attach", "decision": "reuse", "reason": "BtAudioAnalyzer 复用加载/降级模式" },
    { "id": "REUSE-3", "path": "server/src/bt_monitor.cpp", "symbol": "BtMonitor.getRssiSnapshot", "decision": "reuse", "reason": "复用既有 RSSI 数据源" },
    { "id": "REUSE-4", "path": "server/src/event_manager.cpp", "symbol": "EventManager", "decision": "reuse", "reason": "复用统一事件路由" }
  ],
  "obsolete_items": [
    { "id": "OBS-1", "path": "蓝牙监控优化实现方案.md", "symbol": "implementation plan doc", "disposition": "deprecate", "reason": "迁移为 OpenSpec change 后避免双事实源", "exit_condition": "本 change archive 时移入 docs/archive/", "task_or_debt": "archive 前置" }
  ],
  "exceptions": [
    {
      "id": "EX-1",
      "metric": "ebpf-attach-env",
      "paths": ["server/src/bpf/a2dp_media.bpf.c", "server/include/bt_audio_analyzer.hpp", "server/src/bt_audio_analyzer.cpp"],
      "reason": "eBPF 内核态挂点探测依赖内核版本/root/蓝牙挂点，测试环境不可控，无法稳定复现 attach 成功与各分支失败",
      "requirement_refs": ["eBPF Bluetooth Traffic Collection|挂点探测降级", "eBPF Bluetooth Traffic Collection|启动成功日志"],
      "task_ids": ["6", "7"],
      "verification": "build + static review + 启动日志断言（eBPF mode: enabled/fallback reason）"
    }
  ]
}
```
<!-- /autoai:implementation-economy:v2 -->

## Changed Paths Classification

| 路径 | 分类 | 说明 |
| ---- | ---- | ---- |
| `server/include/band_conflict_detector.hpp` | production-new | 频段冲突检测器接口 |
| `server/src/band_conflict_detector.cpp` | production-new | 实现 |
| `server/include/bt_monitor.hpp` | production-modified | 新增音频/距离/融合接口 |
| `server/src/bt_monitor.cpp` | production-modified | 新增 `refreshAudioTransports`/`estimateDistance`/融合层接入 |
| `server/include/net_info.hpp` | production-modified | 新增蓝牙字段 |
| `server/src/net_info.cpp` | production-modified | 序列化扩展 |
| `server/src/bpf/a2dp_media.bpf.c` | production-new | eBPF 程序 |
| `server/include/bt_audio_analyzer.hpp` | production-new | eBPF 分析器接口 |
| `server/src/bt_audio_analyzer.cpp` | production-new | 实现 |
| `server/include/bt_audio_fusion.hpp` | production-new | 融合层接口 |
| `server/src/bt_audio_fusion.cpp` | production-new | 实现 |
| `server/src/server.cpp` | production-modified | `network_quality_thread` 接入冲突检测 |
| `server/Makefile` | build-metadata-modified | eBPF 对象编译规则 |
| `server/test/unit/test_band_conflict_detector.cpp` | test-new | 单元测试 |
| `server/test/unit/test_bt_audio_fusion.cpp` | test-new | 单元测试 |
| `server/test/unit/test_event_manager.cpp` | test-modified | eventCounter 原子性并发测试（commit `08165b8`） |

### Reuse Candidates

- **REUSE-1**：复用 `flow_rate.bpf.c` 的 libbpf + CO-RE + vmlinux.h 模式（`a2dp_media.bpf.c`）。
- **REUSE-2**：复用 `NetTrafficAnalyzer` 的 `bpf_object__open/load/attach` + map 轮询 + 降级模式（`BtAudioAnalyzer`）。
- **REUSE-3**：复用 `BtMonitor.getRssiSnapshot()` 与 `NetInfo.rssiDbm()` 既有 RSSI 数据源。
- **REUSE-4**：复用 `EventManager` 统一事件路由。

### Obsolete Item Disposition

- **OBS-1**：`蓝牙监控优化实现方案.md`（迁移源文档）。本 change 归档后建议移入 `docs/archive/` 或删除，避免双事实源。处置时机：archive 时。
- 无废弃代码项。

## Risks

| ID | 风险 | 等级 | 应对 |
| -- | ---- | ---- | ---- |
| R1 | BlueZ 版本差异致 MediaTransport1 不可用 | 中 | 探测接口存在性，不可用降级跳过 |
| R2 | Pearson 样本不足致误判 | 高 | 最少 5 样本，不足不输出结论 |
| R3 | 距离估算误差大（>50%） | 高 | 输出"距离区间"而非精确值 |
| R6 | eBPF 挂点内核升级失效 | 高 | 启动期探测 tracepoint→fentry→kprobe→纯 D-Bus，日志明示 |
| R7 | 会话映射错误 | 中 | 首版按 `hci_index+BDADDR` 聚合为设备级 |
| R9 | LE Audio 不走 AVDTP | 中 | 文档标注不适用，第一版仅经典蓝牙 A2DP |

依赖与兼容：libbpf ≥1.0、clang ≥14、BlueZ ≥5.50、bpftool ≥7.0、内核 ≥5.10。回滚：eBPF 模块独立，失败降级为纯 D-Bus 不影响主进程；新 NetInfo 字段向后兼容，回滚仅需移除新字段。迁移：无外部数据迁移。

## Integration Completeness v1

> 下方为机器可读的 Integration Completeness v1 计划块（`reviewed_inventory` 模式，单一 `build_or_install` surface 覆盖构建与单元测试探针）。archive 前需 `integration_surface_check.sh --plan-check` 对照实际代码核验。

<!-- autoai:integration-completeness:v1 -->
```json
{
  "discovery": { "compile_commands_path": null, "mode": "reviewed_inventory" },
  "schema_version": 1,
  "surfaces": [
    {
      "id": "surface-weaknet-server-build",
      "kind": "build_or_install",
      "consumer_kind": "downstream_build",
      "change_kind": "modified",
      "contract_impact": "compatible",
      "name": "weaknet-dbus-server build and unit tests",
      "entrypoint": "server/bin/weaknet-dbus-server",
      "expected_observation": "make -C server produces weaknet-dbus-server and run_all_tests.sh reports PASS",
      "producer_paths": [
        "server/src/band_conflict_detector.cpp",
        "server/src/bt_monitor.cpp",
        "server/src/bt_audio_analyzer.cpp",
        "server/src/bt_audio_fusion.cpp",
        "server/src/bpf/a2dp_media.bpf.c",
        "server/src/net_info.cpp",
        "server/src/server.cpp",
        "server/src/event_manager.cpp",
        "server/Makefile"
      ],
      "consumer_paths": ["server/test/run_all_tests.sh"],
      "compatibility": null,
      "requirement_refs": [
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "Band Conflict Detection", "scenarios": ["完全正相关输入"] },
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "Band Conflict Detection", "scenarios": ["样本不足降级"] },
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "Band Conflict Detection", "scenarios": ["频段冲突确认"] },
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "Band Conflict Detection", "scenarios": ["周期调用"] },
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "A2DP Audio Quality Monitoring", "scenarios": ["音频质量评分"] },
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "A2DP Audio Quality Monitoring", "scenarios": ["严重延迟扣分"] },
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "A2DP Audio Quality Monitoring", "scenarios": ["MediaTransport1 接口缺失降级"] },
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "A2DP Audio Quality Monitoring", "scenarios": ["三层采集机制"] },
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "Device Distance Estimation", "scenarios": ["1 米处估算"] },
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "Device Distance Estimation", "scenarios": ["RSSI 为零降级"] },
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "Device Distance Estimation", "scenarios": ["校准接口"] },
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "eBPF Bluetooth Traffic Collection", "scenarios": ["设备级聚合"] },
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "eBPF Bluetooth Traffic Collection", "scenarios": ["BPF Map 控制开关"] },
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "eBPF Bluetooth Traffic Collection", "scenarios": ["挂点探测降级"] },
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "eBPF Bluetooth Traffic Collection", "scenarios": ["启动成功日志"] },
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "D-Bus eBPF Fusion", "scenarios": ["正常音频传输"] },
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "D-Bus eBPF Fusion", "scenarios": ["疑似停滞"] },
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "D-Bus eBPF Fusion", "scenarios": ["三种活跃时长"] },
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "D-Bus eBPF Fusion", "scenarios": ["时间加权 Delay"] },
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "D-Bus eBPF Fusion", "scenarios": ["状态矛盾标记"] },
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "NetInfo Bluetooth Extension", "scenarios": ["新增字段"] },
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "NetInfo Bluetooth Extension", "scenarios": ["向后兼容"] },
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "NetInfo Bluetooth Extension", "scenarios": ["序列化往返"] },
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "Event Routing", "scenarios": ["频段冲突事件"] },
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "Event Routing", "scenarios": ["音频/距离事件"] },
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "Event Routing", "scenarios": ["eventCounter 原子性"] },
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "RAG Diagnostics", "scenarios": ["频段冲突查询"] },
        { "spec_path": "specs/bluetooth-monitoring/spec.md", "operation": "ADDED", "requirement": "RAG Diagnostics", "scenarios": ["音频延迟查询"] }
      ],
      "task_ids": ["1", "2", "3", "4", "5", "6", "7", "8", "9", "10"],
      "verify_kinds": ["build", "test"],
      "task_obligations": [
        { "task_id": "1", "verify_kinds": ["build", "test"], "evidence_roles": ["current"] },
        { "task_id": "2", "verify_kinds": ["build", "test"], "evidence_roles": ["current"] },
        { "task_id": "3", "verify_kinds": ["build", "test"], "evidence_roles": ["current"] },
        { "task_id": "4", "verify_kinds": ["build", "test"], "evidence_roles": ["current"] },
        { "task_id": "5", "verify_kinds": ["build", "test"], "evidence_roles": ["current"] },
        { "task_id": "6", "verify_kinds": ["build"], "evidence_roles": ["current"] },
        { "task_id": "7", "verify_kinds": ["build"], "evidence_roles": ["current"] },
        { "task_id": "8", "verify_kinds": ["build", "test"], "evidence_roles": ["current"] },
        { "task_id": "9", "verify_kinds": ["build", "test"], "evidence_roles": ["current"] },
        { "task_id": "10", "verify_kinds": ["test"], "evidence_roles": ["current"] }
      ],
      "evidence_contracts": [
        { "probe_id": "probe-build-current", "kind": "build", "role": "current", "argv": ["scripts/project_command.sh", "build-server", "--change", "bt-a2dp-ebpf-fusion", "--json"], "expected_exit_codes": [0], "output_contains": "weaknet-dbus-server" },
        { "probe_id": "probe-test-current", "kind": "test", "role": "current", "argv": ["scripts/project_command.sh", "test-all", "--change", "bt-a2dp-ebpf-fusion", "--json"], "expected_exit_codes": [0], "output_contains": "PASS" }
      ],
      "symbol_identities": null,
      "runnable_artifact": true
    }
  ]
}
```
<!-- /autoai:integration-completeness:v1 -->

## Self-Review

- 矛盾检查：non-goals 排除 LE Audio，与 R9 风险应对一致；Phase 2 降级路径与"路线二=路线一超集"一致；无矛盾。
- 占位符：Integration Completeness 块的 `integration_surface_check.sh --plan-check` 核验与 EX-1 exit condition 为显式待办，archive 前完成，非占位空洞。
- 范围蔓延：本 change 不扩展到 HFP/Mesh/LE Audio，与 proposal non-goals 一致。
- 双事实源：源文档 `蓝牙监控优化实现方案.md` 与本 change 共存，已在 OBS-1 标注归档时机，archive 时处置。

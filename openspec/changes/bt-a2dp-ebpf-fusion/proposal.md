# Proposal — Bluetooth Monitor A2DP + eBPF Fusion

> 迁移自 `蓝牙监控优化实现方案.md` v2.0（2026-07-24 定稿）。本 change 将该方案纳入 OpenSpec 治理，使其具备结构化、可归档、可验收的形态。方案文档原稿保留于仓库根目录，待本 change 归档后可作为历史参考资料删除或移入 `docs/archive/`。

## Why

当前 `WEAK_NET Server` 的蓝牙监控线程仅覆盖 BlueZ 适配器状态、设备发现、RSSI、连接/配对状态，存在三类关键能力空白：

| 不足项 | 影响 | 优先级 |
| ------ | ---- | ------ |
| 不监控蓝牙音频质量 | 无法发现"蓝牙卡顿"问题 | 中 |
| 无法检测 Wi-Fi/蓝牙频段冲突 | 漏报"2.4GHz 干扰"场景 | 高 |
| 无法估算设备距离 | 缺乏空间感知能力 | 低 |

补齐这三类能力后，监控系统可从"信号弱"升级到"是干扰还是距离远"的精确归因，并补全多协议协同分析亮点。

## Goals

- **G1（P0）频段冲突检测**：关联 Wi-Fi RSSI 与蓝牙 RSSI，基于 Pearson 相关性识别 2.4GHz 频段冲突，输出置信度与处置建议。
- **G2（P1）A2DP 音频质量监控**：通过 BlueZ `MediaTransport1` 获取音频传输状态/延迟/编解码器，输出音频质量评分；叠加 eBPF 内核真实流量，区分"active 但卡顿"与"正常播放"。
- **G3（P2）设备距离估算**：基于 RSSI 对数路径损耗模型估算设备距离，支持校准接口。

## Non-Goals

- LE Audio / BAP 协议支持（第一版仅经典蓝牙 A2DP）。
- 蓝牙 HFP 免提通话质量监控。
- 蓝牙 Mesh 网络监控。
- 新增独立线程（严格复用现有蓝牙监测线程 3s 与 `network_quality_thread` 15s）。
- 将 eBPF 会话指标精确映射到单个 `MediaTransport1` 对象（首版按 `hci_index + BDADDR` 聚合为设备级）。

## Affected Capabilities

| 能力 | 类型 | 主要代码区域 |
| ---- | ---- | ------------ |
| 频段冲突检测 | 新增 | `server/include/band_conflict_detector.hpp`、`server/src/band_conflict_detector.cpp` |
| A2DP 音频质量监控 | 新增（扩展 BtMonitor） | `server/include/bt_monitor.hpp`、`server/src/bt_monitor.cpp` |
| 设备距离估算 | 新增（扩展 BtMonitor） | 同上 |
| eBPF 蓝牙流量采集 | 新增 | `server/src/bpf/a2dp_media.bpf.c`、`server/include/bt_audio_analyzer.hpp`、`server/src/bt_audio_analyzer.cpp` |
| D-Bus/eBPF 融合层 | 新增 | `server/include/bt_audio_fusion.hpp`、`server/src/bt_audio_fusion.cpp` |
| NetInfo 数据结构 | 修改 | `server/include/net_info.hpp`、`server/src/net_info.cpp` |
| 事件路由 | 修改（复用） | `server/src/event_manager.cpp`、`server/src/server.cpp` |
| 构建 | 修改 | `server/Makefile`（eBPF 对象编译规则） |

## External Contract Impact

- **D-Bus 信号**：`compatible`。新增 `band_conflict` 类型的 `NetworkQualityChanged` 信号载荷字段（`bt_distance`/`bt_audio_quality`/`band_conflict`/`band_conflict_confidence`），均为 NetInfo JSON 的**新增字段**，不删除/重命名既有字段，旧消费者忽略新字段即可。
- **NetInfo JSON 序列化**：`compatible`（向后兼容新增字段）。
- **eBPF 内核 ABI**：`none`（对用户态外部契约无影响）；内核侧挂点为非稳定 ABI，已在设计内通过启动期探测 + 降级隔离风险。
- **无 BREAKING 变更**。

## Phased Delivery

三阶段独立可上线，后续阶段为增强升级：

| 阶段 | 里程碑 | 内容 | 状态 |
| ---- | ------ | ---- | ---- |
| Phase 1a | M1 | 频段冲突检测器 + `network_quality_thread` 接入 | ✅ 已实现（commit `27b05ee`） |
| Phase 1b | M2 | A2DP 纯 D-Bus 监控 + 设备距离估算 + NetInfo 扩展 | ✅ 已实现 |
| Phase 2 | M3 | eBPF 蓝牙流量采集 + D-Bus/eBPF 融合层 + 降级探测 | ✅ 已实现（commit `e684783`、`08165b8`） |
| 集成与 RAG | M4 | 集成测试 + RAG 知识库诊断条目 | ⏳ 待完成 |

> 注：本 change 创建时 Phase 1a/1b/2 代码已落地（迁移自既有实施方案）。OpenSpec 治理重点覆盖 M4 收尾与归档验收，同时对既有实现补结构化验收基线。

## Reuse Points

- 复用 `flow_rate.bpf.c` 的 libbpf + CO-RE + vmlinux.h 模式与 `NetTrafficAnalyzer` 的 `bpf_object__open/load/attach` + map 轮询 + 降级模式。
- 复用 `BtMonitor.getRssiSnapshot()`（3s）与 `NetInfo.rssiDbm()`（10s）既有 RSSI 数据，避免重复采集。
- 复用 `EventManager` 统一事件路由，不新建事件通道。

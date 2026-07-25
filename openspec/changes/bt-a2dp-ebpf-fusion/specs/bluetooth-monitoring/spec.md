# Spec Delta — Bluetooth Monitoring

> 本文件为本 change 相对主规格 `openspec/specs/bluetooth-monitoring/spec.md`（尚不存在，本 change 为首个能力规格，archive 时创建）的增量。所有需求均为 ADDED。

## ADDED Requirements

### Requirement: Band Conflict Detection

The system MUST correlate Wi-Fi RSSI with Bluetooth RSSI time series and, based on drop correlation and the Pearson coefficient, identify 2.4GHz band conflicts, emitting a confidence score and remediation suggestion. 系统关联 Wi-Fi RSSI 与蓝牙 RSSI 时间序列，基于下降相关性与 Pearson 系数识别 2.4GHz 频段冲突，并输出置信度与处置建议。

#### Scenario: 完全正相关输入

- **WHEN** Wi-Fi RSSI 与蓝牙 RSSI 历史各 30 样本且两者完全正相关（同步等幅下降）
- **THEN** Pearson 相关系数计算结果为 1.0，置信度 ≥ 50%

#### Scenario: 样本不足降级

- **WHEN** Wi-Fi 或蓝牙 RSSI 历史队列长度 < 5
- **THEN** 检测器返回 `detected=false` 的空结果，不输出任何冲突结论

#### Scenario: 频段冲突确认

- **WHEN** Wi-Fi RSSI 与蓝牙 RSSI 同时低于各自基线 10dBm 以上，且 Pearson 相关系数 > 0.7
- **THEN** `detected=true`，置信度 = `min(wifiDrop, btDrop)/20*50 + correlation*50`，并输出处置建议

#### Scenario: 周期调用

- **WHEN** `network_quality_thread` 每 15 秒触发一次
- **THEN** 检测器被 feed 最新 RSSI 样本并执行 `detect()`

### Requirement: A2DP Audio Quality Monitoring

The system MUST collect A2DP audio transport state, delay, codec and volume via the BlueZ `org.bluez.MediaTransport1` interface and emit a 0-100 audio quality score. 系统通过 BlueZ `org.bluez.MediaTransport1` 接口采集 A2DP 音频传输状态、延迟、编解码器、音量，并输出 0-100 的音频质量评分。

#### Scenario: 音频质量评分

- **WHEN** 某 MediaTransport1 的 `Delay` 为 2500（1/10ms 单位，即 250ms）且 `Codec` 为 0x00（SBC）
- **THEN** 质量评分 = 100 - 10（Delay>500 扣 10）- 5（SBC 扣 5）= 85

#### Scenario: 严重延迟扣分

- **WHEN** `Delay` > 2000（即 >200ms... 实际 >2000 表示 >200ms，按方案 Delay>2000 扣 40）
- **THEN** 评分扣 40 分

#### Scenario: MediaTransport1 接口缺失降级

- **WHEN** 启动时 `GetManagedObjects` 未找到 `org.bluez.MediaTransport1` 接口
- **THEN** 跳过音频监控，日志输出 "MediaTransport1 not available, skip audio monitoring"，不崩溃

#### Scenario: 三层采集机制

- **WHEN** 运行时 `State`/`Delay` 属性变化
- **THEN** 通过 `PropertiesChanged` 信号立即更新缓存（事件驱动），并以 10-30s `GetAll` 校准轮询兜底

### Requirement: Device Distance Estimation

The system MUST estimate Bluetooth device distance using the RSSI log-distance path-loss model and provide a calibration interface. 系统基于 RSSI 对数路径损耗模型估算蓝牙设备距离，并支持校准接口。

#### Scenario: 1 米处估算

- **WHEN** 设备距适配器 1 米，`rssiDbm` 等于校准 `txPower`（默认 -59dBm）
- **THEN** `estimateDistance` 返回值 ≈ 1.0 米

#### Scenario: RSSI 为零降级

- **WHEN** `rssiDbm == 0` 或 `txPower == 0`
- **THEN** `estimatedDistance = -1.0`（未知），不输出距离结论

#### Scenario: 校准接口

- **WHEN** 调用 `calibrateDistance(mac, knownDistanceMeters)` 在已知距离采集 30 次 RSSI 取均值
- **THEN** 校准值持久化，后续距离估算更接近真实值

### Requirement: eBPF Bluetooth Traffic Collection

The system MUST use an eBPF program on the kernel L2CAP send path to collect Bluetooth audio traffic (bytes/packets/gaps/errors) aggregated per device. 系统通过 eBPF 程序在内核 L2CAP 发送路径上统计蓝牙音频流量（字节/包/间隔/错误），并按设备聚合。

#### Scenario: 设备级聚合

- **WHEN** eBPF 挂点命中 L2CAP 发送
- **THEN** 按 `hci_index + BDADDR + direction` 组成 key 聚合 bytes/packets/send_errors/max_gap_ns/last_packet_ns 到 `bt_stats` BPF Map

#### Scenario: BPF Map 控制开关

- **WHEN** 用户态收到 D-Bus `State=active`
- **THEN** `active_sessions.enabled=1` 写入 BPF Map，eBPF 开始累计详细流量；`State!=active` 时 `enabled=0`，eBPF 停止详细统计
- **AND** eBPF 程序始终加载，不因状态变化卸载/重载

#### Scenario: 挂点探测降级

- **WHEN** 启动期依次探测 tracepoint → fentry → kprobe 均失败
- **THEN** 自动降级为纯 D-Bus 模式（路线一），日志输出 "eBPF mode: unavailable, fallback: D-Bus only, reason: ..."

#### Scenario: 启动成功日志

- **WHEN** eBPF 成功 attach
- **THEN** 日志输出 "eBPF mode: enabled" + 实际挂点名 + "fallback reason: none"

### Requirement: D-Bus eBPF Fusion

The system MUST combine D-Bus semantic state with eBPF kernel traffic to emit explainable conclusions (`effective_active`/`suspected_stall`) and three distinct active-duration metrics. 系统组合 D-Bus 语义状态与 eBPF 内核流量，输出 `effective_active`/`suspected_stall` 等可解释结论与三种活跃时长。

#### Scenario: 正常音频传输

- **WHEN** `dbusState == active` 且当前窗口 `bytes > minBytes`
- **THEN** `effective_active = true`，`suspected_stall = false`

#### Scenario: 疑似停滞

- **WHEN** `dbusState == active` 且 `now - lastPacketTime > stallSevereMs`（默认 1000ms）
- **THEN** `suspected_stall = true`

#### Scenario: 三种活跃时长

- **WHEN** 融合评估执行
- **THEN** 输出 `bluez_active_duration`（State==active 累计）、`traffic_active_duration`（bytes_delta>threshold 窗口累计）、`effective_active_duration`（active 且有流量累计）三个独立时长

#### Scenario: 时间加权 Delay

- **WHEN** `State == active` 且当前窗口有实际流量
- **THEN** Delay 按窗口时长加权累计平均；空闲期 Delay 不污染统计

#### Scenario: 状态矛盾标记

- **WHEN** `dbusState == idle` 但 eBPF 持续检测到流量
- **THEN** 标记为会话映射错误或 D-Bus 状态不同步，不输出 `effective_active`

### Requirement: NetInfo Bluetooth Extension

NetInfo MUST be extended with Bluetooth-related fields while keeping JSON serialization backward compatible. NetInfo 数据结构扩展蓝牙相关字段，并保持 JSON 序列化向后兼容。

#### Scenario: 新增字段

- **WHEN** NetInfo 序列化为 JSON
- **THEN** 输出含 `bt_distance`/`bt_audio_quality`/`band_conflict`/`band_conflict_confidence` 字段

#### Scenario: 向后兼容

- **WHEN** 解析不含新字段的旧 JSON
- **THEN** 新字段取缺省值（`bt_distance=-1.0`、`bt_audio_quality=""`、`band_conflict=false`、`band_conflict_confidence=0.0`），解析不失败

#### Scenario: 序列化往返

- **WHEN** NetInfo 对象 `toJson` 后再 `fromJson`
- **THEN** 所有蓝牙字段值一致

### Requirement: Event Routing

All new Bluetooth events MUST be routed through the EventManager and exposed via D-Bus signals, and the eventCounter MUST be duplicate-free under multi-threaded concurrency. 所有新增蓝牙事件通过 EventManager 统一路由，经 D-Bus 信号对外暴露，且 eventCounter 在多线程下无重复。

#### Scenario: 频段冲突事件

- **WHEN** 频段冲突检测器输出 `detected=true`
- **THEN** 经 `EventManager.emitNetworkQualityChanged` 推送，`dbus-monitor` 捕获到 `NetworkQualityChanged` 信号含 "band_conflict" 载荷

#### Scenario: 音频/距离事件

- **WHEN** 音频质量降级或设备距离变化
- **THEN** 经 `EventManager.emitBluetoothDeviceChanged` 推送对应事件类型

#### Scenario: eventCounter 原子性

- **WHEN** 10 线程并发各 10000 次 emit
- **THEN** 记录到的 100000 个 counter 值全部唯一（无重复），证明 `std::atomic` 消除 data race

### Requirement: RAG Diagnostics

The RAG knowledge base MUST be augmented with Bluetooth diagnostic entries that return remediation suggestions for band-conflict and audio-stall queries. RAG 知识库补充蓝牙诊断条目，能基于自然语言查询返回频段冲突与音频卡顿的处置建议。

#### Scenario: 频段冲突查询

- **WHEN** 输入查询"蓝牙卡顿"
- **THEN** RAG 返回频段冲突相关建议（Wi-Fi 切 5GHz / 调信道 / 设备靠近）

#### Scenario: 音频延迟查询

- **WHEN** 输入查询"蓝牙音频延迟高"
- **THEN** RAG 返回编解码器/距离/卡顿相关建议

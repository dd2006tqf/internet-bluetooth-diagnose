# 修复 a2dp_media.bpf.c 的 l2cap_chan.dst 偏移错误

> **严重性**：🔴 高（legacy 能力的数据正确性缺陷）
> **状态**：已修复（commit `c6791cb`：`L2CAP_CHAN_DST_OFFSET = 33`，并补全 `dst_type`/`src` 字段核对与 BTF 依据注释）
> **发现日期**：2026-09-29（Phase 1 蓝牙事件采集的板端 BTF 实测过程中顺带发现）
> **依据**：`docs/蓝牙监控优化实现方案.md` 第 11.6 节

## 一句话摘要

`server/src/bpf/a2dp_media.bpf.c` 曾把内核 `l2cap_chan` 结构的 `dst`（目标
BDADDR）字段偏移硬编码为 `+24`，而板端 BTF 实测真实偏移是 `33`——曾经运行的
A2DP 地址提取读的是错误内存（已修复，见状态栏）。

## 动机 / 痛点

- 现场证据：`/sys/kernel/btf/bluetooth` 提取出的 `l2cap_chan` 布局为
  `conn=0 / hs_hcon=8 / hs_hchan=16 / kref=24 / nesting=28 / state=32 / dst=33`，
  即代码注释里假设的 "offset 24 = dst" 实际是 `kref`。
- 后果：`extract_bdaddr_kprobe()` 取出的 6 字节不是目标设备地址，
  → `active_sessions` / `bt_traffic` 按设备统计的字节、包、gap 全部可能
  挂在错误的设备 key 上，`getAudioFusionResult()` 的设备级结论不可信。
- 该缺陷只影响 **legacy A2DP 音频观测能力**，与工业无线诊断主线
  （`bt_events`，走 `hci_conn.dst=20` 且经 BTF 实锤）无关，因此不混入
  当前链路，独立修复。

## 初步技术方案

1. 用板端 BTF（`bpftool btf dump file /sys/kernel/btf/bluetooth -B <vmlinux.btf>`）
   重新导出 `l2cap_chan` 全字段偏移，不只修 `dst`——把 `dst_type`、`src` 一并
   核对。
2. 参照 `bt_events.bpf.c` 的既有做法：`bpf_probe_read_kernel` + 失败即丢弃
   的容错读取，禁止裸指针算术；把偏移定义为具名常量并附 BTF 依据注释。
3. 需要真机 A2DP 环境验证（当前板端无 `MediaTransport1`，此前日志显示
   `MediaTransport1 not available`），这是排期时的前置条件。
4. 顺带评估：`a2dp_media.bpf.c` 是否应迁移到"模块 BTF 驱动的偏移表"而非
   硬编码，避免同类问题复发（与 `bt_events` 的实测偏移注释风格对齐）。

## 涉及的模块

- `server/src/bpf/a2dp_media.bpf.c`（`struct l2cap_chan_minimal`、`extract_bdaddr_kprobe`）
- 验证面：`server/src/bt_audio_analyzer.cpp` / `bt_audio_fusion.cpp`（下游消费者，只测不改）

## 优先级状态

- [x] 已确认根因（BTF 实锤 + 代码走查）
- [x] 已登记（本文档 + `docs/蓝牙监控优化实现方案.md` 11.6 节 + 产品定位文档"已知遗留"）
- [ ] 排期实施（建议独立 change：修复 + A2DP 真机回归）
- [ ] 修复合入并验证

## 参考资料

- `docs/蓝牙监控优化实现方案.md` 第 11 章（板端挂点实测与结构偏移表）
- `server/src/bpf/bt_events.bpf.c`（同类问题的正确处理范式）
- `docs/产品定位-工业无线诊断网关.md` 第九节（遗留项登记）

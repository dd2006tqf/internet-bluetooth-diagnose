# 产品定位文档 v1.2.1：第八节实现状态同步

> 日期：2026-11-08
> 分类：快速修复（文档与代码同步）
> 结果：已完成

## 构思

### 要解决什么问题

`docs/产品定位-工业无线诊断网关.md` 第八节「当前实现状态」标题写的是
「截至 2026-10-01」，但正文已经包含 Phase 4a/4b、S6 资产桥、S7 受限自动化
这些 10 月之后的能力。更严重的是，2026-11-06 / 11-07 落地的四项能力完全没记：

| 已落地但定位文档未记 | 落地提交 |
| --- | --- |
| 采集端热状态观测与归因（`thermal_attribution`） | `b5b24e7`（2026-11-07） |
| 观测者降级归因（`observer_attribution`，CPU 压力） | `c992cc3`（2026-11-06） |
| `weaknet_cpu_stat` 真机上线 | `e5a979e`（2026-11-07） |
| 视频 QoE 归因三字段（生产态恒 `NONE`） | `75e7901`（2026-11-07） |
| 现场交互终端 `qt_dashboard`（已编译，未进发布链） | `a96eed4` / 独立 `qt_dashboard/` |

第八节是给外部读者（面试官、合作方）看「这产品做到哪了」的地方，写的是两个月前的状态，
会让读者系统性低估项目的实际完成度；同时也会掩盖两处「框架已落地但生产态未通」的真实边界。

### 为什么不做成 OpenSpec change

纯文档事实订正，无行为变更、无生产表面变化，符合 `docs/ai/工作流使用指南.md`
「改 typo、改注释 → 不用工作流」的判断。按 `CLAUDE.md`「工作流外任务必须留记录」
落本文件。

### 备选方案与取舍

| 方案 | 取舍 |
| --- | --- |
| A. 只改日期 | ❌ 治标，四项能力仍缺失 |
| **B. 日期 + 补全四项能力状态（采用）** | ✅ 最小改动解决事实漂移，不触碰定位语义 |
| C. 同时补「观测者自身状态」正交维度 | ❌ 属定位语义变更，需用户先讨论（本轮只做 A/B 层） |

选择 B：本轮严格限定在「事实同步」，不预设定位结构的修改方向。第五节三层模型、
第三节之二白名单均未改动。

## 实现方式

### 改了哪些文件

`docs/产品定位-工业无线诊断网关.md`，三处：

1. 文档状态行：`v1.2` → `v1.2.1`，补「第八节实现状态同步 11 月落地能力」
2. 第八节标题日期：`截至 2026-10-01` → `截至 2026-11-08`
3. 第八节表格新增四行（插在 S7 与「知识双向」之间）

### 关键决策：为什么用四行而不是并成两行

thermal 与 observer 是两个独立的纯函数 + 两套字段，合并会掩盖「各自有独立枚举与
`_issues` 字段」的事实。视频与 Qt 看板则分别标注 `⚠️` 而非 `✅`，因为：
- 视频：框架完整但帧源未接，生产态恒 `NONE` —— 这不是故障，是预期形态
- Qt 看板：`weaknet_dashboard` 已编译链接通过，但**不在** `tools/ci.sh`、
  `tools/release.sh`、OTA manifest 内，需手工部署

如果把这两项写成 `✅`，会制造「能力已可用」的假象，与项目「先证据后声明」的纪律冲突。

### 验证手段

全部状态均以代码与提交为证据，不靠记忆：

```bash
# 逐项确认落地提交
for f in server/src/thermal_monitor.cpp \
         server/src/assurance/observer_attribution.cpp \
         server/src/video_qoe_attribution.cpp \
         qt_dashboard/main.cpp \
         kmod/weaknet_cpu_stat.c; do
  git log -1 --format='%h %ad %s' --date=short -- "$f"
done

# 确认 Qt 看板不在发布链
grep -rn 'qt_dashboard' tools/ci.sh tools/release.sh config.yaml   # 无命中
ls dist-arm64/                                                      # 无 qt 产物

# 确认三个归因字段的序列化出口
grep -n 'thermal_attribution\|video_qoe_attribution\|observer_attribution' \
  server/src/server.cpp server/include/assurance/edge_telemetry_serializer.hpp
```

实测结果：

| 检查 | 结果 |
| --- | --- |
| `server.cpp:649-651` | `thermal_attribution` / `_detail` / `thermal_issues` 三字段赋值 |
| `server.cpp:722-730` | `video_qoe_attribution` / `_detail` / `video_qoe_issues` 三字段赋值 |
| `server.cpp:603-604` | `observer_attribution` / `_detail` 两字段赋值 |
| `edge_telemetry_serializer.hpp:150-153` | observer 归因已进遥测序列化 |
| `grep qt_dashboard tools/` | 零命中，确未进发布链 |
| `ls build-qt-arm64/weaknet_dashboard` | 332 KB ARM64 可执行文件，已编译 |

## 结果

### 最终状态

`docs/产品定位-工业无线诊断网关.md` 第八节现在反映 2026-11-08 的真实状态，
共 16 行能力条目，其中 12 项 `✅`、2 项 `⚠️`、2 项 `⬜`（区域级 SiteIncident 的
`suspected_cause` 仍恒 NULL、知识双向未开始）。

### 遗留问题

1. **`qt_dashboard` 的发布路径未定**。当前是独立 CMake 工程、手工部署。是否需要
   纳入 `tools/release.sh` 与 OTA manifest，属发布策略决策，未在本轮处理。
   （相关背景见 `驱动面能力引入与业务融合方案.md` §15.2 第 6 项与 §11 风险登记）
2. **视频归因的 `NONE` 是预期形态，不是缺陷**。需等 `V4l2FrameSource` 落地后才
   可验证，当前不应被当作「已完成能力」对外表述。
3. **「观测者自身状态」仍无产品定位**。thermal / observer 两条线回答的是
   「观测者自己是不是坏了」，与第五节三层模型（设备层 / 链路层 / 区域层）正交。
   本轮只补了实现状态，未补语义定位——留待产品定位讨论。

### 后续是否值得升格为正式 OpenSpec change

不需要。本轮是纯事实订正。但**若补「观测者自身状态」正交维度**，那是一次真实的
语义扩展，会影响后续所有归因类新增（thermal / observer / video 三条线共用同一形态），
届时应走 OpenSpec change 或至少产出一份独立的定位修订记录。

## 相关

- `docs/产品定位-工业无线诊断网关.md`（本次修改对象）
- `驱动面能力引入与业务融合方案.md` §15（已落地能力清单，本次同步的事实来源）
- `docs/架构设计-五层权责契约.md`（L1/L2 边界，决定归因归属）
- `openspec/tasks/2026-11-06-thermal-monitor.md`
- `openspec/tasks/2026-11-06-host-pressure-observer-attribution.md`

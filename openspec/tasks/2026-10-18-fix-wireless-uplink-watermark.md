# 无线上行三域独立游标修复（C2）

> 日期：2026-10-18
> 分类：快速修复
> 结果：已完成

## 构思

全仓 code review 发现 C2【Important】：`EdgeWirelessUplinkExporter` 用**单一
watermark** 同时代表事件/事故/基线三域的"已确认送达"位置。三个查询都按
ASC + LIMIT 截断（事件批上限 1000、事故 200、基线 2000），而推进水位取
三域已发送行时间戳的最大值。当任一域被截断时，另一域更晚的时间戳会把
水位推过截断点之后的未发事实——例如事件积压 1500 条只发最老 1000 条
（最新 ts=T1000），同批事故 last_event_ms=T_inc>T1000，水位被推到 T_inc，
下一轮查询 `ts > T_inc` 永久跳过 (T1000, T_inc] 区间的 500 条事件。
"重发安全、跳发丢事实"的既定原则被单水位破坏。

备选方案：
- A. 三域独立游标（评审推荐，采纳）：state 文件升为多键格式，每个域只按
  本域已发送行的时间戳推进；
- B. 截断时按最新完整批回退水位：实现复杂且回退量难以确定，否决。

## 实现方式

- `server/include/edge_wireless_uplink_exporter.hpp`
  - 新增 `WirelessUplinkCursors{events_ms, incidents_ms, baselines_ms}`；
  - 公开 `loadCursors()/storeCursors()`（替代 `loadWatermark/storeWatermark`，
    仅测试消费的旧接口一并移除，避免孤儿接口）；`storeCursors` 同步内存
    原子游标，文件写失败只影响重启恢复点（重发由云端幂等吸收）；
  - 公开纯函数 `advanceCursors(before, sent)`：每域只取本域已发送行时间戳
    的 max，任何域只增不减；
  - `collect()` 移到 public（与 runOnce 同一"为可测性开放"先例），文档化
    各域边界语义（事件严格 >、事故 >=、基线严格 >，与改造前逐域一致）。
- `server/src/edge_wireless_uplink_exporter.cpp`
  - state 文件仍用 `key=value\n` 行格式（复用 `readUintField` 解析器）：
    `events_ms/incidents_ms/baselines_ms` 三行；旧单键格式升级后缺失的两键
    回退 0 全量重扫（补回单水位时代可能截断丢发的行）；单键损坏只回退该域；
  - `collect()` 三域各用自己的游标查询；`watermark_ms` 回显三域最大值
    （云端契约字段不变，服务端本就不依赖它去重）；
  - `runOnce()` 送达后按域推进；`run()` 积压探测从"仅事件"扩为三域任一非空。
- `server/test/unit/test_edge_wireless_uplink_gtest.cpp`
  - TDD：先写 RED（新 API 编译失败），实现后转 GREEN；
  - 新增：游标三态（缺失/旧格式/单键损坏）逐域回退；`advanceCursors`
    域独立性（事故/基线更晚时间戳不推进事件游标）+ 不回退 + 空批不动；
    `collect` 非对称游标下各域互不越界（事件游标落后时仍取回区间内事件，
    即 C2 丢发场景的直接回归）。
- `docs/架构设计-模块与代码结构.md`：上行流程图 ③ 补充三域独立游标题注；
  头文件游标章节同步（并修正 state 文件名与 server.cpp 实际值一致）。

验证：RED→GREEN 后 x86 全量 `ctest --test-dir build-x86/server`
52/52 套件通过（uplink 套件 11 用例）。

## 结果

已完成。单水位跨域截断丢事实的路径被封死；state 文件格式向后兼容（旧
文件升级后事故/基线域全量重扫一次，云端幂等吸收）。遗留：同域内相同
时间戳并列超过批上限时的截断边界（如 2000+ 条基线同一毫秒）仍是既有
LIMIT 语义，本次未改——现实发生率极低且属独立议题，如需收紧应单独立项。

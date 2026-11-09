# V4L2 深度项补测 + 出队延迟字段接线

> 日期：2026-11-09
> 分类：工作流外实施（补测 + 孤儿字段修复）
> 结果：已完成（7 个文件，全量 ctest 60/60 通过）

## 构思

### 要解决什么问题

承接上一轮（`2026-11-09` 之前的 V4L2 深度实施），本轮要收两个尾巴：

**一、新增代码几乎没有测试守着**

`V4l2FrameSource` 的多平面路径、帧率协商、窗口语义、出队延迟统计全部是新增行为，
但测试文件只在文件头写了一句「真实出帧、格式协商、时间戳来源刻意**不测**」。
结果是：代码写了 462 行，行为却零回归护栏。更糟的是这句话本身已经不成立——
它把「可以做但没做」说成了「有意不做」，会让后来的人以为这是设计决定。

**二、`p95_dequeue_latency_ms` 是孤儿字段**

`server/include/frame_source.hpp` 里为它写了 21 行注释，`v4l2_frame_source.cpp`
里为它做了完整的采集（每次 DQBUF 后取时刻、算差、存百分位），但：

```bash
grep -rn "p95_dequeue_latency" server/ --include=*.cpp --include=*.hpp | grep -v test/
# 只有两处：定义、赋值。零消费者。
```

按项目铁律「生产接口、字段若无可批准的需求、真实消费者和可观测命令，即为孤儿：
删除它，或交回 Planner」。这条字段当时两条路都没走。

### 为什么不做进 OpenSpec change

1. 补测是对**已落地代码**的保护，不新增产品表面，无行为变更；
2. 孤儿字段修复需要接线——但接线方式是把值送进**已存在**的出口
   （`video.issues` → `exp.video_qoe_issues` → `legacy_adapter` JSON），
   不新增任何接口、字段或命令。

按 `docs/ai/工作流使用指南.md`「改 typo、改注释、补测试 → 不用工作流」，
以及项目 memory `no-new-openspec-change-by-default`。

### 备选方案与取舍（孤儿字段）

| 方案 | 取舍 |
| --- | --- |
| A. 删掉该字段与其采集代码 | ✅ 立刻消除孤儿。❌ 但会丢掉「区分采集端慢 / 用户态慢」这唯一的归因能力，而这正是本轮深度项的核心价值 |
| **B. 接进归因层，作为独立成因标签与证据条目（采用）** | ✅ 保住能力、消除孤儿、走已有出口不新增表面 |
| C. 只在 `describe()` 里打印 | ❌ 无机器可读消费者，等于换个地方当孤儿 |

选 B。

## 实现方式

### 一、V4L2 测试（`server/test/unit/test_v4l2_frame_source_gtest.cpp`，+12 个用例）

测试文件重构成两层，并在文件头如实写明：

- **第 1~5 节（无设备，CI 默认执行）**：失败路径 + 新配置项默认值 +
  `describe()` 可观测性。新增 5 条：
  `FrameRateNegotiationDefaultsToOn`、`MultiplanarIsAllowedByDefault`、
  `DescribeStatesPlanarModeEvenWhenStopped`、
  `DescribeOmitsNegotiationNoteWhenNothingAdjusted`、
  `WindowZeroIsNotSilentlyRewrittenLikeSynthetic`。
- **第 6 节（需真实设备，`WEAKNET_V4L2_TEST_DEVICE` 门控）**：成功路径。
  宿主机 `modprobe vivid` 即可跑，**无需真实摄像头**。新增 7 条，覆盖
  start 成功、窗口语义、窗口边界序号、帧率协商、出队延迟、stop 后失效、重启。

门控用环境变量而不是 `GTEST_SKIP`：静默 skip 在 CI 上表现为「测试通过」，
实际一次没跑——测试自己更不能靠 skip 伪装成覆盖。

### 二、出队延迟接线（`video_qoe_attribution.{hpp,cpp}` + 4 条测试）

1. `VideoQoeThresholds` 新增 `dequeue_latency_p95_bad_ms{200}`（3 个帧周期）；
2. `collectFrameIssues()` 新增 `DEQUEUE_LATENCY` 成因标签。它是文件里
   **唯一的阈值判定处**，`isFrameQualityBad()` 由它派生，不另写一份；
3. `evidence` 新增 `video_dequeue_latency_p95_ms`，detail 带
   `vs gap_p95=<值>` 作对照——两个值一起看才有归因意义。

由此形成完整消费链（全部是已有出口，零新增表面）：

```
FrameStats.p95_dequeue_latency_ms
  → VideoQoeAttributionResult.issues["DEQUEUE_LATENCY"]
  → exp.video_qoe_issues
  → legacy_adapter.hpp 序列化为 JSON
```

归因方向（与 `frame_source.hpp` 注释同源）：

| 现象 | 帧间隔 | 出队延迟 | 结论 |
| --- | --- | --- | --- |
| 只报 `FRAME_GAP_P95` | 高 | 低 | 采集端出帧慢（sensor / ISP）|
| 只报 `DEQUEUE_LATENCY` | 正常 | 高 | 用户态 DQBUF 循环被拖慢 |
| 两者都报 | 高 | 高 | `buffer_count` 太小 |

**一个刻意的边界**：判据写 `p95_dequeue_latency_ms > 0 && ... >= 阈值`，
而不是直接 `>=`。因为 `0` 是「未采样」的哨兵值（合成源从不设该字段），
用 `>=` 会在阈值配成 0 时把每个合成源都判成有延迟。

### 三、订正两处无法复现的注释

| 位置 | 原注释 | 实测 | 处理 |
| --- | --- | --- | --- |
| `v4l2_frame_source.hpp` 帧率协商 | 「vivid 默认 5fps，配置 30fps → loss 0.7」 | 当前环境 vivid 默认 30fps，无法复现 0.7 | 改成**可复现**的真实触发条件：驱动帧率**跨进程保留**，前一个进程设成 15fps 后，本进程得到 `exp=30 recv=15 loss=0.5` |
| 同上 | 「Capabilities 0x85231000」 | 当前 `0x05230001`（splane） | 该值来自上一轮 `multiplanar=2` 重载，属环境相关，不再在注释中固化具体值 |

机制本身是真实的（已用探针 `probe5_order` 证明跨实例污染），只是原文把触发条件
写错了。**注释里不可复现的数字比没有数字更糟**：后来的人照着复现不出来，
会怀疑整段代码。

### 验证手段

```bash
cmake --build build-x86 -j$(nproc)
ctest --test-dir build-x86/server --output-on-failure
WEAKNET_V4L2_TEST_DEVICE=/dev/video0 ./build-x86/server/test/test_v4l2_frame_source_gtest
```

## 结果

### 最终状态

| 检查 | 结果 |
| --- | --- |
| 全量 ctest | `100% tests passed, 0 tests failed out of 60`，8.43 s |
| V4L2 测试（含真机，vivid） | 22/22 通过（15 无设备 + 7 真机），10.6 s |
| 归因测试 | 30/30 通过（原 26 + 新 4） |
| 孤儿字段 | `p95_dequeue_latency_ms` 现有完整消费链至 JSON 出口 |

### 变异测试：证明新测试真的能抓错

补测最怕的是「写了但抓不到」。本轮做了三次变异，全部把代码改坏后确认测试失败：

| 变异 | 预期 | 实测 |
| --- | --- | --- |
| `resetWindow()` 删掉 `has_last_seq = false` | `WindowBoundaryDoesNotFakeSequenceGap` 失败 | ✅ 3/3 失败 |
| 窗口跨度改回「实际耗时」 | `ContinuousCallsMeasureFullWindowWithoutPhantomLoss` 失败 | ✅ 失败（`window_ms is 499, expected 500u`）|
| 删掉 `DEQUEUE_LATENCY` 判定 | 2 条归因测试失败 | ✅ 2 条失败，其余 28 条通过 |

### 一处必须如实说明的失败与修正

**第一版序号测试是无效的**。最初我把「累计 seq_gap 应为 0」放在**连续调用**
的用例里。变异验证显示：删掉 `has_last_seq = false` 后该用例 3/3 仍然通过——
因为连续调用时序号常常刚好连号，缺陷不可观测。

用探针扫参数后找到真正原因：窗口间 `sleep < window_ms` 时窗口尚未到期，
睡眠期间缓冲区填满、驱动**真的丢帧**，正常代码也会如实计数 gap（`sleep=200`
时正常版与变异版都得 2）。只有 `sleep >= window_ms`（走 overdue 非阻塞排空分支）
才把「真丢帧」与「跨窗口假比较」分开。

于是拆出独立用例 `WindowBoundaryDoesNotFakeSequenceGap`，用
`window=1000ms / sleep=1000ms / 3 窗口`（约 4 秒），实测正常版恒 0、变异版
稳定非 0。同时把 `ContinuousCalls` 里那条假护栏**删掉并写明为什么删**——
留一条会随运气漏判的断言，比没有断言更危险。

### 遗留问题

1. **`VideoQoeAttributionResult.evidence` 仍未被消费**。归因函数生成的 4 条
   evidence 目前只被构造、不被使用。observer 归因的同类问题上一轮已修
   （`server.cpp:635` 挂回 `exp.dns_service.evidence`），视频侧还没做。
   这是**行为变更**（会让新指标进 JSON 出口），且当前 `server.cpp:718` 是
   `weaknet::FrameStats frames;` 默认构造，`valid()==false`，归因恒 `NONE`，
   接了也看不到效果。**需要用户决策是否现在接**。
2. **视频归因整体仍未真正工作**：帧源未接进 `server.cpp`，前置条件是真机出帧。
3. **DMABUF 与 media controller 拓扑两项深度工作未开工**（grep 零命中）。

### 后续是否值得升格为 OpenSpec change

**补测与孤儿字段修复本身不需要**（无新增表面）。

**但「把 `V4l2FrameSource` 接进 `server.cpp`」需要**：它会让
`video_qoe_attribution` 从恒 `NONE` 变成可能非 `NONE`，是明确的行为变更，
且届时 `evidence` 接线、`net_obs.interval_ms` 取值都要一并定。前置条件仍是真机出帧。

## 相关

- `openspec/tasks/2026-11-09-v4l2-doc-sync-and-suite-count.md`（上一轮文档同步）
- `openspec/tasks/2026-11-08-v4l2-frame-source-and-overlay-playbook.md`（帧源落地）
- `驱动面能力引入与业务融合方案.md` §6.2.1（L2/L3/L4 分层）、§6.2.4（五象限归因）
- `docs/ai/testing.md`（RED-GREEN-REFACTOR、禁止削弱断言、临时程序不入库）

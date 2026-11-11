# 丢帧判据从「丢帧率」改为「绝对帧数」

> 日期：2026-11-11
> 分类：工作流外实施
> 结果：已完成（x86 全量 61/61 测试通过，未做板端验证——本变更与硬件无关）

## 构思

### 问题

`VideoQoeThresholds::frame_loss_ratio_bad = 0.05`（5%）是比例判据。比例的分母
是本窗口的 `frames_expected`，而这个分母在**不同设备上对应不同的帧数**：

| 帧率 | 丢 1 帧 | 丢 2 帧 | 丢 3 帧 |
| --- | --- | --- | --- |
| 30fps | 3.3% | 6.7% | 10.0% |
| **18fps（本板实测真实帧率）** | **5.6%** | **11.1%** | **16.7%** |

本板 IMX219 满分辨率模式真实只跑 18.56fps（PLL 决定，见
`openspec/pitfalls/sensor-mode-table-fps-lie.md`）。18fps 下**丢 1 帧就
5.6%，越过 5% 阈值**。

后果不是理论风险：任何一次 ISP 停顿、一次 page fault、一次调度抖动导致的
单帧丢失，都会被判成「画面差」，进而归因 `APP_LAYER_SUSPECT`。而 30fps
设备丢同样 1 帧完全不会触发。**同一份物理事实在不同板子上得到相反结论**。

### 为什么不做进 OpenSpec change

用户 2026-11-06 明确要求「以后只要我不让你新开 change，你就默认不用新开」
（memory: `no-new-openspec-change-by-default`）。本任务是单字段语义替换 +
测试补齐，按工作流外实施推进，**不具备 OpenSpec 验收效力**。

### 备选方案与取舍

| 方案 | 做法 | 取舍 |
| --- | --- | --- |
| A：保留比例，阈值降到 1% | 调数值 | ❌ 18fps 下丢 1 帧仍是 5.6%，1% 等于「丢 1 帧就报」，反而更敏感 |
| **B：改绝对帧数** | `frame_loss_frames_bad = 3` | ✅ **选中**。判据与帧率解耦，「丢 3 帧」在任何板子上都是「丢 3 帧」 |
| C：按基线做相对判定 | 用校准后的 fps 算偏差率 | ❌ 需要引入「基线丢帧率」状态，纯函数会被破坏；且 fps 校准落地后分母已正确，比例判据的根因大半已消除，剩余问题是**量化步长**而非分母 |

方案 B 的核心收益：**把它从「相对量」变成「绝对量」，判据含义不再依赖
任何其它参数的取值**。这在归因系统里比数值调优重要——判据含义不稳定，
事后复核证据时无法复现结论。

### 为什么默认是 3

- **1 帧太敏感**：单帧抖动在嵌入式采集链路上很常见，报出来只是噪音。
- **2 帧仍偏敏感**：18fps 下 2 帧是 11%，启动阶段仍易触发。
- **3 帧 = 18fps 窗口的 1/6（16.7%）**：覆盖「明显丢帧」，不被单次抖动误触发。
- **30fps 设备上 3 帧是 10%**，与原 5% 阈值（约 2 帧）量级接近，**没有
  把判据整体放松**。

## 实现方式

### 改动文件

| 文件 | 改动 |
| --- | --- |
| `server/include/video_qoe_attribution.hpp` | `double frame_loss_ratio_bad{0.05}` → `uint32_t frame_loss_frames_bad{3}`，附完整取舍理由（帧率对照表、默认值论证、可调性说明） |
| `server/src/video_qoe_attribution.cpp` | `collectFrameIssues()` 判据由 `frameLossRatio() >=` 改为 `framesLost() >=`，附理由注释 |
| `server/test/unit/test_video_qoe_attribution_gtest.cpp` | 替换 1 条旧测试为 3 条新测试；同步 3 处过时注释（「丢 50% > 5%」→「丢 15 帧 >= 阈值 3 帧」等） |
| `docs/hardware/radxa-cubie-a7a/sunxi-vin-摄像头链路根因分析.md` | §7.9.6 第 3 条补「2026-11-11 已在产品侧修复」更新块；保留「18.33fps 成因未定位」的结论 |
| `docs/产品定位-工业无线诊断网关.md` | 视频 QoE 行的「已知边界」由「须扣除 38% 基线」改为「已校准消除 + 残余 −3% 低估 + 判据改绝对帧数」 |

### 关键决策：为什么用 `framesLost()` 而不是 `frames_expected - frames_received`

`FrameStats::framesLost()` 已内含「负数钳到 0」的保护（`frame_source.hpp:126`）。
真机上 `frames_received` 可能略大于 `frames_expected`（1000ms 窗口在 18.56fps
下应到 18.56 帧，取整 18 后天然允许「多收一帧」，实测 18 vs 19）。直接做减法
会得到负数，与 `uint32_t` 比较时回绕成巨大正数，**必然误报**。用 `framesLost()`
是唯一安全的写法。

### 测试设计（3 条新测试）

1. **`FrameLossThresholdBoundaryIsAbsoluteFrameCount`** —— 边界语义。
   30 帧里丢 3 帧（= 阈值）判差；丢 2 帧不判差。钉住 `>=` 语义。

2. **`FrameLossThresholdIsFrameRateIndependent`** —— 本次变更的**核心理由**。
   18fps 丢 1 帧（5.6%）与 30fps 丢 1 帧（3.3%）必须给出**相同**结论，
   且两者都不得判差。再反向确认：丢满 3 帧时两者都判差。
   这条测试在旧的比例实现上**必然失败**（18fps 那个会判差）。

3. **`FrameLossFallsBackToRatioWhenWindowUnknown`** —— 退化路径。
   `frames_expected == 0` 时 `framesLost()` 恒为 0，于是永不判丢帧。
   这是**正确**的：没有分母就没有可信的丢帧事实。测试还验证
   「FRAME_LOSS 缺席不得连带吞掉帧间隔判据」（`p95_gap_ms = 500` 仍判差）。

### 验证手段

**RED → GREEN**：

```
# RED：头文件字段尚未改时
test_video_qoe_attribution_gtest.cpp:251:3: error: 'struct VideoQoeThresholds'
  has no member named 'frame_loss_frames_bad'; did you mean 'frame_loss_ratio_bad'?
  （3 处同类错误）

# GREEN：改完头文件与 .cpp 后
[==========] 32 tests from 1 test suite ran. (0 ms total)
[  PASSED  ] 32 tests.
```

**变异测试**（确认新测试真能捕获回退）：

把 `.cpp` 的判据改回 `frameLossRatio() >= 0.05`（即「悄悄退回比例语义」），
预期新测试必须失败：

```
[  FAILED  ] FrameLossThresholdBoundaryIsAbsoluteFrameCount
[  FAILED  ] FrameLossThresholdIsFrameRateIndependent
[  PASSED  ] 30 tests.
2 FAILED TESTS
```

两条新测试确实捕获了回退。第三条（退化路径）不失败是正确的——比例实现下
`frames_expected == 0` 时 `frameLossRatio()` 也返回 0，行为一致。

**全量回归**：

```
100% tests passed, 0 tests failed out of 61
Total Test time (real) = 8.76 sec
```

## 结果

### 已完成

- 判据由比例改为绝对帧数，`frame_loss_frames_bad` 默认 3 帧
- 3 条单测（含 1 条「帧率无关性」核心理由测试）
- 变异测试确认新测试有区分能力
- x86 全量 61/61 通过
- 文档同步 2 处（根因分析 §7.9.6、产品定位视频 QoE 行）

### 诚实边界

1. **未做板端验证**。本变更是纯判定逻辑改动，不含任何 V4L2 / 硬件交互，
   板上跑与宿主机跑结果必然相同。但**「默认 3 帧在真实场景是否合适」
   未经现场数据检验**——它是设计论证，不是实测结论。

2. **`frame_loss_frames_bad` 不随窗口时长缩放**。当前实现是「一个窗口内
   丢 3 帧算差」，窗口默认 1000ms。若某部署把窗口改成 200ms，3 帧在
   200ms 里的含义与 1000ms 完全不同（分别是 150% 与 30% 的丢帧率）。
   **这是绝对帧数判据的固有代价**：它换来了帧率无关性，代价是窗口时长
   敏感性。当前只有一个窗口配置（1000ms），尚无此问题；若将来引入可配
   窗口，需同步考虑把阈值也按窗口缩放，或明确要求「窗口固定」。

3. **不改变 `video_frame_loss_ratio` 证据条目**。evidence 里仍输出
   `frameLossRatio()` 原始值——证据必须是可复核的原始事实，不能因为判据
   换了就把比例藏起来。运维看到 `loss=3/30` 就知道发生了什么。

### 遗留

1. **接线仍未决策**。`V4l2FrameSource` 尚未接进 `server.cpp` 评估循环，
   生产态 `video_qoe_attribution` 仍恒为 `NONE`。本变更清理的是接线之后
   会立刻踩到的判据缺陷，属于前置准备。
2. **向下取整的 3% 低估不修**（用户 2026-11-11 决策：感觉可以不修）。
   方向是「少报丢帧」，符合本项目「少报优于多报」的纪律。

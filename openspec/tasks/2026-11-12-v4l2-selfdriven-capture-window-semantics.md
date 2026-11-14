# V4L2 自驱采集线程与窗口语义改造（决策 A2）

> 日期：2026-11-12
> 分类：工作流外实施
> 结果：已完成（生产接入点未合，见「结果」）

## 构思

### 要解决的问题

`V4l2FrameSource::snapshotAndReset()` 改造前是**调用方驱动的阻塞排空**：它内部
用 `select()` 排空队列，阻塞时长等于配置的 `window_ms`。这个设计有两个后果：

1. **窗口时长报的是配置不是事实。** `FrameStats.window_ms` 恒等于
   `cfg.window_ms`（默认 1000ms），而 `server.cpp` 的网络侧用 120s 滚动窗口。
   `attributeVideoQoe()` 对两侧窗口做**严格相等**判断，于是接入后必然恒定
   返回 `window_mismatch` 并**拒绝归因**。这不是配置没调对，是接口语义错位：
   帧源无法表达「我这份统计实际覆盖了多久」。
2. **采集节奏被取数节奏绑架。** 调用方多久取一次数，就决定了帧多久被排空一次。
   取数慢 → 队列填满 → 驱动丢帧 → 这个**观测缺陷**被记成**链路丢帧**。

### 为什么不做成 OpenSpec change

用户明确指示「现在开始做 v4l2 部分的工作，不走 change」。本改造是对已实现模块的
契约修正，无新增产品表面、无新增 entrypoint、无新增配置键，不满足 change 的门槛
（规划/实现分离的价值在于约束新表面，而这里改的是既有契约的表达方式）。
按 CLAUDE.md 要求在本目录留记录，但**不具备验收效力**。

### 备选方案与取舍

| 方案 | 做法 | 否决理由 |
| --- | --- | --- |
| A1 调用方对齐 | 把 `server.cpp` 改成每 1000ms 取一次帧 | 网络侧 120s 窗口是 SLE 的既有判定基础，为一个旁路归因改主链路节奏，代价与风险都不成比例 |
| **A2 自驱采集（采纳）** | 内部线程连续 DQBUF/QBUF，`snapshotAndReset()` 立即返回并如实报实际跨度 | 帧侧自报事实、调用方按同一节奏接入即可对齐；且顺带消除「取数慢 → 记成链路丢帧」的观测缺陷 |
| A3 放宽归因函数 | 让 `attributeVideoQoe()` 容忍窗口不等 | 方案 §6.2.4 明确要求拒绝而非近似对齐——错位窗口的四象限结论会反向。放宽等于拆掉纪律 |

选中 A2。

## 实现方式

### 改动的文件

| 文件 | 改动 |
| --- | --- |
| `server/src/v4l2_frame_source.cpp` | +794/−458：新增采集线程、锁、`calibrateTickLocked()`；`snapshotAndReset()` 由阻塞排空改为加锁拷贝 |
| `server/test/unit/test_v4l2_frame_source_gtest.cpp` | +825/−458：五个用例按新契约重写，新增分辨率环境变量、`parseEffectiveFps()` 及其守护用例 |
| `server/include/v4l2_frame_source.hpp` | +38/−？：`window_ms` 与类文档全面重写，写清「设计跨度」与「实际跨度」的区别 |
| `server/include/frame_source.hpp` | +36：`snapshot_interval_ms` 与 `observationGapMs()` 的语义说明 |
| `server/src/server.cpp` | +23：补上拒绝归因分支的 `reason` 记录 |
| `openspec/pitfalls/sensor-mode-table-fps-lie.md` | +8/−1：用例改名后的交叉引用订正 |

### 关键决策

1. **窗口语义（A2 的核心）**：`window_ms` 与 `snapshot_interval_ms` 都报
   **实际跨度**（`now − last_snapshot_time`，首份退化为 `now − window_start`），
   钳到 ≥1ms。于是 `observationGapMs()` 在自驱线程下恒为 0——这是**事实**，
   不是把盲区藏起来。

2. **跨快照状态必须连续（语义反转）**：改造前 `has_last_seq` /
   `has_last_frame_time` / `last_frame_time` 在每次快照时清零，因为每个窗口
   独立排空。自驱线程下**不能清**——清了会把跨快照边界的真实序号跳变藏掉。
   这是与改造前**完全相反**的要求，`resetWindow()` 的注释里写明了。

3. **校准子窗口与取数节奏解耦**：`tryCalibrateFps()` 需要 N 个等长窗口，
   其边界由采集节奏（`cfg.window_ms`）经 `calibrateTickLocked()` 推进，
   **不**由调用方快照推进。否则调用方改成 120s 取一次，校准样本就变成
   120s × 3，启动阶段的假丢帧会持续几分钟。`window_gaps_us` 由
   `tryCalibrateFps()` 自己清，不交给 `resetWindow()`。

4. **采集线程死亡必须显式化**：线程挂了则累计值冻结而 `frames_expected`
   继续按时间增长 → 会凭空造出约 97% 的假丢帧。`thread_fatal` 置位后
   `snapshotAndReset()` 返回 `valid()==false`（上层报 unavailable），
   `describe()` 追加 `(capture-thread-dead)`。

5. **锁边界**：`mutable std::mutex mtx` 只保护累计值；QBUF 在**锁外**做，
   避免 ioctl 期间阻塞快照。停止用 `std::atomic<bool> thread_should_stop`。

6. **生产循环的拒绝原因**：`server.cpp` 原来只有 `if (video.evaluated)`，
   拒绝归因的五条路径（`frames_unavailable` / `window_mismatch` /
   `window_length_undeclared` / `network_unavailable` / `request_mismatch`）
   全部静默丢弃，快照里只剩裸 `NONE`。补 `else` 分支写
   `video_qoe_attribution_detail` 并打日志；**不写 `issues`**（契约要求
   拒绝归因为空）、**不进 `warnings`**（没摄像头是预期形态）。

### 验证手段

- 宿主机 vivid：`WEAKNET_V4L2_TEST_DEVICE=/dev/video0` → **33/33 通过**
- 板端 IMX219：`WEAKNET_V4L2_TEST_DEVICE=/dev/video0 WEAKNET_V4L2_TEST_WIDTH=1640
  WEAKNET_V4L2_TEST_HEIGHT=1232` → **31 通过 / 2 跳过 / 0 失败**
- 全量单测：`ctest --test-dir build-x86/server` → **61/61 通过**
- ARM64 交叉编译：`cmake --build build-arm64-native` → 干净
- **变异测试**（关键，证明用例真的能抓）：对 `/tmp/v4l2_impl_backup.cpp` 做
  字符串手术植入四个变异体，逐一确认被**指定断言**抓住后还原：

  | 变异体 | 内容 | 被抓住的用例 |
  | --- | --- | --- |
  | S | `window_ms` 回填配置值 | `SnapshotSpanIsMeasuredFactNotConfiguredValue` |
  | B | `snapshotAndReset()` 退回阻塞排空 | `SnapshotCostIsIndependentOfWindowMsAndDoesNotPerturbCapture`（实测 42010ms） |
  | 1 | 校准改为每窗口重算 | `FpsCalibrationDoesNotAbsorbRealDegradation` |
  | D | `expected = received` | 同上（`loss=0`） |

- **板端探针**（交叉编译的独立程序，直接证据）：`span=1201 expected=36
  recv=19 bytes=57864576 corrupt=0 seggap=0 maxgap=54`，证明自驱线程在真实
  IMX219 上以 18.56fps 连续出帧、无序号跳变。

## 结果

### 已达成的

- 120s vs 1000ms 的窗口阻塞**在结构上消除**：帧侧如实报跨度，调用方按同一
  节奏取数即可对齐。
- 测试用例已按新契约重写，并用变异测试证明它们确实区分「吸收/如实」
  「配置/事实」「阻塞/非阻塞」。
- 板端所有失败都被定性为**设备能力限制**，不是实现缺陷，且都用
  `GTEST_SKIP()` 显式声明而非削弱断言：
  - `StartSelectsInputZeroBeforeFormatting`：板端只有一个输入
  - `FpsCalibrationDoesNotAbsorbRealDegradation`：sunxi-vin 拒绝流运行中 S_PARM

### 遗留（重要）

**生产接入点未合。** `server.cpp` 目前仍然构造默认（`valid()==false`）的
`FrameStats` 并把 `net_obs.interval_ms` 设成 0，因此归因**恒走
`frames_unavailable` 分支**并返回 `NONE`。这是刻意的：接入需要决定帧源
实例放在哪个生命周期、120s 与 15s 两个节奏怎么对齐，属于设计决策而非
机械改动。所以现状的准确表述是：

> **采集链路与归因逻辑都完整，测试在真机上通过；生产循环仍留着接入点没合。**

不得声称「线上在跑视频 QoE 归因」。

### 后续

- 接入 `server.cpp`（需要先定帧源生命周期与两侧节奏对齐策略）——
  这一步建议**升格为正式 OpenSpec change**，因为它新增了生产表面
  （帧源实例、配置键、失败上报路径），满足 change 的门槛。
- 板端 1640x1232 的 18.56fps 上限成因未定位（MIPI / ISP / sensor 内部节拍），
  不影响可用性但影响丢帧率绝对值的解释，见根因分析文档 §7.9.7。
- 校准精度已知边界：18.56 向下取整为 18，约 −3% 系统性低估，方向是
  「少报丢帧」，符合本项目纪律。

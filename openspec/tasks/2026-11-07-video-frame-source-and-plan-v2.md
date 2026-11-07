# 视频帧源抽象落地 + 驱动面方案 v2 定稿

> 日期：2026-11-07
> 分类：工作流外实施 + 文档迭代
> 结果：已完成（代码 48 项单测全绿：帧源 22 + 归因 26；文档三处结构性订正已落地；
> 硬件阻塞项全部消除；已提交 `1b48564` + `75e7901`）

## 构思

### 背景：三件事挤在同一天

这一轮的起点是三个彼此独立、但最后汇聚成同一条结论的问题：

1. **摄像头硬件阻塞**：早先判断「31pin 转 15pin 的 FPC 要自己找两份 pinout 定制」，
   还要处理「标准 15pin CSI 排线没有 MCLK 脚」的风险。
2. **显示屏选型**：用户问「官方屏符不符合要求、能不能触摸、是不是接个电脑显示器就行」，
   随后明确「官方屏太贵了」，转而考虑 HDMI + 便宜 USB 触控板。
3. **视频归因的接口形态**：`驱动面能力引入与业务融合方案.md` §6.2.5/§6.2.6 原计划
   新增 4 个 `MetricId` + 第 21/22 个插件 + `weaknet-cli stats video` 域。

三个问题最后指向同一个动作：**把结论写进文档，并把第一条代码落地**。

### 为什么不做进 OpenSpec change

用户 2026-11-06 明确要求「以后只要我不让你新开 change，你就默认不用新开」
（memory: `no-new-openspec-change-by-default`）。本任务按工作流外实施推进，
**不具备 OpenSpec 验收效力**。

另外从铁律角度看也不适合套 change：文档迭代不产生行为变更；`FrameStats` /
`IFrameSource` / `SyntheticFrameSource` 是纯新增、默认态下与引入前逐字节等价
（没有任何现有代码 include 它），不改变任何既有行为。

### 关键取舍一：视频归因不再新增 MetricId / 插件

原 §6.2.5 计划新增 `VIDEO_FRAME_LOSS_RATIO` / `VIDEO_FRAME_GAP_P95_MS` /
`VIDEO_FRAME_CORRUPT_RATIO` / `VIDEO_BITRATE_BPS` 四个指标，§6.2.6 计划第 21/22 个
插件线程 + `stats video` D-Bus 域。

**否决依据不是偏好，而是 thermal 已经用一次真实落地把这条路证否了**
（`openspec/tasks/2026-11-06-thermal-monitor.md` §「被否决的方案」）：

| 理由 | 具体代价 |
| --- | --- |
| `MetricId` 是封闭枚举 | 改枚举 + `metricIdToString()` + `getMetricDescriptor()` 三处；落库还要再改 `ensureSchema()` 建列 / `insertSnapshot()` 绑序号 / `queryHistory()` 的 SELECT 三处，共六处 |
| 数据只在当轮评估用一次 | 视频帧统计不进窗口、不做趋势、不参与诊断谓词，没有第二个读者 |
| 已有出口够用 | `observer_attribution` / `thermal_attribution` 都从 `weaknet-cli experience` 出去，视频归因同构 |

因此改为与 thermal 逐字同构的三字段形态（`video_qoe_attribution` /
`_detail` / `video_qoe_issues`），零 `MetricId`、零插件、零新 D-Bus 方法、
零表结构迁移。

### 关键取舍二：显示路线从「买 DSI 屏补图形栈」改为「HDMI 优先」

上一版文档 §7.1 写「项目目前完全没有图形栈」。板端实测证明这句话**不成立**：

```
sddm.service           active (running) 3h 20min
/usr/bin/X             -nolisten tcp ... -seat seat0 vt7
/dev/dri/{card0,card1,renderD128}   存在
de@5000000 / tcon3@5730000 / tcon4@5731000   okay
card0 的 driver        → sunxi-drm
card0-HDMI-A-1         disconnected   ← 唯一缺的是一台显示器
```

`qt_dashboard/` 也已完成 39/39 数据层测试 + 8/8 UI 像素测试 + ARM64 交叉编译 +
板端运行 + VNC 外部抓帧 + 15 分钟长稳（RSS 平稳 60300 kB），唯一遗留 ⏳ 是
「HDMI 实机显示（显示器未在位）」。

所以 DSI 屏「补图形栈空白」的理由消失，它现在唯一独有价值是「MIPI DSI 总线经验」。
再加上「官方屏太贵」和「触摸暂无消费者」（*2026-11-07 晚补记：该理由已由产品定位 v1.2 解除，
但「不买 DSI 屏」的结论不变——触摸应走 USB 触控板，价格差 10 倍*），结论是**不买**。

### 关键取舍三：USB 触屏「必须重编内核」是错的（我自己上一轮的误判，此处订正）

网上流传的说法是「USB 触摸屏免驱、内核自带」——**在本板不成立**：

```
# CONFIG_HID_MULTITOUCH is not set
# CONFIG_TOUCHSCREEN_USB_COMPOSITE is not set
# CONFIG_TOUCHSCREEN_EGALAX is not set
# CONFIG_TOUCHSCREEN_ELAN is not set
# CONFIG_HID_ELAN is not set
```

`modprobe hid_multitouch` / `modprobe usbtouchscreen` 均 `FATAL: not found`。
插上去只能被识别成最基础的单点 HID 鼠标（`CONFIG_USB_HID=y` 是开的）。

**但我上一轮据此得出的「必须重编内核」同样是错的**，实测证否：

```bash
$ make -C /lib/modules/5.15.147-99btf1-a733/build M=/tmp/kmodtest modules
  CC [M]  /tmp/kmodtest/t.o
  LD [M]  /tmp/kmodtest/t.ko
$ modinfo t.ko
vermagic: 5.15.147-99btf1-a733 SMP preempt mod_unload aarch64   ← 与 uname -r 逐字一致
$ sudo insmod t.ko   → rc=0
$ lsmod | grep "^t "
t    16384    0
$ sudo rmmod t       → rc=0
```

四项前提全部核实：

| 前提 | 结果 |
| --- | --- |
| vermagic 匹配 | 板端有完整 `/lib/modules/$(uname -r)/build` |
| 无签名强制 | `# CONFIG_MODULE_SIG is not set` |
| 符号全导出 | `Module.symvers`（911398 B）含 37 个 `hid_*` + 9 个 `input_mt_*`，包括 `__hid_register_driver` / `hid_unregister_driver` / `hid_hw_start` / `hid_hw_stop` / `usb_register_driver` / `usb_deregister` / `input_mt_init_slots` / `input_mt_report_slot_state` |
| 工具链 | gcc 10.2.1-6 / make 4.3 / ld 2.35.2 / bc 1.07.1。**不需要 bison/flex** |

所以三条「重编内核才有的风险」（eBPF `vmlinux.h` / kmod `vermagic` / BSP
`sunxi-drm`）在本方案下**全部不成立**，因为它们的前提都是「替换内核」。

### 关键取舍四：摄像头硬件阻塞全部消除

| 原风险 | 现状 |
| --- | --- |
| FPC 要自己定制（找不到 pinout） | **消除**。官方文档明确给出 `31-Pin 0.3mm pitch → 15-Pin 1.0mm pitch (same side)`，且 **Cubie A7A 在官方支持板卡清单内** |
| 标准 15pin CSI 排线无 MCLK 脚 | **消除**。Radxa 把 A7A 列进支持清单，说明 8M 219 模块自供时钟。这是「必须买正品」的核心理由 |
| 采购风险 | **消除**。正品 8M 219 + 转接线 + USB-UART 救砖线全部到位 |

## 实现方式

### 新增代码

| 文件 | 行数 | 内容 |
| --- | --- | --- |
| `server/include/frame_source.hpp` | 214 | `FrameStats`（纯数据，含 `valid()` / `framesLost()` / `frameLossRatio()`）、`IFrameSource`（5 方法抽象）、`SyntheticPattern`（HEALTHY / PERIODIC_LOSS / BURST_LOSS / REORDERED）、`SyntheticFrameSource`（pimpl） |
| `server/src/synthetic_frame_source.cpp` | 204 | 时间驱动实现，不 sleep |
| `server/test/unit/test_frame_source_gtest.cpp` | 386 | 22 项测试（3 套件） |
| `server/include/video_qoe_attribution.hpp` | 206 | 枚举 6 值（5 象限 + `NONE`）、`videoQoeAttributionToString()`、`VideoNetworkObservation`、`VideoQoeThresholds`、`VideoQoeAttributionResult`（含 `issues`）、判定纯函数 |
| `server/src/video_qoe_attribution.cpp` | 221 | 判定实现 + `collectFrameIssues()` 唯一阈值判定点 |
| `server/test/unit/test_video_qoe_attribution_gtest.cpp` | 414 | 26 项测试 |
| `server/CMakeLists.txt` | +2 | `src/synthetic_frame_source.cpp` + `src/video_qoe_attribution.cpp` 登记 |
| `server/test/CMakeLists.txt` | +12 | 两套 `add_executable` + `target_link_libraries` + `foreach` 列表 |

设计要点：

1. **`FrameStats` 不含任何结论字段**——没有 `healthy` / `degraded` 这类布尔。
   丢帧率与间隔只是原始事实，成因判定归 `VideoQoeAttribution`。
2. **时间语义用 `steady_clock`**，不用 wall clock。工业现场 NTP 跳变会把
   墙钟差值直接变成几十年的假间隔。
3. **`valid()==false` 时上层必须报 unavailable**，不能把全零当成「零丢帧」。
   这是 `host_pressure_monitor.cpp:266` 已有的「不可用不得发布 0」纪律。
4. **`snapshotAndReset()` 是「取走并清零」语义**，不是「读取当前值」。
   避免多次调用拿到重复数据导致丢帧率被重复计算。
5. **合成源不 sleep**——把「时间」当纯函数参数，同一组参数永远得到同一份
   `FrameStats`。CI 上结果确定，单测 0 ms 跑完。
6. **`REORDERED` 模式单独存在**，因为「乱序不是丢帧」。若实现把乱序计入
   `seq_gap`，归因层会把「驱动行为」误判成「网络故障」。

### 验证

```bash
cmake --build build-x86 -j$(nproc)
./build-x86/server/test/test_frame_source_gtest
./build-x86/server/test/test_video_qoe_attribution_gtest
ctest --test-dir build-x86/server --output-on-failure
```

结果：

```
[==========] 22 tests from 3 test suites ran. (0 ms total)
[  PASSED  ] 22 tests.

[==========] 26 tests from 1 test suite ran. (0 ms total)
[  PASSED  ] 26 tests.

100% tests passed, 0 tests failed out of 59, Total Test time 8.66 sec
```

22 项 = `FrameStats` 契约（含 `valid` / `framesLost` / `frameLossRatio` 边界）
+ `IFrameSource` 多态可用性 + `SyntheticFrameSource` 四种模式 × 各窗口推进。

26 项 = 五行真值表 + 三条拒绝归因前置检查 + 证据条目 + 阈值语义
+ 纯函数性质 + `issues` 清单（5 项）。

### 文档改动

`驱动面能力引入与业务融合方案.md`：**+627 / -112 行**（1217 → 1609 行）。

| 章节 | 改动 |
| --- | --- |
| 文档头 | v1 → v2 定稿，列出三处结构性订正 |
| §0 类别表 | D 类从「MIPI DSI 屏 + 触摸」改为「HDMI 输出 + USB 触控面板（可选）」 |
| §3.1 / §3.2 | ASCII 图改 HDMI 优先；总表第 3/4 行的消费者与出口同步 |
| §4.3.3 / §4.3.4 | 加 ⚠️「已被否决，仅存档」头注，保留作为反面论证 |
| §6.2.1 | 分层图按实际落地状态标注（L2/L3/L4 全 ✅，仅 `V4l2FrameSource` ⬜） |
| §6.2.4 | 标题「四象限」→「五象限」（表里 5 个非 NONE 象限，原表述错误） |
| §6.2.5 | **整节重写**：数据出口进 `NetworkExperience`，附「零新增接口清单」与诚实代价 |
| §6.2.6 | 消费者表逐项标注 ✅/⬜/❌，删掉 `stats video` 命令 |
| §6.2.7 | 风险表：FPC 不匹配从「高」改为「已消除」；新增「无 MCLK」行标已消除 |
| §7 | **整节重写**：三条路线对比表 + HDMI 优先 + 两块官方屏实测差异 + USB 触控外部模块证据 + 消费者问题 |
| §9.1 / §9.2 | 任务 3 标完成、任务 4 是下一步、任务 5 标取消；新增 HDMI（¥0）与触摸（等定位）两行 |
| §10.1 / §10.2 | 采购状态改「全部到位」；DSI 屏标「不买」；新增 HDMI 线与 USB 触控板两行 |
| §10.2.1 | 三者对比表（OLED / DSI / USB 触控板），订正「补图形栈空白」的说法 |
| §10.2.3 | **新增**：USB 触屏三种实现与真实代价，含外部模块四项前提与「三条风险不成立」 |
| §10.5 | 采购顺序重写（含「不做官方 DSI 屏」） |
| §11.1 | 实施顺序：阶段 0/1 标完成、新增阶段 1.5（插 HDMI）、阶段 5 从「DRM/KMS 看板」改为「触摸接线（等定位）」 |
| §11.2 | 风险表：两条 `MetricId`/插件风险标「已规避」 |
| §12 | 新增不变量「归因结果走 `NetworkExperience`，不开新接口」 |
| §14 | 速查表 12 条全部重写 |
| §15 | **新增「当前方案一页纸」**：已做完 / 接下来做什么 / 明确不做的 / 一条贯穿纪律 |

## 结果

### 已完成

- `FrameStats` + `IFrameSource` + `SyntheticFrameSource` 落地，22 项单测全绿
- `VideoQoeAttribution` 纯函数落地，26 项单测全绿（**合计 48 项**）
- 视频归因接进 `NetworkExperience`（`1b48564` 三字段 + `75e7901` 补 `issues` 唯一生产赋值点）
- 方案文档 v2 定稿，三处结构性订正全部写入（归因出口 / 摄像头阻塞消除 / 显示路线改向）
- 交叉引用校验：全部 `§x.y` 引用均指向存在章节
- 表格列数校验：0 处异常
- 关键数字一致性校验：22 项 / 26 项 / 39/39 / 8/8 / GT911@0x14 / GT9271@0x5d 全文一致

### 未做（有意为之）

- **`hid-multitouch` 外部模块未编**。技术上已验证可行（空模块能编译加载），
  但触摸当前无真实消费者，用户明确表示后续会修改产品定位，改完再接入。
  （补记：后由并行会话以 `e45f28f` 落地，与本任务的视频线无关；
  同日稍晚产品定位修订为 v1.2，消费者已定义，剩余阻塞为 `qt_dashboard` 交互点。）
- **HDMI 未实机验证**。零成本但需要一台显示器在位。
- **视频结论不写进诊断 `excluded_causes`**（2026-11-07 定）。理由三条：
  ① thermal 同样未接入，单独给 video 接会让三者形态不一致；
  ② 生产态 `attribution` 恒为 `NONE`，写规则也永远不触发、无法验证；
  ③ 顺序不对——应先等真帧，再与 thermal 一并评估。详见方案 §6.2.6 / §15.3。

### 诚实边界

1. **`SyntheticFrameSource` 是唯一可用实现**。`V4l2FrameSource` 要等阶段 2
   overlay 演练完成才能写（摄像头硬件已到货，阻塞点是 overlay 未启用）。
2. **没有任何真实帧统计跑过**。22 项测试验证的是合成源，不是真机帧；
   26 项归因测试的输入也全部是手工构造的 `FrameStats`。
   生产态 `video_qoe_attribution` 恒为 `NONE`，因为 `server.cpp` 里 `frames`
   是默认构造（`valid()==false`）、`net_obs.interval_ms=0`，必然返回
   `frames_unavailable`。这是**正确**的：不把合成假数据灌进生产快照。
3. **文档里所有「实测」都来自板端命令回读**，但 `hid-multitouch` 只验证了
   「空模块能加载」，没验证「真实的 hid-multitouch 能驱动某块具体触控板」。
   后者需要真硬件在手。（`e45f28f` 已把模块编进来了，但这条边界仍未关闭。）
4. **两条我说错又订正的结论**已明确写入文档，避免以讹传讹：
   - 「USB 触屏免驱、内核自带」→ 假（`CONFIG_HID_MULTITOUCH` 未开）
   - 「USB 触屏必须重编内核」→ 假（外部模块即可）
   - 「项目完全没有图形栈」→ 假（sddm + Xorg + sunxi-drm 在跑）
   - 「8HD 屏没有触摸」→ 假（GT911@0x14）

### 后续是否值得升格为 OpenSpec change

**不建议。** `FrameStats` / `IFrameSource` / `SyntheticFrameSource` / `VideoQoeAttribution`
是纯新增，默认态下与引入前逐字节等价（无现有代码 include 它们），不满足「行为变更」前提。
若要升格需先 `git revert` 再走「freeze → 改代码 → evidence」流程，成本远高于收益。

**注意下一步不同**：把视频归因接进 `NetworkExperience` 会真正改变
`GetNetworkExperience` 的输出（多三个字段），那一步**才**是行为变更。
但它仍不建议开 change，理由与 thermal 完全一致——新字段默认 `NONE`，
不参与 `OverallPolicy`，序列化只是把已算出的结论送出去。
thermal 走同一路线已完成并真机验证（`2026-11-06-thermal-monitor.md`），
视频没有理由另起一套。

### 待办（按文档 §15.2 顺序）

1. ~~把视频归因接进 `NetworkExperience`~~ **✅ 2026-11-07 完成**（`1b48564` 三字段接线，
   本轮补 `issues` 唯一生产赋值点）。
   注意：生产态仍恒为 `NONE`——`frames` 是默认构造的 `FrameStats`（`valid()==false`）、
   `net_obs.interval_ms=0`，必然走 `frames_unavailable` 返回。
   这是**正确**的：不把合成假数据灌进生产快照。要出真结论必须先有帧源（待办 4）。
2. 插 HDMI 显示器（¥0，关闭 `qt_dashboard` 最后一个 ⏳）
3. 阶段 2 overlay 演练（第一个有变砖风险的动作，串口线已备）
4. 摄像头 overlay 启用 + `V4l2FrameSource`
5. SSD1306 OLED（可选，¥20）
6. 触摸接线 —— **定位已改完（v1.2），阻塞项转为 `qt_dashboard` 交互点**。
   见 `docs/产品定位-工业无线诊断网关.md` 第三节之二、
   `驱动面能力引入与业务融合方案.md` §7.6、`kmod/hid_multitouch/README.md`「消费者状态」。

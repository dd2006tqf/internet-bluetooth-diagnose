# V4l2FrameSource 落地 + 阶段 2 演练手册 + 采购状态订正

> 日期：2026-11-08
> 分类：工作流外实施 + 文档迭代
> 结果：已完成（`V4l2FrameSource` 10 项单测全绿，全量 ctest 60/60；演练手册成文；文档六处采购状态订正）

## 构思

### 背景：摄像头到货了，但下一步不是「直接接摄像头」

用户 2026-11-08 告知摄像头已到。直觉反应是「那就写 `V4l2FrameSource` 接上去」，
但方案 §11.1 的顺序不支持这么做：

```
阶段 2（硬件已备） overlay 演练：先启用一个无副作用的 overlay，验证 rescue 可用
      ↓
阶段 3（摄像头已购） V4l2FrameSource 真机  ──→  /dev/video0 出帧
```

**摄像头 overlay 本身就是那个有变砖风险的动作**（§11.2 风险表第一条）。
若直接上摄像头 overlay 而板子起不来，就无法区分是「overlay 机制不可用」还是
「摄像头那份 dtbo 有问题」——两个不同量级的故障共用一个症状。

所以这一轮拆成三件事并行推进，全部不依赖板子在线：

1. 写 `V4l2FrameSource`（代码层，接口已定，可实现）
2. 写阶段 2 演练手册（把「有变砖风险」变成「照着做」）
3. 订正文档里的采购状态（多处写着「全部到位」，与事实不符）

### 为什么不做进 OpenSpec change

用户 2026-11-06 明确要求「以后只要我不让你新开 change，你就默认不用新开」
（memory: `no-new-openspec-change-by-default`）。本任务按工作流外实施推进，
**不具备 OpenSpec 验收效力**。

`V4l2FrameSource` 是纯新增实现——`IFrameSource` 接口与 `FrameStats` 契约已在
阶段 1 定死并测透，新实现不改变任何既有行为，默认态下与引入前逐字节等价
（没有现有代码 include 它，`server.cpp` 仍用默认构造的空 `FrameStats`）。

### 关键取舍一：V4L2 细节全部藏在 pimpl 后

`linux/videodev2.h` 是 5000+ 行的内核头，且版本间有差异（宿主机与 ARM64
sysroot 的版本不一定一致）。若把它 include 进公共头：

- 任何包含 `v4l2_frame_source.hpp` 的编译单元都被拖慢；
- 版本差异会直接变成编译错误，且错误信息指向内核头而非本项目代码。

所以公共头只暴露构造参数、`IFrameSource` 五个方法、`lastError()` / `isRunning()`。
所有 `ioctl` / `mmap` / `struct v4l2_*` 都在 .cpp 里。

### 关键取舍二：构造函数不碰设备

这是 CI 可测性的前提。若在构造函数里 `open("/dev/video0")`，那么任何包含本类的
测试在没有摄像头的机器上都会在构造阶段失败——等于把「无硬件」变成「不能编译通过
测试」。改为 `start()` 才打开，于是：

- CI 上可以构造对象、验证 `start()` 返回 false、验证失败原因可读；
- 上层可以先建对象、按需启动，不必在装配阶段就决定是否启用视觉能力。

### 关键取舍三：默认 720p，不是 sensor 满分辨率

8M 219 是 3280×2464。用满分辨率会让 ISP / 内存带宽成为瓶颈，把「ISP 跟不上」
与「网络丢了帧」混在一起，直接污染归因。720p@30 已是 240 Mbps 量级，足够在
弱网下制造可观测丢帧，且留出充足余量。

这条写进了单测（`DefaultConfigMatchesPlan`），防止有人"顺手"改回满分辨率。

### 关键取舍四：演练手册把「串口线」列为 Step 0 硬门槛

方案 §10 把 USB-UART 串口线称作「阶段 2 的变砖保险」。但这个保险**从未在真机上
验证过能连上**——线买了、在手里，但没插到板子上读过一次启动日志。

这是一处纸面保险。若跳过这步直接动 overlay，一旦板子起不来而串口也没通，
就退回到「拔 eMMC/SD 卡挂到另一台机器上改」的境地，风险等级完全不同。

所以手册把「串口线验证」列为 Step 0，不通过就不进入 Step 1。

### 关键取舍五：手册里必须包含回滚演练

只验证「启用成功」而不验证「能否撤回」，等于保险从未被检验。所以 Step 4 是在
一切正常时主动回滚一次，确认 `mv` + `u-boot-update` + 重启这条路径确实可逆。

## 实现方式

### 新增代码

| 文件 | 行数 | 内容 |
| --- | --- | --- |
| `server/include/v4l2_frame_source.hpp` | 133 | `V4l2CaptureConfig`（6 项参数，默认值附方案依据）+ `V4l2FrameSource`（pimpl、禁拷贝） |
| `server/src/v4l2_frame_source.cpp` | 436 | `open` → `QUERYCAP` → `S_FMT` → `REQBUFS` → `mmap`+`QBUF` → `STREAMON`；`snapshotAndReset` 用 `select()` + 内核时间戳 |
| `server/test/unit/test_v4l2_frame_source_gtest.cpp` | 183 | 10 项测试（构造不碰设备 / start 失败语义 / 缺数据不伪装健康 / 来源标识 / 默认配置） |
| `server/CMakeLists.txt` | +1 | 登记 `src/v4l2_frame_source.cpp` |
| `server/test/CMakeLists.txt` | +8 | `add_executable` + `target_link_libraries` + `foreach` 列表 |

设计要点：

1. **`frames_expected` 由真实流逝时间推导**，不用 `frames_received` 回填。
   回填会让丢帧率恒为 0——摄像头掉线后仍报「画面完美」。
2. **帧间隔用 `v4l2_buffer.timestamp`**（内核单调时钟），不用用户态 `now()`。
   调度延迟会让「DQBUF 返回时刻」与「帧真正完成时刻」之间插入任意抖动。
3. **`seq_gap_count` 按事件计不按帧计**：一次跳 30 与三十次各跳 1，总量相同
   但 QoE 天差地别。序号回绕/重排不记为 gap（那是驱动行为，不是丢帧）。
4. **不完整帧用比例判定**（`bytesused / length < 0.5`），不用绝对值，
   避免分辨率变化后阈值失效。
5. **`describe()` 带 `v4l2:` 前缀**，与合成源的 `synthetic:` 对应。这是
   evidence 纪律：事后必须能区分「真的观测到了」与「注入了测试数据」。

### 验证

```bash
cmake -B build-x86 -DCMAKE_BUILD_TYPE=Debug -DBUILD_EBPF=OFF
cmake --build build-x86 -j$(nproc)
./build-x86/server/test/test_v4l2_frame_source_gtest
ctest --test-dir build-x86/server --output-on-failure
```

结果：

```
[==========] 10 tests from 1 test suite ran. (0 ms total)
[  PASSED  ] 10 tests.

100% tests passed, 0 tests failed out of 60
Total Test time (real) =   8.45 sec
```

套件数 59 → 60。

### 新增文档

`openspec/tasks/2026-11-08-stage2-overlay-drill-playbook.md`（278 行）：

| Step | 内容 | 硬门槛 |
| --- | --- | --- |
| 0 | 验证串口线能连 | ✅ **不通过则停止** |
| 1 | 基线快照（extlinux.conf / managed.list / dt 节点 / tty 设备） | — |
| 2 | 启用 uart2 overlay + `u-boot-update` | — |
| 3 | 重启 + 三条判据（SSH 通 / 新节点 / tty 增多） | ✅ |
| 4 | **回滚演练**（主动撤回，验证可逆） | ✅ |
| 5 | 恢复现场 | — |
| 6 | 再上摄像头 overlay | — |

首测选 `sun60iw2p1-uart2.dtbo`：只增加一个串口控制器节点，不触碰存储、显示、
网络、电源管理。最坏结果是「多了个不工作的串口」，而不是「起不来」。

### 文档订正（6 处）

`驱动面能力引入与业务融合方案.md`：

| 位置 | 原文 | 改为 |
| --- | --- | --- |
| §6.2.2.1 购买清单 | 两者均已备齐（2026-11-07） | 摄像头与转接线已到货（2026-11-08）；串口线未验证 |
| §6.2.7 风险表 | 线已备齐 | 已到货，接线前仍要核对三项 |
| §8 任务 8 | 摄像头已购、转接线已备齐 | 已到货 + `V4l2FrameSource` 代码已落地 |
| §10 头注 | 全部到位，硬件无阻塞 | 摄像头到货、屏未到、串口未验证 |
| §10.1 表格三行 | 已备齐 / 已购 | 已到货 / 在手但未验证 |
| §15.2 / §15.4 速查 | 全部到位 | 摄像头到货，屏未到，串口未验证 |

另外两处同步：

- `server/include/frame_source.hpp` 注释「待摄像头到货」→「见 v4l2_frame_source.hpp」
- §11.1 阶段图：阶段 1 增加 `V4l2FrameSource`，阶段 1.5 改「屏未到」，阶段 2 指向手册

## 结果

### 已完成

- `V4l2FrameSource` 落地，10 项单测全绿，全量 ctest 60/60（8.45 s）
- 阶段 2 overlay 演练手册成文（278 行，7 个 Step，Step 0 是硬门槛）
- 文档六处采购状态订正，另两处注释同步
- `frame_source.hpp` 的分层图注释与实际实现对齐

### 未做（有意为之）

- **V4L2 真机路径未验证**。10 项测试只覆盖失败路径与配置语义。真实出帧、
  格式协商（NV12 是否被 ISP 接受）、时间戳来源，都需要摄像头在板子上跑。
- **阶段 2 演练未执行**。板子当前不在线（`ssh board` 超时，隧道端口仍在监听，
  说明板端 sshd 未响应）。且 Step 0 需要人物理接线，无法远程完成。
- **串口线仍未验证**。这是当前最值得警惕的一处纸面保险。
- **HDMI 阶段 1.5 仍 ⬜**。显示屏未到（用户 2026-11-08 确认）。
- **USB 触控板不买**。用户 2026-11-08 明确「先不买了」。

### 诚实边界

1. **`V4l2FrameSource` 是代码完成，不是能力完成**。它还没有在任何真实设备上
   出过一帧。生产态仍走 `SyntheticFrameSource`（实际上 `server.cpp` 用的还是
   默认构造的空 `FrameStats`，归因恒为 `NONE`）。
2. **`snapshotAndReset()` 的取帧循环逻辑未经真机检验**。特别是 `select()` 超时
   与 `VIDIOC_DQBUF` 的 `EAGAIN` 竞态处理，在无摄像头环境下只能靠代码审查。
3. **NV12 是假设不是事实**。代码里 `VIDIOC_S_FMT` 请求 NV12，若驱动改成别的
   格式会记录到 `lastError()` 并继续——但这一步的实际行为没验证过。
4. **演练手册里的排针引脚定义我没有核实**。文中明确写了「若无法确定必须先查
   官方 pinout，不得靠猜」——接线错误可能损伤硬件。
5. **「uart2 overlay 只多一个串口节点」是推断**，基于 dtbo 名称与实际影响面。
   这正是 Step 3 要验证的事。

### 后续是否值得升格为 OpenSpec change

**不建议**。理由与阶段 1 一致：

- `V4l2FrameSource` 是纯新增，`IFrameSource` 接口不变，默认态无行为变更；
- 演练手册是文档，不产生代码变更。

真正会改变生产行为的是「把 `V4l2FrameSource` 接进 `server.cpp` 的评估循环」——
那一步会让 `video_qoe_attribution` 从恒 `NONE` 变成可能非 `NONE`，是行为变更。
但按 thermal 的先例（`2026-11-06-thermal-monitor.md`），新字段默认 `NONE`、
不参与 `OverallPolicy`、序列化只是送出已算出的结论，仍不建议开 change。

### 下一步（按依赖顺序）

1. **板子恢复在线** → 重跑演练手册 Step 1 基线快照（确认事实未漂移）
2. **人在板边** → Step 0 串口验证（硬门槛，不通过则停止）
3. **Step 2-5** → 验证 overlay 启用与回滚闭环
4. **Step 6** → 摄像头 overlay，之后 `V4l2FrameSource` 真机调通
5. **显示屏到货** → 阶段 1.5 HDMI 验证

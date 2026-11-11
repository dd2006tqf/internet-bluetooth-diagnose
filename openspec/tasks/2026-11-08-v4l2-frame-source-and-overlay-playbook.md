# V4l2FrameSource 落地 + 阶段 2 演练手册 + 采购状态订正

> 日期：2026-11-08（2026-11-09 增补：串口门槛通过 + 六处订正 + 引脚争用判断修正）
> 分类：工作流外实施 + 文档迭代
> 结果：已完成（`V4l2FrameSource` 10 项单测全绿，全量 ctest 60/60；演练手册成文；文档六处采购状态订正）
> 2026-11-09 增补：Step 0 串口门槛**实测通过**；Step 1 基线部分抓取（缺 uart 状态，
> 已补采集命令但未重跑）；uart2 引脚争用经复核**不阻塞 Step 2**，但仍是 Step 6 的安全前提

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

> **⚠️ 2026-11-11 订正：这条取舍已被真机推翻，默认值已改为 1640x1232。**
>
> 720p 在本板属于 `bin_factor=2` 模式，**一帧都出不来**（select 超时、
> 0 字节、约 710 条 `tdm0 rx0 frame error! width is 1869`）。`bin_factor=1`
> 的 3280x2464 与由它缩放而来的 1640x1232 才能稳定出帧。
>
> 下文保留当时的推理，作为「未上真机时的合理猜测为何会错」的记录；
> 现行结论见 `server/include/v4l2_frame_source.hpp` 的默认值注释与
> `docs/hardware/radxa-cubie-a7a/sunxi-vin-摄像头链路根因分析.md` §7.9。

8M 219 是 3280×2464。用满分辨率会让 ISP / 内存带宽成为瓶颈，把「ISP 跟不上」
与「网络丢了帧」混在一起，直接污染归因。720p@30 已是 240 Mbps 量级，足够在
弱网下制造可观测丢帧，且留出充足余量。

这条写进了单测（`DefaultConfigMatchesPlan`），防止有人"顺手"改回满分辨率。
（订正后该测试仍然存在且更严格：除等值断言外，另有
`DefaultResolutionAvoidsBinningModes` 逐条排除实测零帧的模式。）

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
- **阶段 2 演练未按序执行**。Step 0/Step 1 已于 2026-11-09 完成；但 Step 2-6
  在 2026-11-11 复查时发现**终点状态已经到达**（uart2 与 camera 两个 overlay
  都已启用，imx219 已被识别），过程则由谁在何时执行不明。**Step 4 回滚演练
  仍无证据表明做过。**
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
4. **接线引脚已核实**：console 走 uart0（`2500000.uart` → `/dev/ttyAS0`），
   接板载调试排针，2026-11-09 已实测连通并读到 U-Boot 菜单。
   **uart2（`2502000.uart`，PB0/PB1，40pin pin 7/11）与 uart0 不争用引脚**——
   2026-11-11 板端实测：`uart0_pins@0` 的 `pins` 属性是空字符串（2 字节），
   console 从未在 pinctrl 里申请 PB0/PB1；overlay 启用状态下 `who` 仍显示
   `radxa ttyAS0`。（原写「仍未解 / 阻塞 Step 2」，已被实测推翻。）
5. **「uart2 overlay 只多一个串口节点」已证实**：实测多出 `/dev/ttyAS2`，
   无其它可见副作用（`/dev/video*` 来自 camera overlay，与它无关）。

### 后续是否值得升格为 OpenSpec change

**不建议**。理由与阶段 1 一致：

- `V4l2FrameSource` 是纯新增，`IFrameSource` 接口不变，默认态无行为变更；
- 演练手册是文档，不产生代码变更。

真正会改变生产行为的是「把 `V4l2FrameSource` 接进 `server.cpp` 的评估循环」——
那一步会让 `video_qoe_attribution` 从恒 `NONE` 变成可能非 `NONE`，是行为变更。
但按 thermal 的先例（`2026-11-06-thermal-monitor.md`），新字段默认 `NONE`、
不参与 `OverallPolicy`、序列化只是送出已算出的结论，仍不建议开 change。

## 2026-11-09 增补：Step 0 已通过 + 六处判据订正

### 新增事实

1. **Step 0 串口门槛实测通过**。用户接板载调试口（Windows + PuTTY 115200），
   `sudo reboot` 后捕获到完整启动链路：BOOT0 → DRAM 8 GiB →
   `U-Boot 2018.07-17-boot-aw2501-g4c7078cfdf7-dirty` → 四条目启动菜单
   （两条带 `rescue target`）→ `Starting kernel ...` → `login:`。
   四项判据（U-Boot 输出 / 菜单 / rescue 标签 / 登录）全部实测满足。

2. **Step 1 基线快照已抓取**（`~/board-baseline-20261109-1213/`）：
   `extlinux.conf`(2547 B)、`managed.list`(552 B)、
   `dt-nodes-before.txt`(51 个顶层节点)、`tty-before.txt`(仅 `/dev/ttyAS0`)。

3. **板端存储是 UFS，没有可拔的 SD/eMMC 卡**。串口日志里
   `ufs has init` + `No MMC device available` 已证实。这直接废掉了原手册
   Step 5「拔卡挂到另一台机器改配置」这条退路。

### 六处手册订正（原内容有错）

| 位置 | 原文（错） | 改为（依据） |
| --- | --- | --- |
| Step 0 接线 | 接 40pin 排针的 UART RX/TX | 接**板载调试排针** → uart0 → `/dev/ttyAS0` |
| Step 0 位置 | 宿主机 `ls /dev/ttyUSB*` + `picocom` | 用户自己的 Windows 电脑 + PuTTY |
| Step 3 判据 | `/proc/device-tree/` 下「出现」uart2 节点 | `status` 由 `disabled` 翻为 `okay`（**节点名后来又被订正为 `uart@2502000`，见 2026-11-11 补记**） |
| Step 5 救援 | 拔卡改 `extlinux.conf` | U-Boot 命令行改，或 FEL/USB 重刷整机 |
| Step 1 基线 | 只抓 51 个**顶层** dt 节点 | 必须额外抓 uart 状态快照（**采集命令后来又被订正：需 `find -L`，见 2026-11-11 补记**） |
| Step 4 判据 | 「uart2 节点消失」 | `status` 回到 `disabled`；节点不会消失 |

**Step 3 判据这条错得最危险**：overlay 不创建新节点，只覆盖已有节点的
`status`。按原判据做拓扑 diff 会得到「无差异」，进而把「已生效」误判成
「启用失败」，然后执行不必要的回滚。

### 引脚争用疑问（**已由板端实测回答：不争用**，2026-11-11）

> ~~本节原结论：「无法从现有证据判断 uart2 overlay 生效后是否会夺走 console 的引脚」~~
> **已被实测推翻，保留原文供追溯。** 见本节末尾的「2026-11-11 实测结论」。

板端 dmesg 有 `uart-ng 2500000.uart: there is not valid maps for state default/sleep`
与 `sunxi:pin-2000000.pinctrl:[ERR]: unknown pin`（×4）；同时
`sun60iw2p1-uart2.dtbo` 声明 `exclusive = "PB0\0PB1\0uart2"`、`pins = "PB0\0PB1"`，
而 `uart0_pins@0` 也带 `pins = [00 00]`（sunxi 二进制编码，大致对应 PB0/PB1）。

~~**无法从现有证据判断 uart2 overlay 生效后是否会夺走 console 的引脚。**~~
**→ 2026-11-11 实测回答：不争用。** 原因见本节末尾。

我最初据此把 Step 2 挂起了（「必须先反编译 dtbo 查明」）。**2026-11-09 用户判断
「感觉好像没有必要解决这个问题」，我复核后同意——原判断过度保守。**

错在把两件事混成一件：

| 事件 | 会不会发生 |
| --- | --- |
| uart2 抢走 console 引脚（若有争用） | 可能 |
| **板子起不来** | **不会**。一个 UART 节点的 `status` 翻转不构成启动失败的成因 |

而救砖通道（串口 + rescue 标签）**只在「板子起不来」时才需要**。若只是 console
失效而板子正常启动，SSH 照样能连，回滚即可。所以这个疑问不必提前 `dtc` 取证——
Step 2 本身就是更便宜的实测手段。

**但风险没有消失，只是从 Step 2 转移到 Step 6**：若 Step 2 真让 console 失效，
那在做摄像头 overlay（真正有变砖风险的动作）之前**必须先把救砖通道修回来**。
因此 Step 3 增设判据 4（显式检查 console 是否仍可用），Step 4 增设「回滚后必须
确认串口恢复」。

#### 2026-11-11 实测结论：不争用，且原因不是「运气好」

板端实测时 uart2 overlay 已处于启用状态，console 完好：

```text
uart@2500000 = okay      uart@2502000 = okay
/dev/ttyAS0  /dev/ttyAS2
who:  radxa  ttyAS0  2026-11-11 01:42
systemctl is-active serial-getty@ttyAS0 → active
```

引脚组的原始字节给出了根因：

| 节点 | pinctrl group | `pins` 实际字节 | 大小 |
| --- | --- | --- | --- |
| `uart@2500000`（console） | `uart0_pins@0` | `0000` | **2 字节 = 空** |
| `uart@2502000`（uart2） | `uart2_pins@0` | `5042300050423100` = `PB0\0PB1\0` | 8 字节 |

**console 的 pinctrl group 压根没写引脚。** 它的引脚在 U-Boot 阶段就配好了，
内核这边不需要再配。dmesg 那条 `not valid maps for state default/sleep`
说的正是「这份 map 里没有有效条目」，字面意思正确，**与引脚冲突无关**。

另外 `exclusive = "PB0\0PB1\0uart2"` 是给 `rsetup` 用的元数据，**内核不读它**，
`u-boot-update` 也不拿它做冲突检测。

> **隐式前提**：uart0 不声明引脚是 BSP 的**遗留写法**，不是设计意图。若有人
> 「顺手修好」`uart0_pins@0` 把 `PB0/PB1` 补进去，冲突会**真的**出现。
> 当前和平依赖「一边不申请」，不是「两边都正确」。

详见 `openspec/pitfalls/dmesg-error-vs-function.md`。

### Step 1 基线的设计缺陷（复查时发现，2026-11-11 再订正）

已抓的 `dt-nodes-before.txt` 只列 **51 个顶层节点**，而全部 uart 节点都埋在
`soc@3000000` 下面——**顶层清单里根本没有 uart 的状态**，Step 3 判据 2 无基线可比。

已在 Step 1 补上 uart 状态快照的采集命令。但 2026-11-11 板端实测发现**那条命令
本身也是错的**（两处）：

1. **缺 `-L`**。`/proc/device-tree` 是指向 `/sys/firmware/devicetree/base` 的
   符号链接，`find` 默认不跟随，实测输出恒为 0——它在 overlay 生效与未生效
   两种状态下都是空，**根本不具备区分能力**；
2. **节点名猜错**。实际被翻转的是 `uart@2502000`，不是 `uart@2501000`。

正确写法见主手册 Step 1。教训见 `openspec/pitfalls/proc-device-tree-find-symlink.md`
与 `openspec/pitfalls/dtbo-filename-node-identity.md`。

### 本轮未做（2026-11-09）

- **Step 2-6 未执行**。用户在 2026-11-09 关闭了板子；执行 Step 2 需要板子开机 +
  人在板边 + 串口终端开着并录制，三个条件缺一不可。
- **Step 1 的 uart 状态基线未补抓**（同上，板子已关）。
- **本地无 dtbo 副本**（`find . -name "*.dtbo*"` 空，`board-assets/` 只有
  `vmlinux.h`），引脚争用无法离线判定——不过按上面的判断修正，这已不是阻塞项。

### 2026-11-11 补记：实测已推进到终点状态

板端实测发现 **uart2 与 camera 两个 overlay 都已启用**（`extlinux.conf` 的
`fdtoverlays` 同时列出两者，mtime `2026-11-10 06:04`），`/dev/video0/1/4/5` +
`/dev/media0` 存在，`imx219` 已加载且 dmesg 里有 `sensor_s_stream on = 1,
1920*1080 30f`。

但必须说清边界：

- **不知道是谁在何时做的**，我没查 git 记录或其它会话；
- **Step 4 回滚演练仍无证据表明做过**——这仍是本阶段唯一的空白，而本板是 UFS
  焊死、没有可拔的卡，回滚不可用的代价是重刷整机；
- `sensor_s_stream on = 1` 是**某个时刻**的 dmesg 残留，不代表此刻能稳定取帧
  （第五轮记录的「TDM 零帧」卡点仍在）。

因此不能写「演练已完成」，只能说「终点状态已经到达，但过程未被按序执行」。

### 下一步（按依赖顺序，2026-11-11 更新）

1. ~~板子开机 → 补抓 `uart2-status-before.txt`~~ → **已核实，且原命令本身是错的**
   （缺 `-L`，且节点猜成 `2501000`）。正确写法见主手册 Step 1。
2. ~~Step 2 启用 overlay~~ → **已生效**。
3. ~~Step 3 验证 `status` 翻转 + 判据 4~~ → **已实测通过**（console 存活，`who` 有
   真实登录会话）。
4. **Step 4 回滚演练 → 仍未做，这是当前最大空白。**
5. ~~Step 6 摄像头 overlay~~ → **已生效**，imx219 已被识别。剩余工作是
   `V4l2FrameSource` 端到端取帧调通（见「TDM 零帧」卡点）。
6. 显示屏到货 → 阶段 1.5 HDMI 验证

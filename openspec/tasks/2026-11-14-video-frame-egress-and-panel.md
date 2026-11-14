# D-Bus 帧出口 + Qt 面板画面框 + sunxi-vin NV12 布局取证

> 日期：2026-11-14
> 分类：工作流外实施
> 结果：已完成（跨进程取帧到面板渲染全链路真机跑通，截图逐像素核对通过）

## 构思

### 要解决什么问题

`openspec/tasks/2026-11-14-video-frame-hub-plan-b.md` 的「遗留问题」前两条：

> 1. **D-Bus 上还没有 `UNIX_FD` 方法。** 中枢有能力，但没有出口。
> 2. **Qt 面板没有画面框。** 用户明确要求「给面板加一个框，框里面是摄像头
>    当前拍到的东西」。

这两件事本来就是一件事的两端：中枢产出的帧必须有**跨进程**的出口，
面板才可能画出来。所以同一次任务里做完。

### 用户的原话与由此确定的形态

> 「给面板加一个框，框里面是摄像头当前拍到的东西」

这句话约束了三件事：

| 要求 | 对应决策 |
| --- | --- |
| 「一个框」 | 面板上是一块固定宽高比区域，不是全屏播放器 |
| 「摄像头当前拍到的东西」 | 必须是**实时**帧，取不到就要明说，不能留上一帧 |
| 「拍到的东西」 | 是这个现场的真实画面，不是占位图、不是测试图案 |

第三条决定了最后必须做**逐像素核对**——「面板上有东西在动」不等于
「画的是摄像头拍到的内容」。

### 为什么传输用 D-Bus `UNIX_FD` 而不是传像素

| 方案 | 代价 |
| --- | --- |
| D-Bus 传像素数组 | 3MB/帧 × 9fps = 27MB/s 走 D-Bus，libdbus 会先整体序列化再拷一次 |
| 文件/shm 约定路径 | 多一套生命周期约定，崩溃后残留需要清理 |
| **`UNIX_FD` 传 dma-buf 句柄（采纳）** | 走 SCM_RIGHTS，只传一个 fd；调用方 mmap 即可，零额外拷贝 |

板端已确认能力：`can_send UNIX_FD = 1`、libdbus 1.12.28、
`max_message_unix_fds` 默认 16。单帧只用一个，远够。

### 为什么不走 OpenSpec change

用户此前明确指示「v4l2 部分的工作不走 change」。按 `CLAUDE.md`
「代码已实现时不套用 OpenSpec change 流程追认」，以本文件作为收尾记录。

## 实现方式

### D-Bus 出口（`server/src/dbus_service.cpp` + `server/include/dbus_service.hpp`）

新增两个方法：

- `GetVideoFrame` → 返回 `(huuuuuuttu)`：
  `fd, width, height, stride, uv_offset, fourcc, size, sequence, timestamp_ns, slot`
- `ReleaseVideoFrame(slot)` → 归还租约。

三处必须记住的跨进程责任（都写进了代码注释）：

1. **`UNIX_FD` 的所有权不归 libdbus。** `dbus_message_iter_get_basic`
   给出来的 fd 是消息里的那一份，消息释放即失效；调用方必须自己 `dup`。
   libdbus 文档不承诺接管，实测确认要靠自己。
2. **有帧分支与无帧分支的 append 链必须**逐字段对齐**。无帧时也要 append
   一个 0 占位，否则签名对不上，客户端按位置解析会错位一位——而错位
   **不会报错**，只会把 `fourcc` 读成别的字段。
3. **签名固定为 `(huuuuuuttu)`，任何一侧加删字段都必须同时改另一端。**
   两端各有一份字段表，位置一一对应。

### 客户端 C API（`client/weaknet_client.h` + `client/client.cpp`）

`weaknet_video_frame` 结构体加 `uv_offset` 字段；解析表在
`{DBUS_TYPE_UINT32, &out->stride}` 之后插入对应项。`slot` 由调用方保管，
用完必须 `weaknet_release_video_frame(slot)` —— **传错槽位会被服务端拒绝**。

### 面板渲染（`qt_dashboard/ui/`）

四个新组件，职责单一：

| 文件 | 职责 |
| --- | --- |
| `nv12_to_rgb.hpp/.cpp` | 纯函数 NV12→RGB888，无 Qt 依赖（除容器），可独立测试 |
| `video_frame_widget.hpp/.cpp` | 画面框控件：等比缩放 + letterbox 黑边 + 占位态 |
| `video_fetch_thread.hpp/.cpp` | 后台线程拉帧，主线程只做渲染 |
| `main_window.cpp`（改） | 接线；`--screenshot` 无显示器验证路径 |

sysroot 内无 QtMultimedia，`QVideoWidget` 不可用，所以转换手写。
BT.601 limited-range 定点公式：`R=298c+409e, G=298c-100d-208e, B=298c+516d`
（`c=Y-16, d=U-128, e=V-128`）。

**取不到帧时必须清掉上一帧**，这是最容易写错的一条：一个想当然的实现会
「最后一帧一直留着」，而现场看到静止画面的第一反应是「摄像头卡了」，
不是「取不到帧了」。有专门的用例钉住它。

### 顺带修正的既有注释错误

`test_video_frame_widget.cpp` 原文写着「本板实测 1640→2460」，
`v4l2_frame_source.hpp` 的 `stride` 注释也写着「bytesperline=2460 才是真正的
stride」。本轮取证证明**这两句都是错的**（见下），已全部改正。

## 本轮的核心取证：sunxi-vin 的 NV12 真实布局

### 现象

第一版实现按「`stride = bytesperline`」和「`uv_offset = stride * height`」
算，板端跑出来的画面**严重偏绿**（缺 U/V 的正确来源，读到全零）。

### 三组独立证据

**证据一：零字节游程扫描**（离线帧 `/tmp/frame2.bin`，3045504 字节）

```text
零字节游程 >4096: [(2020480, 2030336, 9856), (3040576, 3045504, 4928)]
hole [2020480,2030336) len=9856 nonzero=0
tail [3040576,3045504) len=4928 nonzero=0
UV samples: [125,133,131,130,130,131,129,129,128,131,130,129] mean=129.85
```

**证据二：对齐假设自洽性**

```text
align16: uv_offset=2030336 end=3040576 fits=True
align32/64/128: fits=False
bytesperline 2460 总需求 4546080 <= size? False（自相矛盾）
```

关键点：`bytesperline=2460` 时 NV12 整帧需要 `2460*1232*1.5 = 4546080` 字节，
**大于它自己的 `sizeimage=3045504`**。一个装不下自己的行跨度不可能是真值。

**证据三：行间距微扫**（用「相邻行同列绝对差」找周期）

```text
pitch=1640 → 1.1795   ← 尖锐极小值
其它偶数 pitch → 1.54 ~ 1.59
奇数 pitch     → 4.6+
```

真实 stride = 1640 = width，**无行填充**。

**驱动源码依据**（`/tmp/vinref/vin_video.c`）：

```c
:505  cap->frame.bytesperline[0] = cap->frame.o_width * cap->frame.fmt.depth[0] / 8;  // NV12 depth=12 → 2460
:512  size = roundup(o_width, VIN_ALIGN_WIDTH) * roundup(o_height, VIN_ALIGN_HEIGHT);
:542  cap->frame.payload[0] = size * cap->frame.fmt.depth[0] / 8;   // → 3045504（整帧！）
:798  pixm->plane_fmt[i].sizeimage = frame.payload[i];
```

`bytesperline` 用 `o_width × 12/8` 算（把 NV12 当「12 位每像素」），
而 `sizeimage` 用的是「对齐后面积 × 1.5」——**两个字段来自不同的计算，
根本不在同一个度量空间**。用除法从 `sizeimage` 反推 stride 会得到 2472，
逐行取样会斜切。

### 结论：真实布局

```text
[0,        2020480)  Y 平面   stride=1640 × 1232 行，紧凑
[2020480,  2030336)  对齐空洞 9856 字节，实测全零
[2030336,  3040576)  UV 平面  stride=1640 × 616 行，紧凑
[3040576,  3045504)  尾部空洞 4928 字节，实测全零
```

`2020480 = 1640*1232`、`2030336 = align16(1640)*align16(1232)`、
`3040576 = 2030336 + 1640*616`、`3045504 = align16(1640)*align16(1232)*3/2`。

### 由此确立的铁律

> **`uv_offset` 是事实值，由生产者算好原样透传；任何下游都不许推导。**

若下游按 `stride*height` 去读 UV，读到的是空洞里的 0 → U=V=0 →
严重偏绿。这个缺陷下游无法自查（它不知道自己读的是空洞），
所以只能在协议里把它变成必传字段。

## 本轮最难的一个坑：`sizeimage` 在 S_FMT 之后是个**自洽的错值**

### 现象

加完 `deriveGeometry()` 之后第一次部署，日志是：

```text
video frame hub configured: 1640x1232 stride=1640 uv_offset=2020480 size=3045504
                                                          ^^^^^^^^^^^^^^^^ 应为 2030336
```

`stride` 对了，`uv_offset` 却退回了 `stride*height`。

### 第一版判断是错的

我最初凭「`payload[]` 只在 `queue_setup` 里填」这条驱动事实，推断
`S_FMT` 后 `G_FMT` 的 `sizeimage` 是 **0**，于是写下：「guard 因
`sizeimage==0` 被跳过，所以静默失效」。

**真机日志把这个推断推翻了。** 打开 `server.log_level=debug` 后，同一个
进程里 `deriveGeometry()` 的两次调用各打了一行（这行日志就是为本轮取证加的）：

```text
v4l2 geometry: sizeimage=3030720 bytesperline=2460 -> stride=1640   ← S_FMT 之后
v4l2 geometry: sizeimage=3045504 bytesperline=2460 -> stride=1640   ← REQBUFS 之后
```

第一次的 `sizeimage` 是 **3030720，不是 0**。

### 成因：错值恰好自洽，所以所有 guard 都通过

`3030720 = 1640 × 1232 × 1.5`，正是「紧凑 NV12 整帧」的字节数。它比真值
`3045504` 只小 14784（= 9856 + 4928，两块对齐空洞之和），看上去完全合理。

UV 偏移推导的两道 guard 是：

```cpp
const size_t cand = (size_t)frame_sizeimage * 2 / 3;
const size_t uv_bytes = (size_t)frame_stride * (got_h / 2);
if (cand >= uv_off && cand + uv_bytes <= frame_sizeimage) {
    uv_off = cand;
}
```

代入 `sizeimage = 3030720`：

| 量 | 值 | 检查 |
| --- | --- | --- |
| `cand = 3030720*2/3` | **2020480** | 恰好等于 `stride*height` |
| `cand >= uv_off` | `2020480 >= 2020480` | ✅ 卡在**等号**上通过 |
| `cand + uv_bytes` | `2020480 + 1010240 = 3030720` | `<= 3030720` ✅ 卡在**等号**上通过 |

**两道 guard 都是卡在等号上通过的。** 这不是巧合：当
`sizeimage = stride*height*1.5` 时，`sizeimage*2/3` 恒等于 `stride*height`，
加上 `uv_bytes` 又恒等于自己。于是「按紧凑布局」这个兜底被**证明**成了
正确答案——而驱动实际给的是带空洞的布局。

这比 `sizeimage == 0` 危险得多：0 至少会走「我知道自己不知道」的兜底分支，
而 3030720 让系统**确信自己知道**。

至于为什么是 3030720 而不是按 `bytesperline=2460` 算的 4546080：填充它的一方
用的是 1640（真实 stride），不是 2460。这条填充路径在 `v4l2-ioctl.c` 的
`v4l_s_fmt`/`v4l_g_fmt` 默认值逻辑里，而 `bytesperline` 是驱动在 `S_FMT`
响应里后填的——两者读到的时刻不同。**这一段文档标为「未完全查清」**，
但它不影响结论：关键是「S_FMT 后拿到的 sizeimage 能通过所有自洽性检查」。

### 修法

抽出 `Impl::deriveGeometry()`，在**两个时机**各调用一次，第二次生效：

```cpp
// 第一次：S_FMT 之后（此时 sizeimage 可能是框架填的自洽默认值）
s.deriveGeometry(fmt, use_mplane, got_w, got_h, got_fmt);

// ---- 5.5 重新读一次几何（**顺序是硬约束**）----
// sizeimage 来自 frame.payload[]，而 payload 只在 queue_setup()
// （VIDIOC_REQBUFS 时）才被算成真值。S_FMT 之后读到的是框架默认值，
// 它能通过自洽性检查但数值是错的，所以必须在 REQBUFS 后重算。
if (::ioctl(s.fd, VIDIOC_G_FMT, &fmt) == 0) {
    s.deriveGeometry(fmt, use_mplane, got_w, got_h, got_fmt);
}
```

第二次部署后：

```text
video frame hub configured: 1640x1232 stride=1640 uv_offset=2030336 size=3045504 backend=dma-heap
```

### 为什么这个坑值得单独记

它是「**依赖顺序的静默失效**」，而且是最坏的一档：不是「校验被跳过」，
是「**校验通过但值为假**」。函数逻辑完全正确、单测全绿、没有一行错误日志，
只是调用早了——而错值自洽到能骗过为它写的每一道检查。

后果（画面偏绿）看起来像转换公式错、像 UV 采样错，唯独不像「驱动填字段的时机」。

已提炼为 `openspec/pitfalls/v4l2-sizeimage-selfconsistent-wrong.md`。

## 验证

### 单测

| 套件 | 环境 | 结果 |
| --- | --- | --- |
| `test_video_frame_hub_gtest` | x86 | **25/25 PASSED**（6201ms） |
| `test_video_frame_dbus_gtest` | x86（真实 dbus-daemon） | **6/6 PASSED**（2475ms） |
| `test_nv12_to_rgb` | **板端** | **15/15 PASSED** |
| `test_video_frame_widget` | **板端**（`QT_QPA_PLATFORM=offscreen`） | **9/9 PASSED**（30ms） |

Qt 测试只能在板端跑：宿主机没有 Qt5 开发包（`pkg-config Qt5Widgets`
返回非零、`/usr/lib/x86_64-linux-gnu/cmake/Qt5` 不存在），也没有 qemu-user。
所以走交叉编译 `build-qt-dashboard-arm64` 后 scp 到板端执行。

新增的关键用例（都是先想看它失败才写的）：

- `UvOffsetWithAlignmentHoleIsHonored` / `UvOffsetWithAlignmentHoleReachesTheConverter`：
  UV 偏移严格大于 `stride*height`、中间夹零空洞；若任一层丢了 `uv_offset`，
  转换器会读到空洞的 0 → 严重偏绿。判据是「中性 UV 应得灰」（通道差 < 8）。
- `ZeroUvOffsetMeansStrideTimesHeight`：0 是约定值不是「无偏移」，两条路径
  RGB 必须完全相同。
- `UvOffsetInsideYPlaneIsRejected`：`uv_offset=stride` 必须 false（会读到 Y 平面）。
- `UvPlaneMustFitEvenWhenTailHoleMissing`：没有尾部空洞也能画，但缺一行不行。
- `UvOffsetCrossesProcessBoundary`：D-Bus 线上位置解析（6 个 u 不是 7 个）。
- `UvOffsetSurvivesTheLeaseBoundary` / `GeometryChangeOnUvOffsetRebuildsSlots`：
  中枢层面的透传与重建。

### 跨进程探针（不入库）

`LD_LIBRARY_PATH=/tmp/wntest3 /tmp/wntest3/probe_dump2`：

```text
OK seq=3182 1640x1232 stride=1640 uv_offset=2030336 fourcc=0x3231564E size=3045504 slot=1
wrote 3045504 bytes to /tmp/wntest3/frame2.bin
```

证明服务端算出的几何经 D-Bus 原样到了另一个进程。

### 真机面板截图（本任务的最终验收）

板端 `QT_QPA_PLATFORM=offscreen ./weaknet_dashboard --screenshot /tmp/panel.png
--screenshot-delay 6000` → `screenshot: 1024x929 saved to /tmp/panel.png`。

把 PNG 解码后逐像素核对：

| 项目 | 实测 | 应为 | 判定 |
| --- | --- | --- | --- |
| 画面框位置 | y=96..335，x=12..251 | — | 有独立边框 |
| letterbox 黑边 | 上 96..125、下 305..335 全黑 | 上下留边 | ✅ 等比缩放 |
| 画面区宽高比 | 236×178 = 1.326 | 1640/1232 = 1.331 | ✅ 未拉伸 |
| 画面区 RGB 均值 | R=17.02 G=10.47 B=15.29 | 原始帧同公式：R=17.16 G=10.98 B=15.95 | ✅ 差 < 0.7 |
| 最大通道差 | 20 | — | ✅ 无偏色（偏绿时 G 会主导） |

**最后两行是本任务最关键的证据**：面板上画出来的像素均值，与「直接从
`/tmp/frame2.bin` 用同一个 BT.601 公式算出来的均值」逐通道吻合。
这说明画面框画的确实是那一帧真数据，且几何/UV 偏移全链路无误——
不是占位图、不是偏色、不是半帧。

画面本身很暗（Y 均值 27.55，limited-range 黑点是 16），是环境光照/镜头
问题，不是转换缺陷；同一帧的 ASCII 增益拉伸渲染能看出真实场景结构。

## 结果

### 已达成的

- **D-Bus `UNIX_FD` 帧出口落地**，跨进程取帧在真机验证（探针 + 面板两条路）。
- **Qt 面板画面框落地**，真机截图逐像素核对通过。
- **sunxi-vin NV12 布局钉死**：stride=1640、uv_offset=2030336、两段空洞，
  三组独立证据 + 驱动源码，结论已写进 `v4l2_frame_source.hpp`、
  `video_frame_hub.hpp`、`nv12_to_rgb.hpp` 三处头注释。
- 顺带修正了 `v4l2_frame_source.hpp`、`test_video_frame_widget.cpp`、
  `docs/ideas/视频流产品化-远程查看与事件留证.md` 里「stride=2460」的错误说法。
- `docs/ideas/视频流产品化-远程查看与事件留证.md` 已更新：勾掉 D-Bus 出口与
  面板两项，新增「帧内存布局」一节，把面板与推流明确记为同一条工作线。

### 遗留问题

1. **推流/网络场景判定未开始。** 硬件编码器可用性仍 UNVERIFIED
   （`/dev/cedar_dev` 存在，但板端无 `ldconfig`，libmpp 无法确认）。
   软件 x264 已实测可实时（`ultrafast` 档 36.81~42.19fps 对 18.12fps 采集）。
2. **面板未纳入 `tools/ci.sh` 部署产物。** 本轮是单独交叉编译后手工推到
   `/tmp/qtdash/` 验证的。`tools/ci.sh` 属 harness 脚本，禁止中途修改，
   要做的话必须单独一次改动。
3. **`--screenshot` 的等待时间是固定值**（默认 3000ms）。若首帧迟迟不到，
   截出来是占位态而不是画面。本轮因面板确实拿到了帧才没受影响，
   但这条判据本身不够硬。
4. **TDM 零帧故障的修法是运维动作，不在代码里**：见下节。

### 附录：TDM 零帧故障的修法（运维）

采集侧偶发「`/dev/video0` 打开成功但一直零帧」，`dmesg` 可能只有
TDM width=0 一类的提示。修法是整条 VIN 模块链重新加载：

```bash
sudo systemctl stop weaknet-server
sudo rmmod vin_v4l2 imx219 vin_io
sudo modprobe imx219 && sudo modprobe vin_v4l2
sudo systemctl restart weaknet-server
```

顺序不能反（`vin_io` 是底座，`vin_v4l2` 依赖它，`imx219` 是子设备）。
**先停服务再卸模块**，否则服务持有 `/dev/video0` 会让 `rmmod` 报
`Device or resource busy`。

### 后续是否升格为正式 change

**面板/推流这条线暂不升格。** 它仍处于「边做边定形态」阶段，
且用户已明确要求 v4l2 部分不走 change。若将来要把「推流 + 网络场景判定」
做成受管特性（新增生产表面：编码器集成、推流协议、云端接收），那时升格。

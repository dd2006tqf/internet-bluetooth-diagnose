# 视频编码器接入：cedar VE 硬件 H.264

> 日期：2026-11-14
> 分类：工作流外实施
> 结果：**编码侧已完成** —— 板端真机 40 帧连续编码通过、SPS 尺寸吻合、
> 零拷贝与 memcpy 两条路径产出等价；尚未接入生产采集循环、未做推流

## 构思

### 要解决什么问题

`openspec/tasks/2026-11-14-video-frame-hub-plan-b.md` 的遗留第 3 条写着：

> **推流/网络质量判断未开始。** 硬件编码器可用性仍 UNVERIFIED
> （`/dev/cedar_dev` 存在，但板端无 `ldconfig`，libmpp 无法确认）。

本次任务把它拆成两步，这一步只做**编码**：把帧中枢的像素转成 H.264 码流。
推流与网络场景判断留到下一步。

### 为什么接口只吃「内存 + 几何」

与 `VideoFrameHub` 同一个理由：**不 include 任何内核头**。于是整条编码逻辑
可以在没有摄像头、没有 VE 硬件的机器上单测（灌假帧、验码流、验计数）。

唯一的例外是 `VideoFrameView::dmabuf_fd`——它是 `int`，不是
`#include <linux/dma-buf.h>`。传 -1 就退化成纯内存路径。

### 为什么零拷贝默认关闭

这是本次最重要的一个设计判断。

本板几何条件确实满足零拷贝：真实 V4L2 帧的 UV 偏移 2030336 恰好等于
`align16(1640) × 1232`，而编码器 `AllocInputBuffer` 要的正是
`sizeY=2030336 sizeC=1015168`，两者之和 3045504 严丝合缝等于真实 `sizeimage`。
实测两条路径产出同级码流，而零拷贝省掉每帧 3.0 MB 的 memcpy。

但硬件编码是**异步**的：`encode()` 返回时硬件可能仍在读这块 dma-buf。而帧中枢的
租约协议是「`release(slot)` 之后中枢就可以覆写这块内存」。于是调用方若在
`encode()` 返回后立刻 release，中枢下一次发布就会把新帧写进硬件正在读的内存——
**画面错乱且不报任何错**。

所以 `allow_zero_copy` 默认 false，并在头文件里写死了调用方必须遵守的约束：
**持有租约直到下一次 `encode()` 返回或 `flush()` 完成**。

默认值必须让调用方什么都不做也是对的。

### 为什么用 dlopen 而不是静态链接

三个理由，缺一不可：

1. **符号冲突**（见 `openspec/pitfalls/vencoder-symbol-interposition-trap.md`）：
   `H264EncOpenVer2` 在 `libvenc_codec.so` 和 `libvenc_h264.so` 各有一份实现，
   链接命令行决定用哪个，写错就是 `ic 21320` 错误。
2. **sysroot 里没有这些库**：ARM64 交叉编译环境是干净的 Bullseye sysroot，
   不含板厂 cedarc 库，静态链接直接链接失败。
3. **ABI 边界要显式**：dlopen + 手写结构体声明，让「我们到底依赖了哪些字段」
   一目了然。

## 实现方式

### 新增文件

**`server/include/video_encoder.hpp`** — 接口

- `EncoderStats`：与 `FrameStats` 逐条同构（`window_ms` 是**事实**时长不是配置值；
  `valid` 在无输入时必须 false）。
- `dropped` 与 `failed` 是两个计数：前者是我们主动降级（队列满丢帧保实时），
  后者是编码器坏了。归因结论完全不同，混在一起就再也分不开。
- `VideoFrameView` / `EncodedFrame` 都是不持有内存的视图。

**`server/src/video_encoder.cpp`** — 实现（约 1000 行）

- `VencLibrary`：进程级单例，`dlopen("libvencoder.so")` **只加载一次、永不卸载**。
  不 `dlclose` 的理由：`VideoEncCreate` 内部的芯片判定依赖全局状态，失败后重试
  也不会重新走干净的初始化；而 `dlclose` 会把 VE 上下文连同寄存器页一起撤掉，
  其他实例可能还持有它。失败结果也缓存——`/dev/cedar_dev_ve2` 不存在是环境事实。
- `CedarVideoEncoder`：真后端，memcpy 与零拷贝两条路径。
- `FakeVideoEncoder`：无硬件桩，**把失败语义做成可断言的真值表**。
- `createVideoEncoder(bool prefer_hardware)`：硬件优先，失败回落桩，**保证非空**。

**`server/test/unit/test_video_encoder_gtest.cpp`** — 18 个用例，无需硬件

**`third_party/allwinner-venc/include/`** — 7 个板厂头文件（2338 行），
只为查阅与实际编码无关的声明；生产代码不 include 它们。

**`tools/board_encoder_check.cpp`** — 一次性板端验证程序

### 实现中发现并修掉的三个真问题

**1. `drainOne` 的 use-after-free**

`FreeOneBitStreamFrame` 把缓冲还给库的内部池，下一次 `GetOneBitstreamFrame`
会立刻复用。而接口契约是「data 在本次调用返回后仍然有效」——直接把 `pData0`
交给调用方就成了 UAF。修法：先 `m_bs_buf.assign` 拷一份再 Free。

板厂 demo 看不出这个问题，是因为它 Free 之后立刻写文件，中间没有别的编码发生。
那是时序侥幸，不是契约。

**2. `CdcMemOpen` / `CdcMemClose` 根本不是导出符号**

见 `openspec/pitfalls/static-inline-not-exported-symbol.md`。这条导致板端
「跑通」了但实际上全程用的是桩后端。

**3. `nBufferNum = 1` 让像素池立刻耗尽**

`AllocInputBuffer` 的 `nBufferNum` 是**像素缓冲池**大小（每块 3 MB），
不是流水线深度，**也不是**决定那条「4 帧上限」的参数——那是另一套
描述符队列，长度硬编码在 `VideoEncCreate` 里。两个池子容易混为一谈，
详见下一节。

## 验证

### 单元测试（x86，无摄像头无硬件）

```
[==========] 18 tests from 3 test suites ran. (81 ms total)
[  PASSED  ] 18 tests.
```

覆盖的都不是「能不能编出码流」（那要看硬件），而是**静默的语义边界**：
几何不匹配显式拒绝、dropped/failed 分离、无输入时 `valid` 为 false、
`window_ms` 是事实值、零拷贝默认关闭。

### ARM64 交叉编译

```
cmake --build build-arm64-native -j$(nproc)  → RC=0
```

顺带修了一个真实的链接缺陷：`server_lib` 用了 `dlopen` 但 `dl` 没往下游传，
所有测试目标链接失败。修法是在 `server_lib` 的 `target_link_libraries` 里加
`${CMAKE_DL_LIBS}` 且声明为 `PUBLIC`（静态库里含未定义的 `dl*` 符号，
最终可执行文件必须带上）。

### 板端真机（部分）

VE 后端**确认可用**（这是本轮最重要的正面结论）：

```
[enc] backend=cedar-ve describe=cedar-ve
[enc] configure OK: cedar-ve 1640x1232 align_w=1648 uv_offset=2030336 path=memcpy
[src] /dev/video0 不可用（生产服务独占），改用 dma-heap 合成帧
  f0 seq=0 size=68839 key=1 hdr=00000001
  f1 size=5672 key=0 / f2 size=4686 key=0
```

首帧就是 68839 字节的 IDR，Annex-B 起始码 `00 00 00 01` 正确，
`/proc/interrupts` 的 `cedar_dev_ve2` 计数随编码增长——**硬件真的在编**。

验证程序为什么用合成帧而不是真摄像头：`/dev/video0` 在 `open()` 层面独占，
生产服务 `weaknet-dbus-server` 长期占着它。验证程序不该为了自证而打断在跑的服务。
而「编码器吃得对不对」只依赖**内存在哪、布局如何**，与像素来自哪里无关——
所以用 dma-heap 分配一块与真实 V4L2 帧**逐字节同构**的缓冲（同样 1648 行宽、
同样 9856 字节对齐空洞、同样 3045504 总长）。

## 结果

### 已达成的

- `IVideoEncoder` 接口与 cedar VE 实现完成，18 个无硬件依赖的单测全绿。
- **VE 硬件后端在真机上确认可用，40 帧连续编码全部成功。**
  板端实测（x86_64 宿主机交叉编译 → aarch64 板端执行）：

  ```
  [stats] in=40 out=40 bytes=189931 keys=3 dropped=0 failed=0 queue=0 valid=1
  [sps] profile=77 level=41 coded_w=1648 显示=1640x1232 （期望 1640x1232）
  [sps] ✅ 尺寸完全吻合
  ```

- **零拷贝与 memcpy 两条路径产出等价**：同样 40 帧、同样 189931 字节、
  同样 SPS 尺寸。这证明 `nShareBufFd` 导入协议是对的。
- 修掉五个真实缺陷：`drainOne` 的 use-after-free、`dlsym` 找 static inline、
  `dl` 未传递、描述符队列不回收、SPS/PPS 不拼进码流。
- 产出四份 pitfall 记录。

### 两个曾经的硬阻塞，都已定位并修复

**1. 恰好 4 帧后 `AddOneInputBuffer` 恒定失败**

反汇编实证：`VideoEncCreate` 里 `ctx+104` **硬编码为 4**，该值是
FrameBufferManager 的**描述符队列**长度（`libvenc_base.so` 的 `fbm+0x68`）。
`AddInputBuffer` 从这个队列 dequeue，空了就返回 -1。而唯一的回收动作
`AlreadyUsedInputBuffer`（`+0x58` → `+0x68`）在 `libvencoder.so` 里
只有一个调用点，位于 JPEG 路径的 `AWJpecEnc`——**H.264 路径从不调它**。

于是每帧消耗一个描述符、无人补充，4 帧后耗尽。

被这条误导过的方向（全部无效，因为都作用在另一套池子上）：
`nBufferNum` 1/4/8/16、归还时机三种、每帧抽干码流、送帧节拍到真实 18.5fps、
GOP 1/18、补 demo 的 7 个隐藏 `SetParameter`、`MAX_FRM_NUM 5` 参考帧假设。

**为什么 `nBufferNum` 无关**：它调的是**像素缓冲池**（`fbm+0x10`，每块 3 MB），
而卡住的是**描述符队列**（`fbm+0x68`，每项 384 字节元数据）。
`GetOneAllocInputBuffer` 在卡死时仍成功，正是因为它查的是前者。

修法与板端验证见 `openspec/pitfalls/vencoder-input-descriptor-recycle.md`。

**2. 码流不含 SPS/PPS，不可解码**

`bEncH264Nalu` 的语义是「输出封装格式」而不是「是否带参数集」，两种取值
都不带 SPS/PPS。参数集只能通过 `VideoEncGetParameter(0x101)` 单独取
（实测 24 字节 = SPS 16 + PPS 8），由我们在每个关键帧前拼上去。

这个坑的危险之处在于**所有计数都健康**（`in=40 out=40 failed=0`），
只有解析码流内容才能发现。详见
`openspec/pitfalls/vencoder-sps-pps-not-in-stream.md`。

### 一个差点被骗过去的教训

板端第一次「验证通过」时，输出是这样的：

```
[enc] configure OK: fake 1640x1232 path=memcpy
[stats] in=40 out=40 bytes=200 keys=0 dropped=0 failed=0 valid=1
[total] 收到 40 帧, 200 字节, 0 关键帧
```

`in=40 out=40`、`dropped=0`、`failed=0`——**所有计数都健康**。
但每帧只有 5 字节，`backend=fake`。桩后端在替硬件「工作」。

这就是为什么验证程序必须断言 backend 名字与 SPS，而不能只看计数。
现在 `runCase` 里 `backend=fake` 会直接导致 `parseSps` 失败并返回非零。
这条判据是那次误判换来的，别删。

### 遗留

1. ~~**编码器未接入生产采集循环。** `server.cpp` 里中枢只有面板一个消费者。~~
   **2026-11-14 已完成**：编码消费者作为中枢的第二个消费者接入采集循环，
   与面板走同一条 `acquire()`/`release()` 租约路径。详见
   `openspec/tasks/2026-11-14-video-cloud-receiver.md`。
2. **推流传输与网络场景判断未开始。** 云端**接收端**已完成（见上条记录），
   板端**发送端**仍未实现：`encode()` 产出的码流目前只做计数，没有传输。
3. 板端 `/dev/video0` 被生产服务独占（`open()` 层），真机端到端验证需要
   停服务或用独立的采集节点。编码器验证本身不受影响——它只依赖内存布局，
   与像素来自哪里无关，所以用 dma-heap 合成了逐字节同构的帧。
4. `flush()` 的语义尚未在板端验证：本次 40 帧全部即时出码流，没有积压，
   所以 `flush 0`。硬件有在途帧时 flush 是否真能冲干净，需要单独构造场景。
5. 零拷贝路径的**租约约束**（持有到下次 `encode()` 返回）目前只有单测覆盖，
   真实接进中枢后需要一次专门的验证：中枢覆写正在被硬件读的内存是静默错误。
   **生产接入刻意用 `allow_zero_copy=false`**，就是为了不背这条未验证的义务。

### 为什么没走 OpenSpec change

用户明确指示 v4l2/视频线不走 change。按 `CLAUDE.md` 的「工作流外任务必须留记录」，
以本文件作为收尾记录。

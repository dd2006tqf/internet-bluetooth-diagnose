# 快照编码器（cedar JPEG）接入与真机验证

> 日期：2026-11-20
> 分类：工作流外实施
> 结果：已完成（编码器部分）；消费者接入待定

## 构思

### 要解决什么问题

摄像头已经能出帧，但**没有取图能力**：整条链路只有 H.264 视频流，没有「拍一张照片」的出口。
用户要求的是「能稳定拍摄画面并接入生产路线」，而生产路线上最能被直接消费的产物是一张静态图。

### 为什么不做成 OpenSpec change

`CLAUDE.md` 与 `openspec/tasks/README.md` 都写明：除非用户明确要求，一律走工作流外实施 + 本节记录。
本次是一个能力补充（新增接口 + 实现 + 单测），不是行为变更，也不触碰既有接口契约，
按工作流外实施处理。

### 为什么不复用 `IVideoEncoder`

看起来「都是 NV12 → 压缩字节」，但 `IVideoEncoder` 的每个成员都带 H.264 语义：

| `IVideoEncoder` 成员 | H.264 含义 | JPEG 里是什么 |
| --- | --- | --- |
| `EncodedFrame::keyframe` | 是否 IDR | 无意义（每张都完整） |
| `gop_size` | 关键帧间隔 | 无意义 |
| `bitrate_kbps` | 码率控制 | 无意义（质量参数取代） |
| `flush()` | 冲出 pipeline 延迟帧 | 常为空操作 |
| `encode()` 返回 false | 异步延迟，不是错误 | **就是失败** |

下游 `VideoClipRecorder` 还依赖 `keyframe` 做 GOP 切分以保证片段从 IDR 起 —— 让 JPEG 流进去会产生
「每帧都是关键帧」的荒谬判定。所以做成**独立接口**，语义只有一条：给一帧，给你一张图，或者明确失败。

### 后端可注入的理由

- x86 开发机没有 `libvencoder.so`，整条逻辑必须能在无硬件时单测；
- 「硬件不可用」必须是**可观测的降级**，不是崩溃或静默产坏图。

工厂 `createSnapshotEncoder()` 与 `createVideoEncoder()` 同构：硬件优先、失败回落、**保证非空**，
能力缺失体现在 `configure()` 返回值与 `describe()` 里。

## 实现方式

### 改动文件

| 文件 | 内容 |
| --- | --- |
| `server/include/snapshot_encoder.hpp` | 新文件，232 → 260 行。接口、配置、产物、两个工厂声明 |
| `server/src/video_encoder.cpp` | 加入 `CedarSnapshotEncoder` / `FakeSnapshotEncoder` 与两个工厂（1866 → 1985 行） |
| `server/test/unit/test_snapshot_encoder_gtest.cpp` | 新文件，18 项契约测试 |
| `server/test/CMakeLists.txt` | 注册测试目标 |
| `tools/board_snapshot_check.cpp` | 新文件，板端真机验证探针 |
| `server/include/snapshot_service.hpp` | 新文件，消费者：帧源接口 + 服务声明 |
| `server/src/snapshot_service.cpp` | 新文件，约 470 行：原子落盘、路径中和、去重队列、hub 适配 |
| `server/test/unit/test_snapshot_service_gtest.cpp` | 新文件，11 项消费者契约测试 |
| `server/include/server.hpp` / `server/src/server.cpp` | 接进生产路径：构造、事故触发、停止序列 |
| `server/include/common.hpp` | `resolveSnapshotDir()`，与 history.db 同目录 |

**为什么实现放在 `video_encoder.cpp` 而不是新开一个 .cpp**：ABI 声明（`VencHandle` / `VencBaseConfig` /
`ScMemOpsS` / `VencApi`）与 `VencLibrary` 单例都定义在该文件的匿名命名空间里。另起文件就得复制整片声明，
两份布局迟早漂移 —— 而布局错位在这里的后果是**静默产出坏图**，不是编译错误。

### 关键决策

#### 1. 质量默认值从 85 改为 70（被真机数据推翻的初始判断）

初始版本按 640x480 的旧测量（quality=85 → 约 120 KB）把默认值定为 85。
**真机 1640x1232 实测推翻了它**：

| quality | 体积（合成纹理，6 张/档） | 相对 640 KiB (655360B) |
| --- | --- | --- |
| 60 | 569 KB | 余量 86 KB |
| **70（新默认）** | **624 KB** | **余量 31 KB** |
| 75 | 634 KB | 余量 21 KB |
| 85（原默认） | 686 KB | **超出 30 KB** |
| 100 | 1095 KB | **超出 440 KB** |

云端 `video_contracts.py` 的 `MAX_VIDEO_CHUNK_BYTES = 640 KiB` 是**单分片原始体**上限。
quality=85 会让每张图都变成分片上传（两块 base64、两次请求、两个失败点）。

**这个错误的性质值得记住**：120KB 这个数字本身没错，错在它来自 640x480，被搬到了面积 6.6 倍的
分辨率上。按面积折算 120×6.6≈790KB，实测 686KB 本就自洽。这类错误**不会报错**，只会让「一张图」
悄悄变成「一次分片上传」。所以把实测表写进类型注释，而不是留在提交信息里。

#### 2. 删掉了 `timeout_ms` 配置项

最初设计了编码超时，实现时意识到它是个**假旋钮**：硬件编码调用（`VideoEncodeOneFrame` /
`GetOneBitstreamFrame`）是库内部同步阻塞，没有取消句柄。在调用线程里做超时只能「另起线程去等、
超时丢结果」，而丢结果时那块输入描述符仍归硬件所有 —— 丢结果等于泄漏，下次 `capture()` 直接卡死。

留一个设了却不起作用的旋钮比没有更危险：调用方会以为「最坏情况有上限」。
真正的超时必须在**上层**做，注释里写明了这一点。

#### 3. 三处会静默出错的隐患

| 隐患 | 原写法 | 后果 | 改法 |
| --- | --- | --- | --- |
| `pAddrVirC` 为空 | `if (in.pAddrVirC) memcpy(...)` | 整张图 UV 平面全零/残留，偏色，**不报错** | 显式失败并归还缓冲 |
| 描述符回收失败 | 只记 notes 继续用 | 「第一张成功，之后全失败」，看起来像几何配错 | 拆掉实例，下次 `capture()` 自动重建 |
| 体积超限 | 无检查 | 上传时才 422，看不出是哪个字段不合规 | `SnapshotImage::fitsSingleChunk()` + 编码后告警 |

#### 4. 最后一道守门：不信任后端的成功返回

`capture()` 返回 true 前必须校验产出以 `FF D8 FF` 开头。板厂库在错误配置下可能返回 0 却给出
一段非 JPEG 内存，而这张图会被落盘/上传，事后无法与真图区分。

### 验证手段

**x86（无硬件）**：
```
cmake --build build-x86 -j$(nproc)
ctest --test-dir build-x86/server --output-on-failure
```
结果：**69/69 套件通过**（含新增 18 项快照断言）。

**ARM64 交叉编译**：`cmake --build build-arm64-native -j$(nproc)`，零 error 零 warning。

**真机（合成帧，不碰 /dev/video0）**：
```
clang++-18 --target=aarch64-linux-gnu --sysroot=/opt/sysroot-bullseye-arm64 \
  --gcc-toolchain=/opt/sysroot-bullseye-arm64/usr -std=c++17 -O2 \
  -I server/include tools/board_snapshot_check.cpp \
  build-arm64-native/server/libserver_lib.a build-arm64-native/server/libweaknet_common.a \
  -lglog -ldl -lpthread -o board_snapshot_check
scp board_snapshot_check board:/tmp/
ssh board 'sudo /tmp/board_snapshot_check --frames 6'
```

结果：`backend=cedar-ve-jpeg`，连续 6 张，每张 624 KB（quality=70），约 10~12 ms，
段结构 SOI/APP0/DQT×2/SOF0/DHT×4/SOS/EOI 全部齐备，SOF0 自报 1640x1232 p8 c3。

### 真机踩到的两个环境事实

1. **`/dev/cedar_dev` 对 `radxa` 用户完全不可访问**：权限是 `user::rw-` + `user:sddm:rw-`，
   `other::---`。生产服务以 `User=root` 运行所以不受影响，但**任何非 root 的探针/工具都会失败**，
   而错误信息是 `CdcMemOpen (memops->open) failed`，看不出是权限问题。
2. **`libvencoder.so` 在 `/usr/lib/aarch64-linux-gnu/`**，不在 `/usr/lib`。探针链接时还要显式
   加 `-lglog`，否则 `LOG_INFO` 展开的 `google::LogMessage` 符号未定义。

## 消费者接入（`SnapshotService`）

### 为什么必须接消费者

本仓库铁律：**「没有真实消费者的生产接口是 orphan，必须删掉或回到规划」**。
只在测试和探针里被调用的接口不算接入完成，所以编码器做完之后必须把调用方做出来。

### 形状：按需取帧，而不是持续编码

| 方案 | 代价 | 结论 |
| --- | --- | --- |
| sink 里持续深拷贝 | 3 MB × 30fps = 90 MB/s 的 memcpy | 纯浪费 |
| sink 里持续编码 | JPEG 实测 10~12 ms/张，占 33 ms 帧预算 1/3 | 不可接受 |
| **事故时去 hub 取当前那一帧** | 平时零开销 | **采用** |

`VideoFrameHub::acquire()` 交付的是「dup 出来的 dma-buf fd + slot 号」。适配层
（`HubFrameSource`）在 `acquire()` 里 mmap、在 `release()` 里 munmap + close + 归还槽位。
**三者缺一不可**，且失败路径各不相同：

- 少了 munmap → 每次拍照泄漏 3 MB 地址空间（只在事故时发生，极难发现）；
- 少了 close → dma-buf 引用泄漏；
- 少了 `hub->release(slot)` → 三个槽位很快被吃光，采集侧 `publish()` **全部静默跳过**，
  而采集线程本身看起来完全正常。

### 触发点选在巡检线程，不是编码线程

编码线程只在**有帧可编**时才转，而事故的典型形态恰恰是画面卡死/丢帧 ——
那正是最该拍到一张的时候，也正是编码线程可能卡住的时候。巡检线程每 2 秒必定醒来，
不依赖采集是否还在推进。

### 被单测抓出来的两个真实缺陷

#### 1. 去重只查待办队列，等于没有去重

初版 `request()` 只在 `m_pending` 里查重。但**编码只要 10 ms，而轮询间隔 2 s** ——
第二次轮询时上一张早就拍完出队了，队列是空的，于是又入队。

后果：一次持续 5 分钟的事故产出 **150 张 600 KB 的图（90 MB）**，而代码注释里写的
正是要防止这件事。测试用「先连发 5 次、等拍完再连发 5 次」两阶段断言把它抓了出来
（修之前报 `queued=2 vs 1`）。

修法：`m_attempts` 表同时记录 `tries`（试了几次）和 `done`（是否已拍到）。两个标志
各管一件事——只看 `tries` 会让成功的那次之后还能再拍，只看 `done` 会让失败的无限重试。

#### 2. 路径中和只按字符白名单过滤，留下了 `..`

`../../etc/evil` 逐字符替换后是 `.._.._etc_evil`。它没有分隔符所以不构成穿越，
但那要靠「上游一定不会拼出 `/`」这条脆弱假设兜底。改成**连续两个及以上的点折叠成 `_`**，
结果里永远不可能出现 `..`。

同时测试本身也修了：原来的断言是「文件名里不含 `..`」，这是**用字符串匹配代替性质断言** ——
它会把安全的结果误判成不安全，反过来也挡不住真正的穿越（那要看文件到底落在哪）。
改成断言「只有一层子目录」「子目录名不是 `.` 或 `..`」。

### 失败重试的边界

失败允许重试（事故刚触发时帧源可能还没发布），但 `kMaxAttempts = 3`。
没有上限的话，摄像头真坏了的时候等于每 2 秒对坏硬件敲一次，失败计数也失去意义。

### 停止顺序

```
巡检线程 join  →  快照服务 stop()  →  编码线程 join  →  hub 销毁
```

快照服务经 `HubFrameSource` 持有 hub 裸指针，必须在 hub 之前停。巡检线程先 join 保证
`stop()` 期间不会再有新的 `request()`；`stop()` 会等当前那张拍完（10 ms 等得起，
中途放弃会泄漏一个 dma-buf 租约）。

## 真机端到端验证（事故全链路）

部署后用一个**真实事故**跑通了「事故 → 静帧」的完整生产路径，中间没有任何探针代码参与：

```
./tools/weaknet-incident-forge --apply
```

触发事故 `sitinc_radxa_cubie_a7a_radxa_cubie_a7a_1795155743913`，巡检线程随即落盘：

```
快照已留存 incident=sitinc_...1795155743913
  path=/home/radxa/weaknet/data/snapshots/<incident>/1795155756003_2133.jpg
  bytes=118332  1640x1232  seq=2133  encode_ms=10
```

**118 KB 而不是编码器基准的 624 KB**，原因是两者内容不同：624 KB 来自满屏合成纹理
（最坏情况，逼出质量上限），这张是真实室内场景（大片平坦区域，JPEG 极擅长）。
两者不矛盾，而且说明 quality=70 在真实场景下余量充足。

对落盘文件做了结构核验（不信任「写成功」）：

- SOI `ffd8` / EOI `ffd9` 均在位；
- 段序列 `APP0(16) DQT(67) DQT(67) SOF0(17) DHT(31) DHT(181) DHT(31) DHT(181) SOS(12)`；
- 解析 SOF0 得 `1640x1232 precision=8 components=3`，与帧几何一致；
- 0.0586 bytes/pixel，对 q70 室内场景合理。

清理时只删了 12 条 `forge_` 前缀事件，事故行**有意保留**作为证据。

## 结果

### 已完成

- `ISnapshotEncoder` 接口 + cedar 硬件后端 + 桩后端 + 双工厂，x86/ARM64 双构建通过；
- 18 项契约单测，覆盖几何拒绝、短缓冲拒绝、JPEG 形状、元数据透传、计数分离、失败后恢复、分片边界；
- 真机验证 cedar JPEG 路径可用，默认质量按实测数据定为 70；
- **消费者已接入生产路径**：`SnapshotService`（11 项单测）+ `server.cpp` 事故巡检触发 +
  `resolveSnapshotDir()` 落盘到 `data/snapshots/<incident_id>/`；
- 全量回归：x86 **70/70 套件通过**，ARM64 交叉编译零 error。

### 遗留问题

1. **云端上传这一半被契约挡住**。用户选的是「本地落盘 + 云端上传」（方案 C），但
   `video_contracts.py:107` 把 `codec` 钉死为 `Literal["H264"]`，且全云端没有任何
   image/snapshot 端点。JPEG 传到 `/edge/video-clips` 会被 422 拒绝。
   本地落盘已完成，云端需扩契约或新增端点——这需要用户决策。
2. **未做长时间稳定性验证**。单次事故、6 张连拍成功只证明「不会在第 N 张卡死」，
   不证明几百张、几小时稳定，也不覆盖振动/温循下的排线可靠性。
3. **事故链路依赖 `bt_events` 插件**。`site_incident_correlator` 只由该插件注册，
   插件不可用时巡检线程永远拿到空列表，静帧与片段都不会触发。这是既有结构，本次未改。
4. **零帧的三形态分诊还没进生产代码**。`V4l2FrameSource` 目前只报 `received=0`，
   排线松动与驱动帧解析锁死在运维视角下是同一件事。判据已在
   `tools/board_reopen_recovery_check.cpp` 的 `IrqVerdict` 里跑通，但尚未接进采集层。

## 附：`hub:none` 这个假象（观测性缺陷，已修）

启动日志原本打的是：

```
事故静帧留证已启用: snapshot-service ... frames=hub:none ...
```

而同一进程**关闭**时同一字段是 `hub:dma-heap`。两者不一致说明 `hub:none`
不是「后端坏了」，而是 `VideoFrameHub::backend_name` 的初值——它要等
第一帧到达、`configure()` 按真实几何跑过之后才会被改写。

`VideoFrameHub::describe()` 本来用 `(unconfigured)` 标注了这个状态，
但 `HubFrameSource::backend()` 只取了 `backend()`，把那个限定词丢掉了。
于是「还没收到首帧」在日志里被压缩成 `none`，运维看到只会理解成故障。

修法是补一个 `VideoFrameHub::configured()` 只读访问器，适配层据此区分：

| hub 状态 | 旧日志 | 新日志 |
| --- | --- | --- |
| 还没收到首帧 | `hub:none` | `hub:unconfigured` |
| 后端分配失败 | `hub:none` | `hub:none` |
| 正常 | `hub:dma-heap` | `hub:dma-heap` |

关键在于**头两行现在可以区分了**。新契约由
`test_video_frame_hub_gtest.cpp` 的 `ConfiguredFlagDistinguishesUnconfiguredFromBroken`
钉死。已部署验证：启动日志现为 `frames=hub:unconfigured`，且真机事故拍照仍然成功
（127895 字节，段结构完整）。

## 附：这一轮推翻了自家判据（值得单独记）

加固 `board_reopen_recovery_check` 时，我先把「三处中断全静止 = 排线松动」写成判据。
在**摄像头完全健康**的状态下实跑，才发现这个判据 100% 误报：

```
tdm  = 801    → 801        ← 健康时根本不动
isp  = 80904  → 81017
vinc = 323605 → 324057
```

`tdm`（`5908000.tdm`）是 Level 触发的**错误类**中断，累计 801 次后静默，dmesg 里
582 条 `hblank short` 正是它历史被触发的次数。逐帧推进的真值是 isp 与 vinc。

后果不只是探针写错——`openspec/pitfalls/ribbon-contact-zero-frame-looks-like-software.md`
里那张分诊表也照着错的判据写了。两处都已按实测改正。

**教训**：判据必须先在**已知健康**的样本上跑一遍，确认它不会命中。这次之所以能发现，
是因为我把探针装到板子上真跑了一次；如果只做代码审查，这个错误会带着「已在真机验证」
的标签活下去。

### 是否升格为正式 OpenSpec change

暂不。等云端契约方案确定后，如果涉及既有接口变更，再走完整工作流。

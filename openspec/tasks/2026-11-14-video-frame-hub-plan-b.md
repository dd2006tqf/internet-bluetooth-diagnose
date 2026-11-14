# V4L2 帧中枢（Plan B：单一所有者 + 句柄分发）与生产接入

> 日期：2026-11-14
> 分类：工作流外实施
> 结果：已完成（采集 → 中枢 → 归因全链路已在真机跑通）

## 构思

### 要解决什么问题

`openspec/tasks/2026-11-12-v4l2-selfdriven-capture-window-semantics.md` 的「遗留」
一节写着：

> **生产接入点未合。** `server.cpp` 目前仍然构造默认（`valid()==false`）的
> `FrameStats` 并把 `net_obs.interval_ms` 设成 0，因此归因**恒走
> `frames_unavailable` 分支**并返回 `NONE`。

> **采集链路与归因逻辑都完整，测试在真机上通过；生产循环仍留着接入点没合。**

本次任务就是合上这个接入点，并顺带解决它的前置障碍：**像素往哪里去**。

### 为什么不能简单地「让面板自己打开摄像头」

板端实测（本轮之前的探针证据，已随探针清理）：`/dev/video0` 在 `open()` 层面
独占——A 仅 `open` 成功，B 紧随其后 `open` 得到 `EBUSY`，**只读 open 也独占**。
而 `video1/4/5` 可同时打开，说明是按节点独占而非全局。

这条事实直接否掉了几种更省事的方案：

| 方案 | 为什么不可行 |
|---|---|
| 面板/Qt 自己 `open("/dev/video0")` | 与帧源争抢同一个节点，必然 `EBUSY` |
| 帧源把采集 buffer 的 dma-buf fd 直接交出去 | 见下，会把观测缺陷栽给被观测对象 |
| 面板与帧源各开一个 video 节点 | 只有 video0 通 ISP 主通路，其余节点拿不到同一路画面 |

### 为什么不能直接分发采集 buffer 的 fd

这是本任务最重要的一个取舍。把 `VIDIOC_EXPBUF` 得到的采集 buffer fd 交给
消费者看似最零拷贝，但它有致命后果：

消费者持 fd 时间一长 → 4 个采集 buffer 全被占住 → 驱动无处入队 →
**驱动丢帧** → `FrameStats` 把它记成链路丢帧。

而这条链路的全部意义是**判断网络/链路质量**。把「消费者读得慢」记成
「链路劣化」，等于观测缺陷栽给被观测对象，是本项目明令禁止的事。

所以中枢的方案是：**自己另开一组快照槽位**，从采集 buffer 拷一次过去，
再按租约把快照槽位的 fd 分发出去。代价是一次 memcpy（3MB，A55 上约 1ms），
换来的是采集侧与消费侧的生命周期彻底解耦。

### 备选方案

- **Plan A：单消费者直通。** 帧源只服务一个消费者，不做中枢。
  被否：未来要同时接面板 + 编码器/推流（用户明确要求），单消费者模型必然要重写。
- **Plan B（采纳）：单一所有者 + 中枢分发。** 用户决策原话：
  「那就当然是用b；因为未来我们要引入推流啊，引入rtmp，等等推流相关的
  东西，并且在此基础上进行网络质量的判断」。

### 为什么不走 OpenSpec change

用户明确指示「现在开始做v4l2部分的工作，不走change」。按 `CLAUDE.md` 的
「工作流外任务必须留记录」，以本文件作为收尾记录。

## 实现方式

### 新增文件

**`server/include/video_frame_hub.hpp` + `server/src/video_frame_hub.cpp`**

中枢本体。关键设计：

- **租约协议**：`acquire()` 返回 fd + slot + 几何 + sequence；消费者用完
  `release(slot)` 并 `close(fd)`。fd 与 slot 的生命周期相互独立。
- **`refs > 0` 的槽位绝不被覆写。** 覆写会让消费者看到撕裂画面，且这种
  撕裂无法与解码错误区分。跳过是可观测的（`skipped_busy`）。
- **消费者崩溃可回收**：`lease_timeout_ms` 到期后回收，计入 `reclaimed`。
  租约起点**按槽位**记录，不是全局——全局时间戳会让「一个槽超时」被误判成
  「所有槽超时」。
- **分配分级**：`/dev/dma_heap/system`（真 dma-buf，可交给硬件编码器/显示）
  → 回落 `memfd_create`（可跨进程 mmap，但**不是** dma-buf）。后端名进
  `describe()`，**绝不静默回落**。
- **限速发布**：默认 10ms 间隔策略针对采集 ~18.5fps，避免白拷。

**`server/test/unit/test_video_frame_hub_gtest.cpp`**（21 个用例，无需摄像头）

### 并发安全：两道**互不充分**的保护

这是本次实现里最容易写错、也最值得记录的部分。

`publish()` 为了不阻塞消费者，把 3MB 的 memcpy 放在锁外。这带来两个独立的
危险，需要两道**各自都不够**的保护：

1. **提交正确性**：拷贝期间 `configure()` 可能重建槽位图。
   靠 `generation` 计数器——回锁时 `gen != s.generation` 就作废本次发布。
2. **内存生命周期**：拷贝期间 `configure()` 的 `releaseSlotsLocked()` 可能
   `munmap` 掉目标内存，导致 memcpy 写进已释放地址（直接 SIGSEGV）。
   靠 `std::shared_ptr<Mapping>`——拷贝期间持一份引用，析构推迟到拷完。

仅靠 generation 挡不住悬空写入；仅靠 shared_ptr 挡不住「提交进无关的新槽」。
两者都必须有，代码里各自写了理由。

### 生产接入（`server/src/server.cpp` / `server/include/server.hpp`）

- `ServerContext` 新增 `frame_source` / `video_hub` / `frame_source_running` /
  `video_hub_configured`。
- **首帧惰性配置**：几何只有 `S_FMT` 之后才知道（驱动会校正分辨率），
  所以 `hub->configure()` 在第一帧的回调里做，用原子标志记住已配过。
- **QoE 归因接通**：`net_obs.interval_ms = frames.window_ms`。
  理由写进了注释——本轮归因所据的网络证据是本轮算的，而帧证据覆盖的正是
  「上一轮到现在」这段，两者说的是同一个当下，所以用帧侧的**事实跨度**
  作为两边共同的窗口声明。这不是「凑数字让校验通过」。
- **关停顺序**：先停帧源（不再有新 publish）→ 清标志 → 打印中枢计数 →
  reset 中枢 → reset 帧源。反过来是 UAF。

### 本轮发现并修复的缺陷：describe() 平面标记在 stop 后失真

板端取证时发现同一进程对同一设备给出两种说法：

```
started: v4l2:/dev/video0@1640x1232/1000ms mplane {vin_video0 <- ... <- imx219}
stopped: v4l2:/dev/video0@1640x1232/1000ms splane (stopped) {vin_video0 <- ...}
```

根因：`closeDevice()` 把 `buf_type` 复位成单平面（这对下一次 start 是**必要**的），
而 `describe()` 直接读 `buf_type`。

危害在**事后取证**：拿停止那行日志归档，会得出「本板不需要 MPLANE」这个与
事实相反的结论，进而把唯一能出帧的路径判成无用。

修法：新增 `started_mplane` 影子字段，在探测成功后立刻赋值，**不被
`closeDevice()` 复位**，`describe()` 改读它。这与同文件里 `buffer_source`
早已采用的做法**完全同构**——当时只修了内存模型、漏了平面模型。

### 补充取证手段：中枢停止时打印累计计数

`describe()` 原本只在启动时打印一次，那一刻 `pub=0`。若只留那一行，
事后**无法从生产运行中证明中枢到底发布过没有**——只能看到「配置成功」，
看不到「真的在动」。

所以在 resolve 析构前（唯一存活窗口）补了一行停止日志。这不是装饰，
它是「中枢确实在发布」这个结论可被验证的前提。

## 验证

### 单测（无需摄像头）

- 新增 21 个中枢用例。
- **变异测试证明测试有效**：第一轮把保护关掉后测试 **3/3 存活**——说明断言
  没有鉴别力（只查「没崩」，而 shared_ptr 本来就保证了不崩）。改判据为
  「交付的像素必须属于它声称的 sequence」后，第二轮 **6/6 捕获**（降低迭代
  次数后仍 4/4）。这是本次对「测试价值」唯一的硬证据。
- x86 全量回归：**62/62 通过**。

### ARM64

```
cmake --build build-arm64-native -j$(nproc)  → RC=0
```
只有既有的 `-Wmismatched-tags` 警告，与本次改动无关。

### 真机（Radxa Cubie A7A + IMX219，经 `ssh board`）

**中枢配置成功，且走到真 dma-buf：**

```
video frame hub configured: 1640x1232 stride=2460 size=3045504 backend=dma-heap
```

`stride=2460` 取自 `bytesperline`，**不是** `sizeimage/height=2472`。这两者
来自驱动里不同的对齐计算，用除法反推会让下游逐行取样斜切。

**归因不再 `frames_unavailable`（对照修复前后）：**

修复前（旧二进制）：
```
video qoe not attributed: reason=frames_unavailable frames_valid=no frames_window_ms=0
```

修复后：
```
video qoe attribution: CONSISTENT_HEALTHY frames_bad=no net_degraded=no issues=0
```

D-Bus `GetNetworkExperience` 实际读到（此前生产中从未被赋过值）：
```json
"video_qoe_attribution":"CONSISTENT_HEALTHY",
"video_qoe_attribution_detail":"画面与网络侧指标一致地正常",
"video_qoe_issues":[]
```

**中枢确实在发布（运行 159s 后的累计计数）：**

```
video frame hub stopped: hub:dma-heap@1640x1232/stride=2460/size=3045504
  pub=1478 skip(rate=1477,busy=0,unconf=0) acq=0 rel=0 reclaim=0
```

| 指标 | 实测 | 核对 |
|---|---|---|
| 总帧数 | 1478 + 1477 = 2955 | — |
| 采集速率 | 2955 / 159s = **18.58 fps** | PLL 推算 18.56、校准 53.875ms ✓ |
| 发布速率 | 1478 / 159s = **9.30 fps** | 见下 |
| `skip(rate=1477)` | 理论值 1477 | 差 1（首帧未受节流）✓ |
| `busy=0` | 无租约时从不跳帧 | 4 槽从未写满 ✓ |
| `unconf=0` | 首帧配置一次成功 | 无重复重建 ✓ |

**9.30fps 不是缺陷，它就是正确答案。** 限速条件是「距上次发布 ≥100ms」，
而帧间隔是 53.875ms：`53.875 × 2 = 107.75ms > 100ms`，所以每 2 帧才发布
1 帧，发布率天然是采集率的一半。`18.58 / 2 = 9.29`，与实测 9.30 吻合。

**平面标记跨 stop 已一致（本轮缺陷的验收）：**

```
started: ... mplane {vin_video0 <- ... <- imx219}
stopped: ... mplane (stopped) [fps: 校准为 18fps] {vin_video0 <- ... <- imx219}
```

### 一处必须说明的测试边界

为守住「平面标记跨 stop 不变」加的断言挂在 `V4l2LiveTest` 下，需要
`WEAKNET_V4L2_TEST_DEVICE` 才会跑。但**宿主机的 vivid/vimc 全部只有单平面**
（`device_caps` 的 `CAPTURE_MPLANE` 位为 0，本轮实测确认；我最初把
`0x00200000` 误当成 MPLANE 位，它其实是 `EXT_PIX_FORMAT`）。

在该断言上做变异测试，结果**存活**——因为单平面环境下改动前后都打印
`splane`，没有鉴别力。

所以：**这条断言在 CI 上是假绿灯，不能作为本修复的证明。** 本修复的证据是
上面那两条板端日志。这一点已写在测试注释里，避免后人误信。断言保留的理由是
一旦有 mplane 设备进 CI 它立刻生效，且它顺带钉住了「两个标记不得同时缺失」
这条与设备无关的格式契约。

## 结果

### 已达成的

- 11-12 记录里的「生产接入点未合」**已合**：采集 → 中枢 → 归因全链路在真机
  跑通，D-Bus 上有真实归因值。
- 中枢的租约/限速/回收协议有 21 个无硬件依赖的用例，且测试价值经变异测试
  证明（而非声称）。
- 真机数据三方自洽：采集速率与 PLL 推算吻合、发布速率与限速策略吻合、
  跳帧计数与理论值差 1 帧。
- 顺带修复了 `describe()` 平面标记在 stop 后失真的既有缺陷，并在板端完成
  修复前后对照取证。

### 遗留问题

1. **D-Bus 上还没有 `UNIX_FD` 方法。** 中枢有能力，但没有出口——
   `GetVideoFrame` 之类的 D-Bus 方法尚未实现。板端能力已确认
   （`can_send UNIX_FD = 1`、libdbus 1.12.28、`max_message_unix_fds` 默认 16）。
   **这是 Qt 面板接线的前置条件。**
2. **Qt 面板没有画面框。** 用户明确要求「给面板加一个框，框里面是摄像头
   当前拍到的东西」。sysroot 内无 QtMultimedia，`QVideoWidget` 不可用，
   需要手写 NV12→RGB 渲染。
3. **推流/网络质量判断未开始。** 用户要求「根据摄像头拍到的东西推流，并且
   根据推流情况判断网络场景」。硬件编码器可用性仍 UNVERIFIED
   （`/dev/cedar_dev` 存在，但板端无 `ldconfig`，libmpp 无法确认）。
4. `docs/ideas/视频流产品化-远程查看与事件留证.md` 尚未更新把「Qt 面板 +
   推流」记为同一条工作线（用户明确要求计入对应文档）。

### 后续是否升格为正式 change

**后续的 D-Bus `UNIX_FD` 方法 + Qt 面板接线建议升格**：它新增了生产表面
（D-Bus 方法、fd 传递协议、面板渲染路径），满足 change 的门槛。

本次已完成的部分（中枢本体 + 生产接入）不再补 change。理由：代码先行，
补 change 会导致 footprint 为 0 与 fingerprint 死锁，`CLAUDE.md` 明令禁止
「代码已实现时不套用 OpenSpec change 流程追认」。

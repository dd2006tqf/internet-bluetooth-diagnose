# V4l2FrameSource 深度项：DMABUF 缓冲路径 + media controller 拓扑

> 日期：2026-11-09
> 分类：工作流外实施
> 结果：已完成（10 个文件，全量 ctest 61/61 通过；DMABUF 与拓扑变异各 5/5、4/4 全部被测试捕获）

## 构思

### 要解决什么问题

`V4l2FrameSource` 落地后留下四个「深度项」，本轮做其中两个：

1. **DMABUF 零拷贝缓冲路径**——采集循环原本写死 `V4L2_MEMORY_MMAP`。
2. **media controller subdev 拓扑**——真机上「节点打开了却拿不到帧」时，V4L2
   层只会返回 `EINVAL`，无法区分「设备不存在」「链路未启用」「上游 subdev 没
   配好」三种情况。

（另两项——多平面 MPLANE 与出队延迟归因——已在 `7e52abf` 完成。）

### 为什么不做成 OpenSpec change

按项目 memory `no-new-openspec-change-by-default`（用户 2026-11-06 明确要求
不主动开 change），以及 `docs/ai/工作流使用指南.md` 的适用性判断：这两项是
在**已有能力内部**补实现细节，不改变任何生产接口、不新增 entrypoint、不改变
`FrameStats` 契约，`server.cpp` 也未接帧源（生产态仍恒 `NONE`）。属工作流外
实施。

### 关键前提：没有开发板、没有摄像头

用户明确说明「此时我没有打开开发板并且摄像头也没有就位」。因此全部验证必须
在不依赖真机的条件下完成。这直接决定了后面两个设计选择（见「实现方式」）。

### 备选方案与取舍

| 方案 | 取舍 |
| --- | --- |
| A. 只写 DMABUF 导入路径 | ❌ 板端若无 `/dev/dma_heap`，功能直接不可用且无兜底 |
| B. 只写导出路径（EXPBUF） | ❌ 导出对**本消费者**零收益：采集路径从不读像素，MMAP 已是零搬移 |
| **C. 三级回落：导入 → 导出 → MMAP（采用）** | ✅ 有 dma-heap 时用有实质收益的导入；无则退到导出；驱动不支持导出再退 MMAP |
| D. media 拓扑就地写在 `v4l2_frame_source.cpp` | ❌ 唯一入口 `describe()` 要求 `start()` 成功，本机 vimc 因 `STREAMON: Broken pipe` 走不到，等于零覆盖 |
| **E. 拆成独立纯函数编译单元（采用）** | ✅ 算法可脱离设备测试，19 项无设备单测 |

## 实现方式

### 一、DMABUF：三条子路径与它们的真实收益

| 子路径 | 实现 | 收益 |
| --- | --- | --- |
| 导入 | `/dev/dma_heap/system` + `DMA_HEAP_IOCTL_ALLOC`，把 fd 交给 `QBUF` | **唯一有实质收益**：内存由调用方掌控，可交给外部（编码/显示） |
| 导出 | `REQBUFS(MMAP)` → `QUERYBUF` → `VIDIOC_EXPBUF` → `REQBUFS(0)` → 以 DMABUF 重新 `REQBUFS` | 零收益，仅为无 dma-heap 环境兜底 |
| 回落 | 两级都失败时走 MMAP，并在 `describe()` 标注 `dmabuf->mmap-fallback` | 不静默降级 |

**关键实测发现（都写进了代码注释）**：

1. `REQBUFS(0)` **不会**让已导出的 dma-buf fd 失效——vb2 把释放推迟到 dma-buf
   detach。6.8 内核实测成立，**但不是 ABI 保证**，所以代码只把它当兜底路径。
2. 内核**确实**在 `QBUF` 阶段校验 DMABUF fd：`QBUF(fd=-1)`、`QBUF(普通文件fd)`、
   `QBUF(fd=0)` 全部返回 `EINVAL`。这让「忘记回填 fd」变成响亮的失败而非静默
   错误。

### 二、一个真实缺陷：`describe()` 在 `stop()` 后说谎

`closeDevice()` 会把 `memory` 复位成 `V4L2_MEMORY_MMAP`（否则下次 `start()`
会带着旧类型走 `REQBUFS`）。于是 `stop()` 之后再看 `describe()`，会把「配置了
DMABUF 且成功了」误报成「回落到 MMAP」。

修法：新增独立的 `BufferSource buffer_source` 成员，**刻意不在 `closeDevice()`
里复位**——它记录的是「上一次 start 实际生效的方案」，停止后仍应如实呈现。

### 三、requeue 不能用 DQBUF 写过的结构体

原实现 requeue 时复用 DQBUF 返回的 `v4l2_buffer`，只补 `m.planes` / `m.fd`。
实测（`probe_fd_preserved`）证明 vivid 单平面下 `m.fd` 被内核**原样保留**，
所以「忘了回填」也能成功——即代码依赖了一个内核从未承诺的行为。

**改成构造全新的 `v4l2_buffer`**，字段全部由自己填。这样：

- 不再依赖「DQBUF 会回写 `m.fd`」这个未承诺行为；
- fd 回填从「碰巧冗余」变成**真正承重**（漏填即 fd=0 → 内核 `EINVAL`）。

这个改动同时让变异可被抓到（见「结果」一节的变异矩阵）。

### 四、media 拓扑：拆成纯函数 + 三条踩过的坑

新增 `server/include/media_topology.hpp` + `server/src/media_topology.cpp`
（零 syscall，只做判定与格式化），ioctl 枚举留在 `v4l2_frame_source.cpp`。

**拆分的理由写进了头文件**：ioctl 枚举错了会「响」（枚举不出来就是空的），
而图算法错了**不会响**，只会安静地输出一个看起来合理的错误结论。后者只能靠单测抓。

三个真实踩过的坑（都成了测试用例）：

| 坑 | 症状 | 正确做法 |
| --- | --- | --- |
| 用 `st_dev` 而非 `st_rdev` | 节点匹配**永远失败**且无任何症状（实测 `/dev/video3` 的 `st_dev` 是 `0:5`，`st_rdev` 才是 `81:3`） | 必须传 `st_rdev` |
| 只按 `type & 0x00ff0000` 分类 | Sensor / Debayer / Scaler 全判成同一类 | 位域拆成类别 + 子类 |
| 按「第一个 V4L 节点」认领 | vimc 注册 3 个 video 节点，指到别人家的链路 | 按 major:minor 精确匹配 |

**最重要的一个认知订正**：我一开始以为内核有「旧式位域」与「新式常量」**两套
编码**，并照此写了两轮判定。打印真实值后才发现：

```
MEDIA_ENT_F_IO_V4L              = 0x00010001
MEDIA_ENT_F_CAM_SENSOR          = 0x00020001
MEDIA_ENT_F_LENS                = 0x00020003
MEDIA_ENT_F_V4L2_SUBDEV_UNKNOWN = 0x00020000
```

这些「新式常量」**本身就是位域取值**（`IO_V4L == OLD_BASE | 0x0001`），不存在
两套编码。第二轮判定是永远走不到的死代码。已删除，并把订正过程写进注释与
`MediaEntityConstantsShareTheLegacyBitLayout` 这条守卫用例。

### 五、验证手段

没有开发板与摄像头，全部本机完成：

```bash
# 无设备部分（CI 默认）
cmake --build build-x86 -j$(nproc)
ctest --test-dir build-x86/server --output-on-failure

# 真机路径用 vivid 虚拟驱动（无需摄像头）
modprobe vivid
WEAKNET_V4L2_TEST_DEVICE=/dev/video0 ./build-x86/server/test/test_v4l2_frame_source_gtest

# 强制走导出路径：隐藏 /dev/dma_heap
sudo unshare -m bash -c 'mkdir -p /tmp/eh; mount -t tmpfs none /tmp/eh; \
  mount --bind /tmp/eh /dev/dma_heap; \
  WEAKNET_V4L2_EXPECT_DMABUF_EXPORT=1 WEAKNET_V4L2_TEST_DEVICE=/dev/video0 \
  ./build-x86/server/test/test_v4l2_frame_source_gtest'
```

### 六、变异测试：这一步发现了三个真问题

按项目纪律（「一条靠运气通过的断言，比没有断言更危险」），每条防护都做了变异
验证——把防护代码改坏，看测试会不会红。

**第一轮 3 条变异存活，每条都指向一个真实缺陷**：

| 存活变异 | 真实原因 | 修法 |
| --- | --- | --- |
| requeue 不回填 dma fd | vivid 单平面下内核保留 `m.fd`，不填也对 | 改成构造全新 `v4l2_buffer`（见上文三） |
| describe 漏报 heap 来源 | 测试只断言「包含 dmabuf」，而 `dmabuf->mmap-fallback` 也包含 | 改成精确枚举解析 |
| 防环上限调到 100000 | 只断言「能返回」，而 100000 次终究会返回 | 解析跳数并断言 `<= 20` |

**第二轮又发现测试断言本身有假绿口子**：`dmabuf->mmap-fallback` 被归入「合法
落点」，于是「禁用导出路径」也能通过——回落正好落在 fallback 上。加
`WEAKNET_V4L2_EXPECT_DMABUF_EXPORT=1` 显式开关把两者区分开。

**第三个发现是环境依赖**：`/dev/dma_heap/system` 是 `0600 root:root`，非 root
测试根本走不到导入路径。所以 DMABUF 变异必须在 root 下、且分两种环境各跑一遍，
否则永远是「在某些分支上从未被检验」。

## 结果

### 最终状态

| 项 | 结果 |
| --- | --- |
| 全量 ctest | `100% tests passed, 0 tests failed out of 61`，7.31 s |
| V4L2 套件 | 26 项（16 项无设备 + 10 项 vivid 真机），真机全绿 |
| media 拓扑套件 | 19 项，全绿 |
| DMABUF 变异 | **5/5 全部被抓到**（M1/M3/M4 在 root 下，M2/M5 在隐藏 dma_heap 并强制导出期望时） |
| 拓扑变异 | **4/4 全部被抓到**（ENABLED 优先、防环上限、类型过滤、subdev 分类） |

### 变更文件（10 个）

| 文件 | 性质 |
| --- | --- |
| `server/include/media_topology.hpp` | 新增 |
| `server/src/media_topology.cpp` | 新增 |
| `server/test/unit/test_media_topology_gtest.cpp` | 新增（19 项） |
| `server/include/v4l2_frame_source.hpp` | 新增两个配置开关 + 收益边界说明 |
| `server/src/v4l2_frame_source.cpp` | DMABUF 三级路径、requeue 重构、拓扑拆分 |
| `server/test/unit/test_v4l2_frame_source_gtest.cpp` | 22 → 26 项 |
| `server/CMakeLists.txt` | 加入 `media_topology.cpp` |
| `server/test/CMakeLists.txt` | 加入新测试可执行文件 |
| `CLAUDE.md` + `.ai-harness/manifest.json` | 套件数 60 → 61（已跑 `manifest_sync.sh`） |
| 四份文档 | 套件数、`.cpp` 计数、V4L2 能力表同步 |

### 遗留问题

1. **多平面 DMABUF 未被真机验证**。vivid 本机 64 个实例全是
   `multiplanar=1`（单平面），而改模块参数会影响本机其他使用者。多平面分支的
   fd 回填逻辑已写好，但**没有一条真机证据**。板端 sunxi-vin 是单平面还是多
   平面，需真机确认。
2. **`/dev/dma_heap` 在板端的可用性未知**。若板上没有该节点，实际会落在导出
   路径；而导出路径依赖「`REQBUFS(0)` 不使 fd 失效」这一非 ABI 保证。
3. **帧源仍未接进 `server.cpp`**，生产态视频归因仍恒 `NONE`（与上一轮一致）。
4. **不宣称「断电自愈」「DMABUF 完备」**——本轮只做到「三条子路径都在 vivid 上
   跑通且有变异验证」，换驱动需重新验。

### 后续是否值得升格为 OpenSpec change

**不需要**。两项都是在既有能力内部补实现，未改变 `FrameStats` 契约、未新增生产
接口、`server.cpp` 未接。

真正值得走 change 的仍是下一步：**把帧源接进 `server.cpp` 评估循环**——那会让
`video_qoe_attribution` 从恒 `NONE` 变成可能非 `NONE`，是明确的行为变更。前置
条件是真机出帧。

## 相关

- `openspec/tasks/2026-11-09-v4l2-depth-tests-and-dequeue-latency-wiring.md`（另两项深度项）
- `openspec/tasks/2026-11-08-v4l2-frame-source-and-overlay-playbook.md`（帧源落地）
- `openspec/tasks/2026-11-09-v4l2-doc-sync-and-suite-count.md`（上一轮文档同步）
- `skills/test-driven-development`（变异测试纪律来源）
- `skills/systematic-debugging`（"先证据后结论"的来源，本轮三次纠正错误前提）

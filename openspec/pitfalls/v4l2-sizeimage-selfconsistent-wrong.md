# S_FMT 之后读到的 sizeimage 是一个**自洽的错值**——它不是 0，所以每次都校验通过

> 发现于：2026-11-14
> 位置：`server/src/v4l2_frame_source.cpp` 的 `Impl::deriveGeometry()` 与 `start()` step 5.5；驱动侧 `vin_video.c:505/512/542/800`
> 影响：NV12 的 UV 平面偏移退回 `stride*height`（2020480 而非 2030336），把驱动的 9856 字节对齐空洞当成色度起点 → 画面严重偏色，且**所有 guard 都通过、全程不报错**

## 现象

`V4l2FrameSource` 推导交付几何时，`stride` 算对了，`uv_offset` 却一直是错的：

```text
# 第一次部署（错）
video frame hub configured: 1640x1232 stride=1640 uv_offset=2020480 size=3045504
                                                          ^^^^^^^^^^ 应为 2030336

# 在 REQBUFS 之后重算一次（对）
video frame hub configured: 1640x1232 stride=1640 uv_offset=2030336 size=3045504
```

差 9856 字节，正好是驱动在 Y 与 UV 之间插入的对齐空洞长度。后果是 UV 采样
从全零的空洞里读 → `U=V=0` → 画面严重偏绿。

## 成因

### 真机取证：两次推导拿到的 sizeimage 不一样

打开 `server.log_level=debug` 后，同一进程里 `deriveGeometry()` 被调用了两次
（本行的输出就是为此新加的，见「修法」）：

```text
v4l2 geometry: sizeimage=3030720 bytesperline=2460 -> stride=1640   ← S_FMT 之后
v4l2 geometry: sizeimage=3045504 bytesperline=2460 -> stride=1640   ← REQBUFS 之后
```

**第一次的 `sizeimage` 是 `3030720`，不是 0。**

`3030720 = 1640 × 1232 × 1.5`，恰好等于「紧凑 NV12 整帧」的字节数。
它是一个**完全自洽的值**——比真值 `3045504` 只小 14784 字节（= 9856 + 4928，
两块对齐空洞之和）。

### 为什么自洽的错值比 0 更危险

我最初（凭 `payload[]` 只在 `queue_setup` 里填这一条驱动事实）推测
`sizeimage` 在 `S_FMT` 后是 **0**，于是把坑写成「guard 因 `sizeimage==0` 被跳过」。
真机日志证明这个推测是**错的**，而且错得比想象中更麻烦：

推导 UV 偏移的两道 guard 是：

```cpp
const size_t cand = (size_t)frame_sizeimage * 2 / 3;   // 期望：Y 平面字节数
const size_t uv_bytes = (size_t)frame_stride * (got_h / 2);
if (cand >= uv_off && cand + uv_bytes <= frame_sizeimage) {
    uv_off = cand;
}
```

把 `sizeimage = 3030720` 代进去：

| 量 | 值 | 检查 |
| --- | --- | --- |
| `cand = 3030720*2/3` | **2020480** | 恰好等于 `stride*height` |
| `cand >= uv_off` | `2020480 >= 2020480` | ✅ 通过（等号） |
| `cand + uv_bytes` | `2020480 + 1010240 = 3030720` | `<= 3030720` ✅ 通过（等号） |

**两道 guard 都通过，且都是卡在等号上通过。** 这不是巧合：
`sizeimage = stride*height*1.5` 时，`sizeimage*2/3` 恒等于 `stride*height`，
`+ uv_bytes` 恒等于自己。于是「按紧凑布局」这个兜底被**证明**成了正确答案，
实际上驱动给的是带空洞的布局。

而 `sizeimage == 0` 反而会留下痕迹：`frame_sizeimage == 0` 分支会走紧凑兜底，
至少逻辑上「我知道自己不知道」。`3030720` 让系统**确信自己知道**——这是
「静默失效」里最坏的一档。

### 为什么 S_FMT 之后会报 3030720

`3030720` 不是驱动算出来的，是 **V4L2 framework 的默认值**：

```c
// 内核 v4l2-ioctl.c 的 v4l_s_fmt()/v4l_g_fmt() 路径
if (!pix->sizeimage)
    pix->sizeimage = pix->bytesperline * pix->height;   // 对 NV12 再乘 3/2
```

具体到本驱动的路径：

```c
vin_video.c:505  bytesperline[0] = o_width * depth[0] / 8;    // S_FMT 就填，NV12 depth=12 → 2460
vin_video.c:542  payload[0]      = size * depth[0] / 8;       // 只在 queue_setup 里算
vin_video.c:800  plane_fmt[0].sizeimage = frame.payload[0];   // 只在这里回填
```

`S_FMT` 阶段驱动的 `payload[]` 还没算，`plane_fmt[0].sizeimage` 是 0；但
**`bytesperline` 已经被填成 2460**，V4L2 的默认值逻辑就按 `2460 × 1232 × 1.5`
算出一个数……不过那会是 4546080，不是 3030720。

所以 3030720 的来源必须另找。它精确等于 `1640*1232*1.5`，说明填充它的一方
**已经知道真实 stride 是 1640**——即 `bytesperline` 在这个上下文里并不是 2460。
日志里 `bytesperline=2460` 是**我们读到的值**，而框架算默认 `sizeimage` 时
用的是 `pix_mp.plane_fmt[0]` 尚未被驱动覆盖的那份 `bytesperline`（0 或 width）。
这条路径没有源码可指，故标为 **未完全查清**；但它不影响结论——
**关键是「S_FMT 后 G_FMT 得到的 sizeimage 是一个能通过所有自洽性检查的错值」**，
这一点由日志与等号推导双重证实。

### 为什么必须等到 REQBUFS 之后

`queue_setup()` 是 VB2 在 `VIDIOC_REQBUFS` 时回调的，那是 `frame.payload[]`
第一次被真正算出来的时刻：

```text
size = ALIGN(o_width,16) * ALIGN(o_height,16)   // 2030336
payload[0] = size * depth / 8                    // 2030336 * 1.5 = 3045504
```

`3045504*2/3 = 2030336` = 正确的 UV 起始。同一套 guard 在真值上给出正确答案，
在这个假值上给出错误答案，而**两者都通过**。

## 修法

抽出 `Impl::deriveGeometry()`，在**两个时机**各调用一次，第二次生效：

```cpp
// 第一次：S_FMT 之后（此时 sizeimage 是框架填的默认值，可能自洽但是错的）
s.deriveGeometry(fmt, use_mplane, got_w, got_h, got_fmt);

// ---- 5.5 重新读一次几何（**顺序是硬约束**）----
if (::ioctl(s.fd, VIDIOC_G_FMT, &fmt) == 0) {
    s.deriveGeometry(fmt, use_mplane, got_w, got_h, got_fmt);
} else {
    s.error = errnoText("VIDIOC_G_FMT(after REQBUFS)", errno);
}
```

做成独立方法而不是两处各写一份，理由写在函数头注释里：**两处若各写一份，
早晚会漂移成两个不同的判断标准。**

同时补了一行可观测痕迹——**这不是调试残留**：

```cpp
LOG_DEBUG(weaknet_dbus::LogModule::NETWORK,
          "v4l2 geometry: sizeimage=" << frame_sizeimage
          << " bytesperline=" << (...)
          << " -> stride=" << frame_stride
          << (frame_sizeimage == 0
                  ? " (sizeimage=0: guard SKIPPED, compact fallback)" : ""));
```

理由：`deriveGeometry()` 返回 `void`，几道 guard 全都静默跳过或静默通过，
外部完全看不出区别。没有这行日志，「几何是校验出来的还是蒙出来的」这个问题
**只能靠猜**——本文件的上一版就是因为只能靠猜，把成因写错了。

## 边界

1. **只覆盖「`S_FMT` 后 `sizeimage` 是框架默认值」这一种时序。** 别的驱动可能
   在 `S_FMT` 就填好真值，那时第一次推导就是对的，第二次是幂等重算，无副作用。
2. **`sizeimage` 恰好等于 `stride*height*1.5` 时，本方法无法分辨「真实紧凑布局」
   与「假值」。** 二者在数值上完全同构，只能靠**帧内容取证**区分：
   - 零字节游程扫描：找连续 >4096 的零段，位置即空洞
   - 对齐假设自洽性：`align16` 拟合、`align32/64/128` 不拟合 → 确认 16
   - 行间距微扫：相邻行同列绝对差的极小值 → 真实 pitch
3. **只能靠 REQBUFS 拿真值**：任何「在 REQBUFS 之前就确定几何」的优化
   （例如缓存上次结果）都会重新引入这个 bug。

## 教训

**「校验通过」不等于「值正确」——如果校验的两边来自同一个错误来源。**

这里的 `sizeimage` 和 `bytesperline` 都出自同一次不完整的 `G_FMT`，
guard 用它俩互相验证，自然自洽；而真值要等到驱动第二次填字段才出现。
所以判断「某个参数可不可信」，不能只看它自不自洽，要看**它和什么是独立来源**。

**能通过等号通过的不变量，等于没校验。** `cand >= uv_off` 与
`cand + uv_bytes <= sizeimage` 在紧凑布局下同时取等号，这让「没有校验」
和「校验通过」输出完全一致。写 guard 时应当要求**严格不等式**或额外的
独立证据，否则它只是一条同义反复。

配套：`openspec/tasks/2026-11-14-video-frame-egress-and-panel.md`
「本轮的核心取证」一节记录了三组独立的帧内容取证手法。

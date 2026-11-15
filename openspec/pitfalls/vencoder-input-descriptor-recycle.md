# H.264 恰好编码 4 帧后卡死：描述符队列没人回收

> 发现于：2026-11-14
> 位置：`server/src/video_encoder.cpp`（`encode()` 里的 `AlreadyUsedInputBuffer`）
> 影响：编码器**看起来在工作**——出码流、无报错、计数健康——但第 5 帧开始永久失败

## 现象

板端跑真机编码，输出是这样的：

```
f0 size=68839 key=1        ← 首帧 IDR，68839 字节，一切正常
f1 size=5672  key=0
f2 size=4686  key=0
f3 ...
[stats] in=4 out=4 dropped=0 failed=36
```

`in=4`：只成功送进 4 帧。之后每帧 `AddOneInputBuffer` 返回 -1，库打印：

```
AddInputBuffer fail, all input buffer is used by encoder
```

**而同一时刻 `GetOneAllocInputBuffer` 仍然成功**——池子里明明还有空闲缓冲。

## 排查走过的弯路

这些全部试过，全部无效，因为都作用在错误的对象上：

| 尝试 | 结果 |
|---|---|
| `nBufferNum` 1 → 4 → 8 → 16 | 全部一样，恰好 4 帧 |
| 归还时机：encode 后 / 取码流后 / 待归还队列 | 全部一样 |
| 每帧抽干码流 | 一样 |
| 送帧节拍降到真实 18.5fps | 一样（所以不是灌太快） |
| GOP 1 / 18 | 一样（码流字节数变了，说明参数生效了，但不是病因） |
| 补上 demo 的 7 个隐藏 `SetParameter` | 一样 |
| `MAX_FRM_NUM 5` 参考帧假设（4 = 5-1） | **假线索**，数字对上纯属巧合 |

真正的教训：**「4」这个数字不是从我们的参数里来的**，它是库里的一个常量。

## 成因（反汇编实证）

四段代码拼起来才是完整因果链：

```
VideoEncCreate            libvencoder.so
  mov w1, #0x4
  str w1, [x0, #104]        ← ctx+104 硬编码 = 4

VideoEncInit              libvencoder.so
  ldr w0, [ctx, #104]       ← 取那个 4
  bl  FrameBufferManagerCreate(count=4, ...)

FrameBufferManagerCreate  libvenc_base.so
  cdc_calloc(4, 384)        ← 只有 4 个描述符
  for i in 0..4: enqueue(fbm+0x68, &desc[i])   ← 空闲队列填满

AddInputBuffer            libvenc_base.so  0x1b4c
  desc = dequeue(fbm+0x68)  ← 空闲队列空 ⇒ 返回 NULL ⇒ 打印那句报错
  memcpy(desc, in, 376)
  enqueue(fbm+0x60, desc)   ← 进待编码队列

VideoEncodeOneFrame       libvencoder.so
  … 硬件编码 …
  bl AddUsedInputBuffer     ← 描述符从 +0x60 挪到 +0x58（用毕队列）

AlreadyUsedInputBuffer    libvencoder.so  0x3c2c
  bl GetUsedInputBuffer(fbm, buf)   ← +0x58 → +0x68
  ↑ 全库唯一的回收动作
```

**关键事实：`AlreadyUsedInputBuffer` 在整个 `libvencoder.so` 里只有一个调用点
（`0x4708`），而它在 `AWJpecEnc` 里——那是 JPEG 路径。H.264 路径从来不调它。**

于是：每编一帧消耗 1 个描述符，那个 4 槽队列没有任何补充，4 帧后耗尽。

## 为什么 `AllocInputBuffer` 的 `nBufferNum` 完全无关

这是最容易误判的地方，两套池子长得太像：

| | 变量 | 内容 | 谁消费 |
|---|---|---|---|
| **描述符队列** | `fbm+0x68`，长度固定 4 | 每项 384 字节的**元数据**（地址、尺寸、标志） | `AddInputBuffer` |
| **像素缓冲池** | `fbm+0x10`，长度 = `nBufferNum` | 每项 3 MB 的**真实内存** | `GetOneAllocInputBuffer` |

`nBufferNum` 调的是下面那个。所以把它从 1 改到 16，
描述符队列长度纹丝不动，仍然是 4。

`GetOneAllocInputBuffer` 在卡死时还成功，也是因为它查的是下面那个池。

## 修法

每次 `VideoEncodeOneFrame` 之后立刻回收：

```cpp
int erc = api.VideoEncodeOneFrame(m_enc);

VencInputBuffer used;
std::memset(&used, 0, sizeof(used));
if (api.AlreadyUsedInputBuffer(m_enc, &used) == 0 && used_alloc) {
    api.ReturnOneAllocInputBuffer(m_enc, &used);  // 像素缓冲也还回池
}

if (erc != 0 && erc != 1) return false;   // rc=1 是背压，不是失败
```

板端实测（同一份探针，20 帧）：

```
不调 AlreadyUsedInputBuffer : add_ok=4  bs=4  bytes=1298737  STUCK
调了（memcpy 路径）        : add_ok=20 bs=20 bytes=3573429  FULL
调了（零拷贝路径）          : add_ok=20 bs=20 bytes=3573429  FULL
```

后两行字节数**完全相同**，顺带证明零拷贝导入与拷贝回退产出等价码流。

## 通用判据

**库的「资源池」概念可能不止一个，而且报错信息里的名词未必指你以为的那个池。**

「all input buffer is used by encoder」听起来像「你提交太多帧了」，
实际意思是「我的描述符元数据用完了」——与你提交多少帧无关，与你的
`nBufferNum` 设置更无关。

下次遇到「恰好 N 帧后卡死」这种**精确的、与参数无关的**数字，
第一反应应该是去 `objdump` 里找那个常量，而不是继续试参数组合。
一个不随任何输入变化的数字，一定是被写死在某个地方。

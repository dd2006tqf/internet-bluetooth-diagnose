# 复用 DQBUF 写过的 buffer 做 requeue，依赖了内核未承诺的行为

> 发现于：2026-11-09
> 位置：`server/src/v4l2_frame_source.cpp:1265` 附近（归还 buffer）
> 影响：换一块摄像头板子就可能从「一直能跑」变成「第一帧后全部 EINVAL」

## 现象

最省事的写法是：DQBUF 拿到的 `v4l2_buffer` 直接改都不改，原样传给 QBUF 归还。

在 vivid 上实测，这条路**能跑通**。于是很容易得出「这样写没问题」的结论，
并且还省掉了重新绑定 fd 的代码。

## 成因

问题在于「能跑」和「被承诺」是两件事。

实测结论（vivid 6.8，单平面 DMABUF，探针名 `probe_fd_preserved`）：

```
DQBUF 返回后，buf.m.fd 被原样保留
```

也就是说，内核在 DQBUF 时把 `m.fd` 又写了回来。依赖这个行为，即使代码里
「忘了重新绑定 fd」，requeue 也会成功。

但 V4L2 规范从未承诺 DQBUF 会回写 `m.fd`。驱动完全可以只填
`index`、`bytesused`、`timestamp` 这几个必需字段，其余保持调用前的值。

一旦换上这样的驱动，后果是：

- 第一帧正常（因为初始 QBUF 时 fd 是对的）；
- 之后每一次 requeue 都带 `fd = 0`，内核校验拒绝，返回 EINVAL；
- 而报错点在 QBUF，离「fd 没人填」这个根因隔了好几层。

这类 bug 最难查的地方在于**它不会在你手上出现**——你的开发板驱动恰好回写了
`m.fd`，到了客户现场换块板子才爆。

## 修法

刻意构造一个全新的 `v4l2_buffer`，每个字段自己填。这样 fd 必须由代码显式赋值，
漏填就是 `fd = 0`，内核在 QBUF 阶段立刻拒绝——错误点在根因处暴露。

```cpp
v4l2_buffer requeue{};
requeue.type   = s.buf_type;
requeue.memory = s.memory;
requeue.index  = buf.index;
```

单平面 DMABUF 显式绑 fd：

```cpp
} else if (s.memory == V4L2_MEMORY_DMABUF && buf.index < s.buffers.size()) {
    requeue.m.fd = s.buffers[buf.index].dma_fds[0];
}
```

多平面 DMABUF 每个 plane 各绑一个：

```cpp
for (uint32_t p = 0; p < s.num_planes && p < s.buffers[buf.index].dma_fds.size(); ++p) {
    s.plane_bufs[p].m.fd = s.buffers[buf.index].dma_fds[p];
}
```

MMAP 模式下 `m` 不需要任何输入（内核按 `index` 用自己的映射），所以零初始化
即可，不必分支区分。

已实测确认内核确实会校验：`QBUF(fd=0)`、`QBUF(fd=-1)`、`QBUF(普通文件 fd)`
三种情况均被拒绝，返回 EINVAL。所以这条路是「显式失败」而不是「静默错乱」。

## 边界

多平面那边 `plane_bufs` 是成员变量复用（不是每次新建），因为 `requeue.m.planes`
是指针，指向的数组必须**活到 ioctl 返回**——若用局部变量，ioctl 时数组已析构。
这是必要的差别。

但复用就带来一个风险：上一次调用的残留值可能被沿用。当前代码的处理是
**每处使用前都先 `assign(num_planes, v4l2_plane{})`**（`:1287`、`:1153`、
`:675`、`:455` 等处一致），即每次都重新零初始化再填 fd，所以残留值不会漏进来。

这一点值得记下：如果有天有人为了「省一次分配」把 `assign` 改成 `resize`，
残留值就会复活，且表现为「上一次的 fd 被用来 requeue 这一次的 buffer」——
错误会发生在离根因很远的地方。**这里的 `assign` 不是随手写的，它是那条纪律的承载点。**

## 教训

**「实测能跑」不等于「契约保证」，而 V4L2 这类 ioctl 接口的坑通常长在
驱动实现差异上。**

判断标准不是「我这块板子上跑通了吗」，而是「我这个写法依赖了规范里的哪一条？
规范写了吗？」

凡是依赖「内核会帮我填好」的地方，都要问一句：这是承诺还是巧合。

同类思路：结构体只填必需的字段、其余零初始化，比复用调用方传入的结构体更安全。
后者会把调用前的脏数据一起带进内核。

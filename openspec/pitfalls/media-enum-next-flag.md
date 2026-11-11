# 迭代式 ioctl 用 0..N 循环枚举，会重复拿到同一批实体

> 发现于：2026-11-08
> 位置：`server/src/v4l2_frame_source.cpp:127`（`enumerateEntities`）
> 影响：拓扑里 11 个实体被数成 29 个，链路图凭空多出重复节点

## 现象

`MEDIA_IOC_ENUM_ENTITIES` 看起来就是个「按 id 遍历」的接口，
最自然的写法当然是：

```cpp
for (uint32_t id = 0; id < N; ++id) {
    media_entity_desc ed{};
    ed.id = id;
    if (::ioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &ed) < 0) break;
    ...
}
```

跑起来**不报错，能拿到一堆实体**。实测 vimc 上输出 29 条，
而那台 media 设备里实际只有 11 个实体。

多出来的 18 条不是错误数据，是**同一批实体的重复**。所以下游看到的
拓扑是「节点数虚高、同一节点出现多次」，链路图也会跟着重复。
如果后面按「第一个 V4L 节点」之类的规则取设备，重复会直接导致取错。

## 成因

`media_entity_desc.id` 在查询时的语义**不是「我要第 id 号实体」**，
而是游标。GNU/Linux 头文件对标志位的注释只有一行，但已经说清了：

```c
/* OR with the entity id value to find the next entity */
#define MEDIA_ENT_ID_FLAG_NEXT          (1U << 31)
```

所以协议是：

- `ed.id = <上一次返回的 id> | MEDIA_ENT_ID_FLAG_NEXT` 表示「给我这个之后的下一个」；
- 返回结构里的 `id` 被改写成**实际找到的那个实体的 id**；
- 返回 `id == 0` 表示游标到尽头。

用固定计数器 `i` 去置标志位，传进去的就是 `i | NEXT`，
内核会老老实实回答「id 大于 i 的那个实体」——**它没做错事，
是用的人给了错的游标**。一旦实体的 id 不连续（vimc 里就有跳号），
相邻两个 `i` 会返回同一个实体，于是同一个实体被 push 进 `entities` 多次。

> 未查清的部分：vimc 上「29 条对 11 个实体」这个具体倍数，与内核在
> `find_entity` 找不到时的返回行为有关，我没有读内核源码逐行确认。
> 站点上确认的事实是**数量对不上且出现重复名**，而修复后枚举数与
> `MEDIA_IOC_DEVICE_INFO` 的实体计数一致。契约层面的错误是明确的，
> 但精确的重复次数不该照抄我这个数字去推断实现。

正确写法是**把上一次返回的 id 作为下一次的基址**：

```cpp
uint32_t next_id = 0;
for (;;) {
    media_entity_desc ed{};
    ed.id = next_id | MEDIA_ENT_ID_FLAG_NEXT;

    if (::ioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &ed) < 0) break;

    ...
    if (ed.id == 0) break;      // 内核约定的"最后一个"
    next_id = ed.id;
}
```

两个细节都不能省：

- `next_id = ed.id` 用的是**返回的** id，不是自己手里的计数器；
- `if (ed.id == 0) break;` 必须在 `next_id = ed.id` **之前**判断，
  否则 `next_id` 归零后又从头开始，变成死循环。

## 修法

见上。修改后实测枚举出 11 条，与 `MEDIA_IOC_DEVICE_INFO` 里的实体数一致。

这是一类**迭代协议**，不是「带参数查询」。判断特征是：结构体里既有
输入字段（标志 + 起始位置）又有输出字段（实际找到的值），
且输出字段会被下一次调用当输入用。凡是这种形状的 ioctl
（`VIDIOC_ENUM_FMT` / `VIDIOC_ENUM_FRAMESIZES` / `VIDIOC_QUERYCTRL` 等
都是同一模式），都不能用 `0..N` 循环。

## 边界

同一处代码里还有一个相邻的坑，值得一起记住：

`MEDIA_IOC_ENUM_LINKS` 对 **零链路或零 pad 的实体**会返回 EINVAL。
这个 EINVAL **不是错误**，而是「这个实体没有链路可枚举」的正常回答。
所以调用前必须先看 `probe.links == 0 || probe.pads == 0` 直接跳过
（`v4l2_frame_source.cpp:170`）。

若把 EINVAL 当成失败处理，日志里会出现一堆「枚举链路失败」，
而实际上拓扑完全正常——这是把正常返回误报成故障的典型，
排查时会往错误方向花很多时间。

另一个边界：实体数不是拓扑正确性的证明。29 条和 11 条都能「成功返回」，
所以**枚举条数本身要跟独立来源对数**（这里用 `MEDIA_IOC_DEVICE_INFO`
的实体计数，或直接数链路的两端）。不能只看「有没有报错」。

## 教训

**遇到带 `_FLAG_NEXT` 之类标志位的 ioctl，先确认它是「游标迭代」还是「按值查询」。**

凭直觉写成 `for (i = 0; i < N; ++i)` 的代价不是编译错误，而是
「结果看起来合理、数量对不上」。这类错误在只有一个节点的简单设备上
根本看不出来（vimc 有 3 个 V4L 节点才暴露），换块板子才爆。

再往上一层：**枚举接口的返回值必须与独立来源交叉核对。**
数量对不对，不能靠「它没报错」来证明。

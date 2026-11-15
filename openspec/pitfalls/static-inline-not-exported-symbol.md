# CdcMemOpen/CdcMemClose 不是导出符号，dlsym 永远找不到

> 发现于：2026-11-14
> 位置：`server/src/video_encoder.cpp`（`ScMemOpsS` / `VencApi`）
> 影响：`dlopen` 成功、库可用、代码「跑通」，但后端被静默判定为不可用并回落到桩实现，一帧真码流都没编出来

## 现象

用 `dlopen` 加载 `libvencoder.so` 后，用 `dlsym` 解析 17 个函数指针。
`VencApi::ok()` 返回 false，`missing()` 打印：

```
missing symbols: CdcMemOpen CdcMemClose
```

于是工厂回落到 `FakeVideoEncoder`，板端"验证通过"了：`configure OK`、
`in=40 out=40`、`bytes=200`（每帧 5 字节的假码流）、`failed=0`。
**所有计数都健康，只是没有任何真实编码发生。**

## 成因

`CdcMemOpen` / `CdcMemClose` 在板厂头文件 `sc_interface.h` 里是 **`static inline`**：

```c
static inline int CdcMemOpen(struct ScMemOpsS *memops)
{
    assert(memops != NULL);
    return memops->open();
}
```

它们不是导出符号，`nm -D` 里根本没有这两个名字。它们只是对
`memops->open()` / `memops->close()` 的语法糖。

`MemAdapterGetOpsS()` 返回的是一张函数指针表，真正干活的是表里的 `open`/`close`。

## 怎么绕开

按真实 ABI 布局声明 `ScMemOpsS` 的**前三个**成员，然后自己调函数指针：

```c
struct ScMemOpsS {
    int (*open)(void);
    int (*open2)(void*, void*);
    void (*close)(void);
};
```

只需这三个——其余二十来个（`palloc` / `flush_cache` / `get_phy_by_fd`…）
一个都不用，声明出来只会引入一堆板厂特定类型。

代码锚点：`server/src/video_encoder.cpp` 的 `extern "C"` 块，以及
`configure()` 里的 `CdcMemOpen(m_memops)` / `teardown()` 里的 `CdcMemClose`。

## 为什么这个坑特别值得记

它**不会报错**。回落链设计成「硬件不可用 → 用桩后端 → 永远能返回一个能用的对象」，
这是对的（调用方不必写空指针分支）。但副作用是：`dlsym` 少一个符号，
整条链路就静默降级，而计数仍然「健康」。

`describe()` 是唯一能发现它的地方：

```
[enc] backend=fake describe=fake | cedar VE unavailable: missing symbols: CdcMemOpen CdcMemClose
```

教训：**验证程序必须断言 backend 名字，不能只看计数**。光看 `dropped=0
failed=0` 会把桩后端当成硬件在工作。

## 通用判据

`dlsym` 返回 NULL 且 `dlerror()` 说找不到时，先 `nm -D` 确认那个名字**到底是不是
导出符号**。头文件里看到的函数名，有一半可能只是 `static inline` 包装。

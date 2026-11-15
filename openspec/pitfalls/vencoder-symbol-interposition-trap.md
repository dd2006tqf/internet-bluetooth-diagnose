# libvencoder 的 H264EncOpenVer2 有两个实现，链接顺序决定用哪个

> 发现于：2026-11-14
> 位置：`server/src/video_encoder.cpp`（`VencLibrary`）、`third_party/allwinner-venc/`
> 影响：链接命令里多写一个 `-lvenc_h264`，编码器创建直接失败；错误信息指向芯片型号，与真实原因毫无关系

## 现象

自己写的探针程序（链接 `-lvencoder -lMemAdapter -lVE -lcdc_base -lvenc_base
-lvenc_codec -lvenc_h264 -lvenc_common`）调用 `VideoEncCreate(VENC_CODEC_H264)`
返回 `NULL`，stderr 只有一行：

```
H264EncOpenVer2: error: the driver do not support the ic 21320
```

跟进错误信息去查芯片型号、驱动版本、`/dev/cedar_dev_ve2` 权限，全部正常。
而同一台板子上，板厂自带的 `demoVencoder` 用同样的输入跑得好好的。

## 成因

`H264EncOpenVer2` 在 `libvenc_codec.so` 和 `libvenc_h264.so` 里**各有一份实现**，
两者都会去判芯片版本，但只有 codec 里那份认得本板的 `ic 21320`。

ELF 全局符号解析取「先加载的那一份」。`libvencoder.so` 自己的 `DT_NEEDED`
把 `libvenc_codec.so` 排在 `libvenc_h264.so` 之前，所以只要不干预，
解析到的是 codec 里那份——demo 能跑正是因为这个。

而显式 `-lvenc_h264` 会让链接器把 h264 排在前面（或直接以 DT_NEEDED 顺序
提前），于是解析到 h264 里那份，芯片判定失败。

`LD_DEBUG=bindings` 是唯一的直接证据：

```
demo  : binding file libvenc_h264.so  to libvenc_codec.so : normal symbol 'H264EncOpenVer2'  ✅
probe : binding file libvenc_codec.so to libvenc_h264.so  : normal symbol 'H264EncOpenVer2'  ❌
minimal: binding file libvenc_codec.so to libvenc_codec.so: normal symbol 'H264EncOpenVer2'  ✅
```

## 怎么绕开

两条，缺一不可：

1. **链接时只写 `-lvencoder -lMemAdapter`**，其余 `libvenc_*.so` 全部不写，
   让动态链接器按 `libvencoder.so` 自己的 `DT_NEEDED` 顺序解析。
2. **生产代码用 `dlopen("libvencoder.so", RTLD_NOW | RTLD_LOCAL)`**，不碰
   `libvenc_*.so`。`RTLD_LOCAL` 顺带保证这些符号不会泄漏到全局作用域去和
   别处的同名符号互相插桩。

代码锚点：`server/src/video_encoder.cpp` 的 `VencLibrary` 构造函数。

## 排查过程中被证伪的假设

这些全都花过时间，记下来免得下次重走：

| 假设 | 证伪方式 |
|---|---|
| stride 对齐问题 | 把 stride 改成 16 对齐/不对齐，结果一致 |
| `CdcMemOpen` 调用顺序 | 调整前后顺序，结果一致 |
| 缺物理地址 | 补 `pAddrPhyY/C`，结果一致 |
| 缺 demo 那 7 个 SetParameter | 全部补上，结果一致 |
| 缓冲归还太早 | 改成延迟归还，结果一致 |
| `nWidth/nHeight/nAlign` 全零是 bug | 库就是返回全零，不是 bug |
| 两次 `VeInitialize`（先通用 VE 后 VE2） | 变体探针证明无关 |

真正有价值的教训不是「这次是符号冲突」，而是：**错误信息里出现的名词
（`ic 21320`、芯片型号、驱动）未必是原因所在的层次**。当同一个库的
demo 能跑而你的程序不能时，第一件事应该是 `LD_DEBUG=bindings` 对比
符号绑定，而不是去查 demo 与你的参数差异。

# cedar VE 不把 SPS/PPS 写进码流，必须自己拼

> 发现于：2026-11-14
> 位置：`server/src/video_encoder.cpp`（`refreshHeader()`、`drainOne()`）
> 影响：40 帧全部编码成功、字节数正常、`key=1`、起始码正确，但**码流不可解码**

## 现象

修好「4 帧卡死」（见 `vencoder-input-descriptor-recycle.md`）之后，板端输出：

```
[stats] in=40 out=40 bytes=189931 keys=3 dropped=0 failed=0 valid=1
!! SPS 解析失败（不是合法 H.264）
```

每个计数都健康。首字节 `00 00 00 01` 也是标准 Annex-B 起始码。\n但 dump NAL 类型一看：

```
frame0 len=407 : [0]t=5      ← 只有 IDR，没有 SPS
frame1 len=516 : [0]t=1
frame2 len=513 : [0]t=1
```

`t=5` 是 IDR slice。整份码流里 **NAL type 7（SPS）和 8（PPS）一次都没出现过**。

对照板厂 demo 落盘的文件：

```
off=0   nal_type=7    ← SPS
off=21  nal_type=8    ← PPS
off=29  nal_type=5    ← IDR
```

## 成因

`VencBaseConfig.bEncH264Nalu` 这个名字极具误导性。它的实际语义是\n**「输出用哪种封装」**，不是「要不要带参数集」：

| 取值 | 输出格式 | 含 SPS/PPS |
|---|---|---|
| `0` | Annex-B（`00 00 00 01` 起始码） | **否** |
| `1` | 长度前缀（`xx xx xx xx` + payload） | 否 |

两种都不带参数集。实测确认（探针 26）：

```
A-nalu0 (bEncH264Nalu=0):
  frame0 len=407 : [0]t=5
B-nalu1 (bEncH264Nalu=1):
  frame0 len=407 : NO-STARTCODE first=00000193    ← 长度前缀，不是起始码
```

参数集只能通过**独立接口**取：

```c
VencHeaderData hd;
VideoEncGetParameter(enc, VENC_IndexParamH264SPSPPS /* 0x101 */, &hd);
// hd.nLength = 24, hd.pBuffer = [SPS: 00000001 67 … (16B)]
//                              [PPS: 00000001 68 … (8B)]
```

## 修法

`configure()` 里 `VideoEncInit` 之后立刻取一次并缓存：

```cpp
refreshHeader(api, "after-init");   // 填 m_header
```

然后每个关键帧输出前拼上去：

```cpp
if (key && !m_header.empty()) {
    m_bs_buf.insert(m_bs_buf.end(), m_header.begin(), m_header.end());
    m_bs_buf.insert(m_bs_buf.end(), ob.pData0, ob.pData0 + size);
} else {
    m_bs_buf.assign(ob.pData0, ob.pData0 + size);
}
```

为什么每个 IDR 前都拼而不是只在文件开头拼一次：

- 事件触发的录像片段可能从任意 GOP 开始，没有 SPS 就解不出来；
- 网络丢包后解码器需要重新同步；
- 24 字节 vs I 帧几十 KB，开销可忽略。

修复后实测：

```
[sps] profile=77 level=41 coded_w=1648 显示=1640x1232 （期望 1640x1232）
[sps] ✅ 尺寸完全吻合
```

`profile=77` 是 Main，`level=41` 是 4.1，`coded_w=1648` 是 16 对齐后的编码宽度，\n`显示=1640` 是扣除 crop 后的真实宽度——全部自洽。

## 为什么这个坑值得单独记

它和「4 帧卡死」是同一类错误的两面：

| | 4 帧卡死 | SPS 缺失 |
|---|---|---|
| 表现 | 计数异常（in=4） | 计数完全正常（in=40） |
| 发现方式 | 报错信息 | **只能靠解析码流内容** |
| 危险程度 | 明显坏了 | 悄悄产出不可解码的数据 |

第二类比第一类危险得多。**「编码器返回了非空内存」和「这是合法 H.264」是两件事**，\n中间隔着 SPS 解析这一步。验证程序里那句 `parseSps()` + 尺寸比对不是多余的仪式，\n它是唯一能发现这个问题的判据——别在「跑通了」之后把它删掉。

## 通用判据

板厂头文件里 `b` 开头的字段（`bEncH264Nalu`、`bOnlyWbFlag`、`bEnableCorp`…）\n很容易被当成布尔开关望文生义。**名字里的 `b` 只说明它是 1 字节字段，不说明语义是二选一。**\n\n凡是要靠它决定「输出格式」的，都必须拿真实码流验证一遍字节布局，而不是读注释。

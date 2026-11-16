# 跨日期读日志：把 5 天前的报错当成今天的现场

> 发现于：2026-11-16
> 位置：`journalctl` 全量输出 / `journalctl -b N` 的 boot 归属
> 影响：**据此得出「同一块板子每次开机失败形态都不同」的错误结论，**
> 并围绕这个不存在的现象构建了一整套假设。

## 现象

排查 IMX219 零帧时，我连续几次重启开发板，分别读日志，得到：

| 我报告的 | 我看到的证据 |
| --- | --- |
| 「13:23 那次 boot：TDM 中断 110 次，全部 `frame error`」 | `tdm0 rx0 frame error! width is 1869` 等 |
| 「04:16 那次 boot：TDM 中断 0，错误 0，彻底静默」 | `/proc/interrupts` 全 0 |
| 结论 | 「同一板子同一二进制，两次 boot 两种形态 → 指向冷启动 PHY/lane 训练偶发失败」 |

这个结论看起来很扎实：两次开机、同一份软件、不同表现，那必然是硬件时序的
随机性。**它完全是错的。**

后来按日期做了全量统计：

```bash
$ sudo journalctl | grep -E "tdm0 rx" | awk '{print $1,$2}' | sort | uniq -c
  55771 Nov 11

$ sudo journalctl | grep "frame stats" | awk '{print $1,$2}' | sort | uniq -c
      9 Nov 16
```

| 日期 | `tdm0 rx` 错误 | `frame stats` |
| --- | --- | --- |
| **11-11** | **55771** | 0 |
| 11-15 | 0 | 0（服务当天只有 2 条日志） |
| 11-16 | 0 | 9（全部 `received=0`） |

那些 `width is 1869` / `hblank short` **全部属于 11-11**，是另一个问题
（`sensor-mode-table-fps-lie` 里那批 `bin_factor=2` 坏模式）的数据，
一直在 1.2 GB 的 journal 里躺着。11-15 和 11-16 一条都没有。

**真相是：TDM 从 11-15 起就没有收到过任何数据，一直是完全静默。只有一种形态。**

## 成因

三个因素叠加，每一个都不显眼。

### 一、`journalctl` 默认输出全量历史，不是「本次」

`sudo journalctl | grep "tdm0 rx"` 会扫过 journal 覆盖的**全部时间**。
本机 journal 有 1.2 GB、覆盖 10-19 至今，于是 grep 返回的是`11-11`的
老数据，而我当时脑子里认为「这是我刚重启的那次」。

正确做法是**先限制时间窗口**：

```bash
sudo journalctl -b 0 ...                        # 本次 boot
sudo journalctl --since "2026-11-16 04:00" ...  # 明确时刻
sudo journalctl --since "-10min" ...            # 最近
```

`grep` 之后**必须核对输出的时间戳前缀**。日志行开头的 `Nov 11` 一直在那里，
是我没看。

### 二、`journalctl -b N` 在时钟跳变过的板子上不可靠

发现日志对不上之后我改用 `-b` 编号，结果更糟：

```bash
$ sudo journalctl --list-boots
 -4 ... Sun 2026-11-15 13:03:17 UTC—Sun 2026-11-15 13:22:49 UTC
 -3 ... Sun 2026-11-15 13:22:58 UTC—Sun 2026-11-15 13:58:03 UTC
 -2 ... Sun 2026-11-15 13:58:04 UTC—Mon 2026-11-16 04:23:02 UTC
 -1 ... Mon 2026-11-16 04:35:54 UTC—Mon 2026-11-16 04:36:14 UTC
  0 ... Mon 2026-11-16 04:36:14 UTC—Mon 2026-11-16 04:48:11 UTC
```

用 `-b -3` 和 `-b -2` 去查 `tdm0 rx`，两次都返回 **0**，与全量统计同样为 0
（这次是对的），但更早用 `-b 0` 时拿到的是**长达 12 小时**的 boot（-2，
11-15 13:58 → 11-16 04:23），里面自然包含了我以为是「刚刚」的读数。

板子没有 RTC 电池，开机后 NTP 会校正时钟，**journald 的 boot 边界被
时间跳变写乱**。`-b N` 在这个前提下不能当作可靠的分段依据。

### 三、grep 自匹配（第三次踩）

11-15 当天那 4 条「摄像头相关日志」经查是：

```
Nov 15 11:24:04 sudo[10374]: radxa : COMMAND=/bin/grep -i 'camera capture\|样本仅\|frames' ...
```

**是我自己执行的那条 `grep` 命令被 journald 记下来，又被我自己的 grep 匹配到。**
它看起来像一条应用日志，实际是一条审计记录，内容是命令字符串本身。

第一次踩是 `2026-11-15-camera-zero-frame-imx219.md` 里记过的
「boot -2 的 2 次命中其实是 sudo 命令行里的字符串」，
第二次在 `2026-11-14-system-health-watchdog-plan-b.md`（`pkill -f` 自匹配）。
这是第三次。

## 修法

### 统计日志必须带时间维度

任何「历史上有过几次 X」的统计，**必须同时输出时间分布**，不能只给总数：

```bash
# 错：只给总数，看不出跨了多少天
sudo journalctl | grep -c "tdm0 rx"

# 对：先看分布
sudo journalctl | grep "tdm0 rx" | awk '{print $1,$2}' | sort | uniq -c
```

这次正是靠后一种写法才发现的。**统计的输出形式决定了能不能发现错误**，
只给一个数字的统计，错误空间是无穷的。

### 取证前先确认真实时间窗口

```bash
date                                                    # 现在几点
sudo journalctl --disk-usage                            # journal 有多大
sudo journalctl | head -1                               # 最早一条
sudo journalctl --list-boots | tail -5                  # boot 分段
```

板子无 RTC，`date` 与日志时间戳可能都不准，但**它们彼此的关系**是可用的：
「现在」和「日志里那条」的差值，比绝对时间更可信。

### grep 一律排除审计记录

```bash
sudo journalctl | grep -E "tdm0 rx" | grep -v "sudo\[" | grep -v "COMMAND="
```

或者更稳妥：只查特定单元（应用日志不会混进 sudo 审计记录）：

```bash
sudo journalctl -u weaknet-server | grep "camera capture"
sudo journalctl -k | grep "tdm0 rx"          # -k 只看内核
```

## 边界

- 本篇的教训不能推广成「`journalctl -b` 一定不可用」。本板无 RTC 才导致
  boot 边界错乱；有 RTC 的机器上 `-b N` 是可靠的。**判断依据是
  「这台机器的时钟会不会在开机后跳变」。**
- `grep -v "sudo\["` 也会过滤掉真正由 sudo 启动的进程日志
  （比如服务的 `ExecStart=... sudo ...`）。只在明确想排除审计记录时用。
- 按日期分组也有盲区：如果同一个日期内既有真实现场又有历史残留，
  `uniq -c` 分不出来，还需要进一步看小时，或者干脆按 boot 分段。

## 教训

**只统计数量、不统计时间的做法，会默默地把不同事件混在一起。**

我拿到了真实的 55771 条报错，它们每一条都是真的，可是它们和「我刚重启的
那次 boot」毫无关系。真实数字 + 错误的归属 = 错误结论，
而这个结论还带着「硬件实测」的可信度。

工程上落地成一条规则：**任何跨时间的断言，输出里必须带时间。**
如果一条命令的输出无法回答「这是什么时候的」，那它就还不能当证据。

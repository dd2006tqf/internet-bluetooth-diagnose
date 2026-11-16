# sunxi-vin 的调试日志在这块板子上根本没编进内核，调 `vin_log_mask` 是空转

> 发现于：2026-11-16
> 位置：`drivers/vin/utility/vin_log.h:47-105`、`drivers/vin/Kconfig:72-76`、
> 板端 `.config:8286`
> 影响：整个 VIN 驱动的 `vin_log()` 输出**在内核里根本不存在**。
> 排查零帧时以为「日志没打出来 = 那条路径没走到」，实际是「这个宏被编译成了空语句」。
> 由此推出的一大类「某函数没被调用」的结论全部不成立。

## 现象

排查 IMX219 零帧时，反复出现这样的困境：

- 想知道 `vin_set_mclk_freq()` 有没有成功设频 —— `dmesg` 里没有
  `mclk0 set rate ... get rate ...` 这一行；
- 想知道 MIPI PHY 有没有被 `cmb_phy0_en(1)` 使能 —— 没有对应日志；
- 想知道 TDM 的 `s_stream` 有没有走完 —— 没有对应日志。

于是把方向转向「调日志等级」：驱动里有个全局变量 `vin_log_mask`，
默认值就把 ISP/STAT/VIDEO/TDM 四类关掉了：

```c
/* utility/vin_os.c:26 */
unsigned int vin_log_mask = 0xffff - VIN_LOG_ISP - VIN_LOG_STAT - VIN_LOG_VIDEO - VIN_LOG_TDM;
```

而且它是**运行时可写的**，`/sys/.../vin_log_mask` 属性 store 就是把入参
直接赋给这个全局：

```c
/* vin-video/vin_core.c:92-103  DEVICE_ATTR_STORE(vin_log_mask) */
val = simple_strtoul(buf, NULL, 16);
name = val;                        /* name 展开成 vin_log_mask */
```

（属性组注册于 `vin-video/vin_core.c:2247`。）

看起来只要 `echo ffff > vin_log_mask` 就能打开全部日志。
**写进去没有任何效果，一条新日志都不会出现。**

## 成因

### 一、这个配置项在本板的内核配置里**根本不存在**

```kconfig
# drivers/vin/Kconfig:72-76
config VIN_LOG
	bool "use vin log for debug"
	depends on AW_VIDEO_SUNXI_VIN
        depends on DYNAMIC_DEBUG
	default y
```

`depends on DYNAMIC_DEBUG`。而板端内核配置里：

```
# 5.15.147-99btf1-a733/.config:8286
# CONFIG_DYNAMIC_DEBUG is not set
# 8287
CONFIG_DYNAMIC_DEBUG_CORE=y
```

依赖不满足 → `VIN_LOG` 这个符号**在 Kconfig 里就不可见** →
`.config` 里搜 `VIN_LOG` 三种形态（`=y` / `=m` / `# ... is not set`）**全部为零命中**。
`default y` 也救不了：符号不可见，default 不会被求值。

注意 `DYNAMIC_DEBUG_CORE=y` 与 `DYNAMIC_DEBUG=y` 不是一回事：
前者只提供底层设施（`pr_debug` 可被 `dyndbg` 参数控制），
后者才是那个 Kconfig 依赖所指的开关。

### 二、编译出来就是空语句

```c
/* utility/vin_log.h:28 */
#define USE_SUNXI_LOG		1

/* :47-48 */
#if USE_SUNXI_LOG
#if IS_ENABLED(CONFIG_VIN_LOG)
/* ... 一百行的 switch，每类前缀一个 sunxi_debug ... */
#else
/* :104 */
#define vin_log(flag, arg...) do { } while (0)
#endif
```

`IS_ENABLED(CONFIG_VIN_LOG)` 在符号不存在时求值为 0，
于是 `vin_log(...)` 整个展开为 `do { } while (0)`。
**不是运行时被 mask 掉，是编译期就不生成代码。**

因此 `vin_log_mask` 那个 sysfs 属性成了一个纯粹的空转开关：
写进去的值确实被保存了（`vin_log_mask_show` 能读回来），
但它唯一的消费者 —— `vin_log()` 宏里的 `if (flag & vin_log_mask)` —— 已经不复存在。
连 `sunxi_isp.c:1756` 那两处 `vin_log_mask >> 16` 的读取也一起失去了意义。

### 三、`vin_err` / `vin_warn` / `vin_print` 不受影响

```c
/* utility/vin_log.h:106-108 */
#define vin_err(x, arg...)   sunxi_err(NULL, x, ##arg)
#define vin_warn(x, arg...)  sunxi_warn(NULL, x, ##arg)
#define vin_print(x, arg...) sunxi_info(NULL, x, ##arg)
```

这三个是无条件映射，所以 `dmesg` 里**有** `tdm0 rx0 frame error`（`vin_err`），
**没有**任何 `[VIN_LOG_*]` 前缀的行。这两件事同时成立，正是本坑的表征。

### 四、还有第二批被静默掉的输出

除了 `vin_log`，还有两处同源的空宏：

- `#define sensor_dbg(...)` —— `vin_log.h:110-115`，`DEV_DBG_EN` 写死 0；
- `/sys/kernel/debug/mpp/vi` 里 TDM/ISP 缓冲区那一段（`vin_core.c:930-950`）
  整个被 `#if IS_ENABLED(CONFIG_VIN_LOG)` 包住 —— **不是日志，是 debugfs 的正文**。
  所以那个「安全入口」输出的内容也比源码看起来少一截。

## 怎么确认的（两条独立证据）

单看 Kconfig 依赖链是「读代码推出来的」，容易再犯
`register-value-semantics-not-verified.md` 里那类错。所以做了第二条。

**证据一（推导）**：`Kconfig` → `DYNAMIC_DEBUG` → 板端 `.config` → 无 `VIN_LOG`
→ `vin_log.h:104` 空宏。链条完整，每步都可复核。

**证据二（观测）**：在仓库全部历史日志（`docs/`、`openspec/tasks/`、`openspec/pitfalls/`）
里对两种日志前缀做全量检索：

| 检索串 | 命中 |
| --- | --- |
| `[VIN_LOG_` （`vin_log` 的产物） | **0 条** |
| `tdm0 rx0 frame error` （`vin_err` 的产物） | 至少 8 处文档记录 |

积累了几个月的排查日志里，`vin_err` 的产物反复出现，
`vin_log` 的产物一条都没有。这与「宏被编译成空」完全一致，
与「日志等级没开」也一致 —— 但后者已被证据一排除（等级开关本身也是空的）。

**顺带解释了一件旧事**：`vin_supply.c:312-313` 里
`vin_log(VIN_LOG_POWER, "mclk%d set rate %ld, get rate %ld\n", ...)`
就是本仓库曾经反复寻找却没有的那行 MCLK 日志。
它不是没执行，是**没编进去**。

## 修法

**没有修。** 打开它需要重编内核（`CONFIG_DYNAMIC_DEBUG=y` 是内核级开关，
不是模块参数），成本远高于收益，且板端内核是 BSP 预编译包。

当前可行的替代手段按可靠性排序：

| 目的 | 手段 |
| --- | --- |
| 想知道某个函数有没有被调用 | **自己写只读内核探针**（`kprobe` / 直接调函数），不要指望 `dmesg` |
| 想知道某个寄存器的值 | 同上；但**先确认时钟/电源已开**（见 `mpp-mipi-debugfs-panics.md`） |
| 想知道链路拓扑与帧计数 | 读 `/sys/kernel/debug/mpp/vi`（安全，但内容见上文少了 TDM/ISP 段） |
| 想看驱动自己报的错 | `dmesg` 里的 `vin_err` 行仍然有效，可以放心依赖 |

写探针时**不要**去 echo `vin_log_mask` 试图开日志 —— 那是本项目已经踩过的时间黑洞。

## 边界

- 本结论绑定**这一个内核配置**（`5.15.147-99btf1-a733`）。
  换内核或在 `.config` 里打开 `CONFIG_DYNAMIC_DEBUG` 后，
  `VIN_LOG` 会恢复为 `default y`，`vin_log_mask` 立刻变得有意义
  （但默认 `0xeb9f` 仍然关着 ISP/STAT/VIDEO/TDM 四类，还得再 echo 一次）。
  复核方式：`grep -n "VIN_LOG" /path/to/.config`，有命中即说明本坑不成立。
- **「某条日志没出现」永远不能单独作为「那段代码没执行」的证据**，
  这条推论只有在**先确认该日志宏会被正常发射**的前提下才成立。
  本项目在这一条上已经错过不止一次。
- `%d`/`%ld` 之类的格式串错误、日志被 `dmesg_restrict` 过滤、
      printk 等级低于 `console_loglevel`，都会造成「日志没出现」但代码执行了。
     本坑只是其中一种成因，不能替代排查。

## 教训

**「没有日志」是三种完全不同的状态：没执行、执行了但没打印、打印了但被过滤。
把它们当成一件事，就会把「观测不到」误读成「不存在」。**

在依赖任何日志做判断之前，先做一次**阳性对照**：
找一条已知必然发生的日志，确认它在你的观测通道里确实可见。
本案的阳性对照就是 `vin_err` 的 `tdm0 rx0 frame error`——
它可见，说明通道通；而 `vin_log` 不可见，就是这个宏本身的问题，不是通道的问题。

没有阳性对照的「日志里没有」，不具备任何判别力。

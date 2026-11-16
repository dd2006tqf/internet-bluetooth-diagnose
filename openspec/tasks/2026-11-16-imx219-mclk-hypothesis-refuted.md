# IMX219 零帧：MCLK 假设被推翻，以及一次跨日期的取证误读

> 日期：2026-11-16
> 分类：探索 / 排查（**未解决，根因仍未定位**）
> 结果：**部分完成** —— 推翻了一个假设，纠正了一处取证错误；
> 摄像头零帧问题**仍然开放**。

## 构思

### 要解决什么

承接 `2026-11-15-camera-zero-frame-imx219.md`。那份记录结束时，
排查范围已缩小到「sensor 输出之后、SoC 接收之前」，所有可查的软件配置
都被逐项排除，剩下的方向都需要物理层手段。

本次要做的，是把最后一个**纯软件**假设验证掉：

> **假设**：`imx219.c:33` 把 MCLK 硬编码为 24 MHz，而板上 `csi-mclk`
> 实测是 26 MHz（父时钟 `dcxo`）。若属实，IMX219 的 PLL 分频表是按
> 24 MHz INCK 写死的，实际输入偏快 8.333%，全部 MIPI 时序跟着偏快，
> 落在 D-PHY 容差边缘 —— 能锁上、能出 `FRAME_END_SYNC`，但行/帧长度
> 解错，于是 `width is 0`、height 随机跳。

这个假设很吸引人，因为它能解释「为什么有时好有时坏」：校准运气每次开机不同。

### 为什么不做进 OpenSpec change

排查类工作，不产生生产代码变更，按 `openspec/tasks/README.md` 走本目录。

### 为什么选「运行时验证」而不是直接改

三条路径里：
- A 改设备树（切 `csi-mclk` 父时钟到 24 MHz 源）
- B 改驱动 `#define MCLK` 为 26 MHz + 重算 PLL 表
- C 先用运行时手段验证假设，再决定固化方案

选 C：最轻，重启即恢复，且**假设错了就不用背 A/B 的改动债**。

## 实现方式

### 为什么不能走 debugfs

先查了板端有没有可写的时钟接口：

```
/sys/kernel/debug/clk/csi-mclk/
  clk_accuracy      -r--r--r--
  clk_duty_cycle    -r--r--r--
  clk_enable_count  -r--r--r--
  clk_flags         -r--r--r--
  clk_max_rate      -r--r--r--
  clk_min_rate      -r--r--r--
  clk_notifier_count -r--r--r--
  clk_parent        -r--r--r--
  clk_phase         -r--r--r--
  clk_prepare_count -r--r--r--
  clk_protect_count -r--r--r--
  clk_rate          -r--r--r--
```

**全部只读**。`CONFIG_COMMON_CLK` 有，但没有可写的 per-clock 接口。
所以 `clk_set_rate` / `clk_set_parent` 无法从用户态调用。

### 改用一次性内核探针（只读）

宿主机已具备完整交叉编译条件：

| 条件 | 状态 |
| --- | --- |
| `aarch64-linux-gnu-gcc` | 13.3.0，可用 |
| 内核头文件缓存 | `~/.cache/weaknet-kmod-headers/5.15.147-99btf1-a733`（含 `Module.symvers`） |
| kbuild 宿主机工具 | `fixdep` / `modpost` 已是 x86-64 原生（`tools/kmod_cross_build.sh` 重建过） |
| `clk_set_rate` / `clk_set_parent` / `clk_get_rate` | `Module.symvers` 中均为 `EXPORT_SYMBOL_GPL` |

探针源码 `/tmp/probe_clk/probe_clk.c`（**一次性材料，未入库**）：

```c
/* 只读：clk_get / clk_get_rate / clk_get_parent / clk_hw_get_num_parents */
static const char *names[] = {
    "csi_mclk0", "csi_mclk0_24m", "csi_mclk0_pll",
    "csi_isp", "csi_isp_src", "csi_top", "csi_top_src",
};
dev = bus_find_device_by_name(&platform_bus_type, NULL, "5800800.vind");
```

关键点：**用 `bus_find_device_by_name` 拿到 `5800800.vind` 这个 platform
device，再 `clk_get(dev, name)`** —— 这和驱动 `vin.c:189` 干的完全一样，
拿到的是同一个时钟对象。绕开了所有猜测。

构建（`$(CURDIR)` 而非 `$(PWD)`，`PWD` 是环境变量会被 shell cwd 污染）：

```make
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
```

vermagic 校验通过：

```
vermagic=5.15.147-99btf1-a733 SMP preempt mod_unload aarch64
```

### 探针输出（决定性证据）

```
probe_clk: csi_mclk0        provider=csi-master0    rate=24000000   parent=sys24M
probe_clk:   possible_parents=7
probe_clk:     [0] sys24M           rate=24000000
probe_clk:     [1] pll-video0-4x    rate=704000000
probe_clk:     [2] pll-video0-3x    rate=704000000
probe_clk:     [3] pll-video1-4x    rate=1188000000
probe_clk:     [4] pll-video1-3x    rate=792000000
probe_clk:     [5] pll-video2-4x    rate=1188000000
probe_clk:     [6] pll-video2-3x    rate=792000000
probe_clk: csi_mclk0_24m    provider=sys24M         rate=24000000   parent=(none)
probe_clk: csi_mclk0_pll    provider=pll-video0-3x  rate=704000000  parent=pll-video0
```

## 结果

### 假设被推翻

**sensor 的实际 MCLK 是 24 MHz，与驱动宏完全一致。**

| 项 | 值 | 与 `#define MCLK (24*1000*1000)` |
| --- | --- | --- |
| `csi_mclk0` 速率 | 24000000 | ✅ 一致 |
| `csi_mclk0` 父时钟 | `sys24M`（24 MHz） | ✅ |
| `csi_mclk0` 提供者 | `csi-master0` | —— |
| 当前选中的父索引 | [0] `sys24M` | ✅ 就是 24M 那一档 |

驱动 `vin.c:186-205` 为每个 CCI 准备了三个句柄：

```c
sprintf(clk_name, "csi_mclk%d", i);       // → csi-master0 (mux)
sprintf(clk_name, "csi_mclk%d_24m", i);   // → sys24M   (24 MHz 固定源)
sprintf(clk_name, "csi_mclk%d_pll", i);   // → pll-video0-3x (704 MHz)
```

设备树 `vind@5800800` 的 `clock-names` 也确认这三项齐全。也就是说
**驱动本来就有 24M 分支并且当前就选在 24M 上**，不存在「被迫用了 26M」。

### 我是怎么错的（第 9 次判据错误）

我把 **`csi-mclk`（带横线）** 当成了 sensor 的 MCLK。

其实是两个完全不同的时钟对象：

| 时钟名 | 是什么 | 速率 |
| --- | --- | --- |
| `csi-mclk`（带横线） | CCU 里 CSI 控制器自己的模块门控时钟，父 `dcxo` | 26 MHz |
| `csi_mclk0`（下划线） | **sensor 引脚 MCLK 的 mux**，父 `sys24M` | 24 MHz |

两者的名字只差一个分隔符，速率差 8.3%，而我只查了前者就下了结论。

这和 `register-value-semantics-not-verified.md` 记的是同一型错误：
**读到的数值是真的，对数值语义的解释是错的**。那篇文档里已经写了「先回答
它的定义在哪个文件里」，我这次跳过了这一步 —— 因为 `csi-mclk` 在
`clk_summary` 里看起来完全像个 sensor 时钟。

### 更重要的一件事：我今天的失败叙事是错的

排查过程中我曾描述「两次 reboot 出现两种不同形态」：

> 13:23 那次：TDM 中断 110 次，全部 `frame error`
> 04:16 那次：TDM 中断 0，错误 0，彻底静默

**这个描述不成立。** 后来按日期全量统计：

```
$ sudo journalctl | grep -E "tdm0 rx" | awk '{print $1,$2}' | sort | uniq -c
  55771 Nov 11

$ sudo journalctl | grep "frame stats" | awk '{print $1,$2}' | sort | uniq -c
      9 Nov 16
```

| 日期 | `tdm0 rx` 错误 | `frame stats` |
| --- | --- | --- |
| 11-11 | **55771** | 0 |
| 11-15 | **0** | 0（服务当天只有 2 条日志） |
| 11-16 | **0** | 9（全部 `received=0`） |

那些 `width is 1869` / `hblank short` 的报错**全部属于 11-11**，
是 `sensor-mode-table-fps-lie` 里那批 `bin_factor=2` 坏模式的数据。
我把它们读成了 11-15 的现场。

同时 11-15 那 4 条「摄像头相关日志」经查是 **`sudo` 命令行里的字符串本身**
（我当天执行的 `grep` 命令被 journald 记下来，又被自己的 grep 匹配到），
不是任何应用日志 —— 即 `2026-11-15-camera-zero-frame-imx219.md` 里
已经记过的「grep 自匹配」陷阱，我第二次踩了。

**纠正后的真相**：从 11-15 到现在，TDM **一次都没有收到过数据**，
一直是完全静默。只有一种形态，不是两种。

这让问题反而变简单了：没有「有时好有时坏」的证据，
也就不必解释「校准运气」，只需要解释「数据为什么从未到达」。

### 仍然成立的部分

| 事实 | 证据 |
| --- | --- |
| sensor 在位且是 IMX219 | `MODEL_ID = 0x0219` |
| 模式寄存器与驱动表逐项一致 | 裸 I2C 比对（11-15 记录） |
| `sensor_s_stream on = 1` 返回成功 | kernel log |
| 7 跳拓扑全 ENABLED | 应用日志 `vin_video0 <- ... <- imx219` |
| sensor MCLK = 24 MHz | 本次探针（**新增，且推翻了旧怀疑**） |
| `csi_top` / `csi_isp` = 704 MHz | 探针 + `clk_summary` |
| 11-15 起 TDM 从未收到数据 | 全量 journal 按日期统计 |

### 遗留

1. **根因仍未定位。** 现在排除掉了「时钟配置错误」这整个方向，
   剩下的分界点是：**sensor 没发** 还是 **SoC 的 PHY 没锁上**。
2. **下一步手段已备好但未执行**：本次验证了「只读内核探针」这条路可行
   （编译、加载、打印、卸载全通）。可以照同样方式读 `phy@6b00000`
   的寄存器，看 lane 使能与 lock 位，从寄存器层面区分上面两种情况。
   **不能**走 `/sys/kernel/debug/mpp/mipi`，那个节点一读整板冻结。
3. **未做的低成本验证**：「重开一次就好」是否成立。本次服务只 open 了
   一次 video0，重启服务让它 open 第二次即可检验。
4. 探针源码留在 `/tmp/probe_clk/`，未入库，按一次性材料处理。

## 追加（同日，纯源码阶段，未动开发板）

MCLK 那条线关掉之后，又去源码里查了两件「为什么我们一直在摸黑」的
基础设施问题，结论各自成文：

| 新记录 | 一句话 |
| --- | --- |
| [`../pitfalls/vin-log-compiled-out.md`](../pitfalls/vin-log-compiled-out.md) | `CONFIG_VIN_LOG` 依赖 `DYNAMIC_DEBUG`，本板没开 → 整个 VIN 驱动的 `vin_log()` 编译成空语句，`vin_log_mask` 是空转开关 |
| [`../pitfalls/mpp-mipi-debugfs-panics.md`](../pitfalls/mpp-mipi-debugfs-panics.md) | `/mpp/mipi` 的致死点在 `open` 而不是 `read`，`open` 里直接 `readl` 已 gated 的 PHY 寄存器 → `timeout` 结构上无效 |

顺带证伪一条自己提的怀疑：`sunxi_mipi.c:1639` 的
`mipi_bps = 1000*dphy_clk*8/phy_freq_cnt` 不可能是致死原因 ——
ARM64 `UDIV` 除零返回 0，不陷入（交叉编译
`unsigned f(unsigned a,unsigned b){return a/b;}` 确认生成 `udiv w0,w0,w1`）。

这两篇都不改变「根因仍未定位」的状态，但它们解释了此前多轮排查为何
既看不到驱动内部日志、也拿不到 PHY 寄存器值。下一步手段不变：
自写只读内核探针读 `phy@6b00000`，**等开发板可用后执行**。

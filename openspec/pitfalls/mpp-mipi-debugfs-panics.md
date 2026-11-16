# `/sys/kernel/debug/mpp/mipi` 是 0444 只读的，但 `open` 就会打死整板

> 发现于：2026-11-15（真机卡死）/ 2026-11-16（源码定位到成因）
> 位置：`drivers/vin/vin-mipi/sunxi_mipi.c:1871`（`mipi_debugfs_open`）、
> `:1970`（`debugfs_create_file("mipi", 0444, ...)`）、
> `vin-mipi/combo_csi/combo_csi_reg.c`（22 处 `vin_reg_readl`）
> 影响：读一次 → 整板 panic → **永不自动重启**，只能物理断电。
> 而且 `timeout` 救不了，因为危险发生在 `open` 而不是 `read`。

## 现象

想看 MIPI PHY 到底有没有使能（`phy_en` / `data_lane_en` / HS 状态），
最直觉的动作是：

```bash
cat /sys/kernel/debug/mpp/mipi
```

结果：SSH 立即断开，`127.0.0.1:22222` 隧道消失，板子不再上线。
2026-11-15 那次据现场记录是「板子自己重启了（`uptime` 归零）」。

**试过 `timeout 5`，照样死。** 这条曾经被记成「必须单次读取 + 硬超时」，
2026-11-16 读完源码后判定：**那个「小心地读一次」的规则是错的，
正确规则是「完全不要碰」**（理由见下）。

## 成因

### 一、危险发生在 `open`，不在 `read`

这个节点的 `file_operations`：

```c
/* sunxi_mipi.c:1947-1953 */
static const struct file_operations mipi_debugfs_fops = {
	.owner   = THIS_MODULE,
	.open    = mipi_debugfs_open,     /* ← 全部寄存器读取在这里 */
	.llseek  = no_llseek,
	.read    = mipi_debugfs_read,     /* ← 只是从 kmalloc 缓冲拷出去 */
	.release = mipi_debugfs_release,
};
```

`mipi_debugfs_open()`（`sunxi_mipi.c:1871`）在**打开文件的那一刻**就把
所有寄存器读进内核缓冲区了：

```c
mipi_status_size_sum += phy_common_status_dump(...);      /* 读链路模式寄存器 */
for (i = 0; i < VIN_MAX_MIPI; i++) {
	mipi = glb_mipi[i];
	if (mipi != NULL)
		mipi_status_size[i] = phy_digital_status_dump(mipi, ...);   /* 读 PHY 数字层 */
}
for (i = 0; i < VIN_MAX_MIPI; i++) {
	mipi = glb_mipi[i];
	if (mipi != NULL)
		mipi_status_size[i] = port_payload_status_dump(mipi, ...);  /* 读 PORT 层 */
}
```

`read()` 只是 `simple_read_from_buffer`，拿的是已经算好的内存。

**这就是 `timeout` 无效的原因**：`timeout` 能中断的是用户态 `read()` 等待，
而 `open()` 里的 MMIO 读写是在内核里同步发生的，没有任何「等待」可供打断。
一旦那几条 `readl()` 触发异常，进程已经不存在了，谈不上超时。

### 二、`open` 路径里没有任何时钟/电源保护

对整个 `sunxi_mipi.c` 检索：

```
$ grep -n "clk_prepare_enable\|clk_disable_unprepare\|pm_runtime\|reset_control" sunxi_mipi.c
(无输出)
```

**这个文件里根本不存在这几个符号。** 也就是说 `mipi_debugfs_open()`
在读 PHY 寄存器之前，既没有把 CSI/MIPI 时钟打开，也没有做 `pm_runtime_get`，
更没有解复位。

读的地址来自 probe 时保存的裸 `of_iomap` 地址：

```c
/* sunxi_mipi.c:1470-1487 */
mipi->base      = of_iomap(np, 0);
mipi->port_base = of_iomap(np, 1);
cmb_csi_set_phy_base_addr(mipi->id,   (unsigned long)mipi->base);
cmb_csi_set_port_base_addr(mipi->id,  (unsigned long)mipi->port_base);
```

之后每次读都走：

```c
/* combo_csi_reg.c:189 */
unsigned int cmb_phy_en_status_get(unsigned int sel)
{
	unsigned int reg_val = vin_reg_readl(cmb_csi_phy_base_addr[sel] + CMB_PHY_CTL_REG_OFF);
	...
}

/* utility/vin_io.h:39 */
static inline u32 vin_reg_readl(volatile void __iomem *addr)  { return readl(addr); }
```

`combo_csi_reg.c` 里这样的 `vin_reg_readl` 有 **22 处**，
`phy_digital_status_dump` / `port_payload_status_dump` 会把它们多数走一遍。

### 三、稳态下这些时钟是关着的

CSI/MIPI 时钟的开合只被 `vin_md_set_power()` 管着：

```c
/* vin.c:795 */
static void vin_md_set_power(struct vin_md *vind, int on)
{
	...
	vin_md_clk_enable(vind);   /* :820 */
	...
	vin_md_clk_disable(vind);  /* :884 */
}
```

它的调用者只有流式路径：

```c
/* vin.c:1266   __vin_pipeline_open() */
vin_md_set_power(vind, 1);
```

而 probe 是**开完就关**：

```c
/* vin.c:2551 / 2565   vin_probe() */
vin_md_clk_enable(vind);
... 注册全部子设备 ...
vin_md_clk_disable(vind);
```

`vin_md_clk_disable()` 打印前先 `if (!vind->clk_en) return;`（`vin.c:546`），
所以关得干净。

**结论：只要当前没有在跑 `STREAMON`，CSI/MIPI 的时钟就是 gated 的。
此时读那几个寄存器 = 读已断电模块的 MMIO。**

### 四、为什么是「永不重启」而不是「重启一下就好」

板端内核配置：

```
# .config:8385-8387
CONFIG_PANIC_ON_OOPS=y
CONFIG_PANIC_ON_OOPS_VALUE=1
CONFIG_PANIC_TIMEOUT=0
```

访问关电模块的寄存器在 ARM64 上产生同步外部中止（SError）→ oops →
`panic_on_oops=1` → panic。而 `panic_timeout=0` 表示**panic 后不自动重启**，
就停在那里。

这与现场观察完全吻合：SSH 掉线、隧道不重连、板子既不回来也不重启，
必须拔电。2026-11-10 的计划里还写着「单次读取 + 60s 硬超时」，
按这个机制，那 60 秒不会有任何作用。

### 五、别指望「提前 bail out」

`phy_digital_status_dump()` 里确实有这么一段：

```c
/* sunxi_mipi.c:1598-1602 */
phy_en_status = cmb_phy_en_status_get(mipi->id);
if (!phy_en_status) {
	count += scnprintf(...);
	return count;          /* phy 没开就提前返回 */
}
```

看起来「PHY 没开时会安全退出」。但**`cmb_phy_en_status_get()` 本身就是那条
致命的 `readl`**。提前返回发生在第一次读之后，救不了任何东西。

### 一条被证伪的附带怀疑（记下来免得重查）

`sunxi_mipi.c:1639`：

```c
mipi_bps = 1000 * dphy_clk * 8 / phy_freq_cnt;
```

曾怀疑 `phy_freq_cnt` 为 0 会除零 oops。**证伪**：ARM64 的 `UDIV`
除零不陷入，直接返回 0。用
`unsigned f(unsigned a, unsigned b){ return a / b; }` 交叉编译确认生成为
`udiv w0, w0, w1`。所以这一行不是致死原因，不要再去追。

## 修法

**不修，禁用。**

### 唯一安全的状态观测入口：`/sys/kernel/debug/mpp/vi`

```c
/* vin_core.c:973   vin_debugfs_open() → vin_status_dump()（:637） */
```

对 `vin_status_dump()` 整个函数体（637~971）检索：

| 检索串 | 命中 |
| --- | --- |
| `readl` / `writel` / `reg_read` / `reg_write` | **0** |
| `vin_reg_` | **0** |
| 任意 `*_get(` | **0** |
| `clk_get_rate` | 2（`:864`、`:865`，只读时钟树不做 MMIO） |

`clk_get_rate()` 是纯软件查表，不碰硬件。所以这个节点历史上读了很多次都没事。
它给出链路拓扑（`vi0: imx219 => mipi1 => csi1 => tdm_rx0 => isp0 => vipp0`）、
CSI/ISP 时钟频率、`frame => cnt/lost_cnt/error_cnt` 等。

**注意**它输出得比源码看起来少：TDM/ISP 缓冲区那两段被
`#if IS_ENABLED(CONFIG_VIN_LOG)` 包住（`vin_core.c:930-950`），
本板没编进去，见 `vin-log-compiled-out.md`。

### 但 `/mpp/vi` 也有自己的雷：无判空的 sensor 解引用

```c
/* vin_core.c:874 / 878 / 883 / 890 / 894 / 899 —— 六处，全部没有 NULL 检查 */
vinc->vid_cap.pipe.sd[VIN_IND_SENSOR]->name,
/* :932 同一函数里这里却有检查 */
if (vinc->vid_cap.pipe.sd[VIN_IND_TDM_RX]) { ... }
```

`pipe.sd[VIN_IND_SENSOR]` 只由 `vin_md_prepare_pipeline()` 填充
（`vin.c:83`，赋值在 `vin.c:119`），而它只在
`__vin_pipeline_open(prepare=true)`（`vin.c:1259`）时被调用，
触发链是 `__vin_s_input()` → `vin_pipeline_call(..., open, &cap->pipe, ..., true)`
（`vin_video.c:2412`）。

`vin_debugfs_open()` 会**遍历全部 `VIN_MAX_DEV`（=18）个 vin_core**。
所以只要其中**任意一个** vin_core 从没收到过 `VIDIOC_S_INPUT`，
读 `/mpp/vi` 就可能空指针解引用 → 同样 oops → 同样 panic 且不重启。

实务判断：只有在「确认所有注册的 vin_core 都已 `S_INPUT`」
（典型情况：应用刚成功打开了 camera 并跑起来）时才读它。

### 想拿 PHY 寄存器值怎么办

只能自己写只读内核模块，而且要满足三条：

1. 先 `clk_get()` + `clk_prepare_enable()` 把 CSI/MIPI 时钟打开，
   `readl()` 之后再关回去；
2. 地址从 `bus_find_device_by_name` / `of_iomap` 正经取，**不要猜 MMIO 偏移**；
3. 读完 `rmmod`，不留在树上。

这条路径已经在 `openspec/tasks/2026-11-16-imx219-mclk-hypothesis-refuted.md`
里被验证可行（那次是读时钟，同样方式可读寄存器），但**尚未真正执行**。

## 边界

- 「读 `/mpp/mipi` 必死」这条结论来自**一次真机事故 + 一份源码分析**，
  不是受控实验。理论上如果恰好在 `STREAMON` 期间读，时钟是开的，
  也许不会立刻 panic。**但不建议用这个「也许」去试** ——
  一旦判断错，代价是物理断电一次，而收益只是确认一条已知不能用的路径。
- 上面「时钟被 gate」的推理基于 `vin_md_set_power` 是唯一的开关点。
  如果 BSP 里还有别的地方（比如 MPP 模块、`pm_runtime` 回调）
  偷偷把 CSI 时钟打开，那么「稳态一定 gated」就不成立。
  没有去穷举验证这一点，所以措辞是「steady state 下 gated」而不是「永远 gated」。
- `/mpp/vi` 的「零 MMIO」是对**当前这份源码**静态检索的结论。
  驱动升级后 22 处 `vin_reg_readl` 或 `vin_status_dump` 都可能变化。
  复核方式：`sed -n '637,971p' vin_core.c | grep -E "readl|vin_reg_"`，
  有命中即说明本结论失效。
- 同样地，`/mpp/vi` 对全 18 个 vin_core 的遍历意味着**它的安全性取决于
  整机状态**，不只取决于你想看的那个设备。这是一条容易被忽略的耦合。

## 教训

**「只读权限 0444」描述的是文件语义，不是内核行为。**
`0444` 只保证你写不进去；它完全不保证读它是安全的。
真正的安全边界在 `open()` 里做了什么事 —— 而 `open()` 可以做任意 IO。

更具体的一条：**「危险在 open 还是 read」决定了防具是什么。**
危险在 `read`（比如无界拷贝），可以用 `timeout` / `head -c` 兜住；
危险在 `open`，任何用户态的防护都无效，只有「别打开」这一条路。

所以在嵌入式真机上，一条只读命令的风险评估应该问的是：
**它 open 的时候，内核会替我去碰哪些已经关掉电的硬件？**

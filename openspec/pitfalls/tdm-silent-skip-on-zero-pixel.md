# TDM 有一处静默 `return 0`：条件隐蔽、无日志、整条调用链不报错

> 发现于：2026-11-16（纯源码定位）
> 位置：`drivers/vin/vin-tdm/vin_tdm.c:1095-1096`
> 影响：`VIDIOC_STREAMON` 返回 0、`s_stream` 返回 0、无任何日志，
> 但 TDM 的接收通道与中断一次都没打开 —— 零帧 + 零错误 + 零中断。
>
> **重要限定（2026-11-16 同日订正）**：本板的实测日志证明
> `work_mode == TDM_OFFLINE`，因此**本板没有命中这条分支**，
> 它不是本板 IMX219 零帧的原因。本文记录的是一个**仍然存在于源码里的
> 潜在缺陷**，以及「怎么判断你的板会不会撞上」。限定依据见「成因·四」。

## 现象

如果撞上这条分支，观测到的形态是：

| 检查项 | 结果 |
| --- | --- |
| `VIDIOC_STREAMON` | 返回 0 |
| 驱动 `s_stream` | 返回 0 |
| 应用层错误 | 无 |
| 驱动报错 | 无 |
| `5908000.tdm` 中断计数 | 恒定不动（一次都不进） |
| 应用层 `received` | 恒为 0 |

关键不是「零帧」——是**零帧 + 零错误 + 零中断**。数据没到、或到了但解析
失败，都会持续刷 `vin_err`；而这条路径是内核完全安静的。

## 成因

### 一、静默返回点

```c
/* vin_tdm.c:1042-1097  摘录，TDM_V200 分支 */
} else {                                  /* 非 2F/3F WDR，即普通线性模式 */
    if (tdm->work_mode == TDM_OFFLINE) {
        ...                                /* offline：完整配置，不返回 */
    } else {                               /* ← 只在 TDM_ONLINE 进入 */
        tdm->ws.tdm_en = 1;
        ...
        tdm_rx->ws.tx_func_en = 0;
        tdm_rx->ws.pkg_en     = 0;
        tdm_rx->ws.lbc_en     = 0;
        tdm_rx->ws.sync_en    = 0;
        if (!tdm->ws.speed_dn_en)
            return 0;                      /* ★ :1095-1096 */
    }
}

/*reg int*/
sunxi_tdm_top_s_stream(tdm_rx, enable);    /* :1101 中断使能在这里 */
tdm_set_rx_data_rate(tdm_rx, enable);      /* :1102 */
tdm->tdm_rx_reset[tdm_rx->id] = 0;         /* :1103 */
tdm_set_rx_cfg(tdm_rx, 1);                 /* :1104 分配 DDR 缓冲 + 写地址 */
csic_tdm_rx_enable(tdm->id, tdm_rx->id);   /* :1105 接收通道使能 */
csic_tdm_rx_cap_enable(tdm->id, tdm_rx->id);/* :1106 */
```

返回的是 `0` 而不是 `-EINVAL`，于是
`sunxi_tdm_subdev_s_stream` → `__vin_subdev_set_stream` →
`__vin_pipeline_s_stream` → `vidioc_streamon` **全线返回成功**。

而 `csic_tdm_rx_enable()` 从未被调用 ⇒ 接收通道不开 ⇒ 一个中断都不会产生。
这解释了「中断计数恒定不动」。

`if (!speed_dn_en)` 上面本来有一行 `vin_log(VIN_LOG_FMT, ...)` 类的输出，
但 `CONFIG_VIN_LOG` 在本板被编译掉（见 `vin-log-compiled-out.md`），
所以连「我跳过了」这句话都不存在。

### 二、`speed_dn_en` 的默认值是 0，且只有一个私有 ioctl 能改

全树 `speed_dn_en` 的写入点只有两处，都在同一个函数的两份拷贝里：

```c
/* vin_video.c:3040-3075  vidioc_set_tdm_speeddn_cfg() */
tdm->ws.speed_dn_en = cfg->tdm_speed_down_en;   /* :3065 */

/* vin_video.c:5097-5136  vin_set_tdm_speeddn_cfg_special()（EXPORT_SYMBOL_GPL）*/
tdm->ws.speed_dn_en = cfg->tdm_speed_down_en;   /* :5124 */
```

用户态入口是厂商私有 ioctl：

```c
/* vin_test/sunxi_camera_v2.h:359-360 */
#define VIDIOC_SET_TDM_SPEEDDN_CFG \
    _IOWR('V', BASE_VIDIOC_PRIVATE + 18, struct tdm_speeddn_cfg)
```

`tdm` 由 `kzalloc` 分配（`:2485`），DT 只解析 `device_id` / `work_mode` /
`delay_init` 三个键，**不解析任何 speed_dn 相关属性**。
所以不调这个 ioctl，`speed_dn_en` 恒为 0。

顺带一条：驱动自己在 `sunxi_tdm_top_s_stream()` 里本来有一段
「OFFLINE 就自动打开 speed_dn」的代码，**被注释掉了**：

```c
/* vin_tdm.c:653-655 */
/*		if (tdm->work_mode == TDM_OFFLINE)
			tdm->ws.speed_dn_en = 1;
*/
csic_tdm_set_speed_dn(tdm->id, tdm->ws.speed_dn_en);
```

### 三、`work_mode` 的缺省值是 OFFLINE，撞上 ONLINE 需要 DT 显式写 0

```c
/* vin_tdm.c:2501-2504 */
tdm->work_mode = 0xff;
of_property_read_u32(np, "work_mode", &tdm->work_mode);
tdm->work_mode = clamp_t(unsigned int, tdm->work_mode, TDM_ONLINE, TDM_OFFLINE);
```

```c
/* vin-tdm/tdm200/tdm200_reg.h:131-134 */
enum tdm_work_mode { TDM_ONLINE = 0, TDM_OFFLINE = 1 };
```

DT 没写 → 保持 `0xff` → `clamp_t(0xff, 0, 1)` = **1 = TDM_OFFLINE**（安全支）。
只有 DT 显式 `work_mode = <0>` 才落到 ONLINE。

**致死条件是两个同时成立**：

| `work_mode` | `speed_dn_en` | `:1095` 是否返回 | TDM 实际行为 |
| --- | --- | --- | --- |
| 1 (OFFLINE) | 任意 | 不返回 | 完整配置 |
| 0 (ONLINE) | 1 | 不返回 | 完整配置 |
| **0 (ONLINE)** | **0** | **返回 0** | **什么都不配，零中断，零报错** |

### 四、怎么判断一块板有没有撞上（本板的答案：没有）

三条判据，按可靠性排序：

**判据 A（决定性，本板已用）**：`dmesg`/journal 里有没有

```
__sunxi_tdm_reset_v2_handle:workqueue tdm_rx0 reset!!!, isp frame count is 0
```

这条打印在 `__sunxi_tdm_reset_v2_handle()`（`:1746`）的循环体内（`:1814`），
而该循环体前面有三道 `continue` 守卫（`:1761-1767`），其中
`if (!tdm->tdm_rx_reset[...]) continue;` 依赖的数组**只在
`tdm_rx_set_reset()`（`:2091`）里被置 1**，而那个函数开头就是

```c
/* vin_tdm.c:2091-2095 */
static void tdm_rx_set_reset(struct tdm_dev *tdm, unsigned int rx_id)
{
#ifdef TDM_V200
	if (tdm->work_mode == TDM_ONLINE)
		return;          /* ← ONLINE 时永不置位 */
```

**所以这条打印只可能出现在 TDM_OFFLINE。** 本板 11-15 / 11-16 的日志里
有它 ⇒ **本板是 OFFLINE ⇒ `:1095` 不会执行**。（这条推理此前被写反过，
教训另见 [`two-similarly-named-reset-functions.md`](two-similarly-named-reset-functions.md)。）

**判据 B（正向对照）**：`tdm_rx0 vb is N%` 与
`wh_fps ... isp_need_clk is ...` 这两行是 `vin_print`（不受 `CONFIG_VIN_LOG`
影响，必然可见），但它们只在 `tdm_set_rx_data_rate()` 跑过 `:776` 那道
`if (work_mode == TDM_ONLINE || !speed_dn_en) return;` 之后才会打。
出现 ⇒ 必然是 OFFLINE 且 `speed_dn_en == 1`。
**注意只能正向用**：不出现什么都不能证明（OFFLINE + `speed_dn_en == 0` 同样不打）。

**判据 C（直接读，最省事）**：

```bash
ssh board 'ls -d /proc/device-tree/soc*/tdm* 2>/dev/null; \
           for f in /proc/device-tree/soc*/tdm*/work_mode; do \
             [ -e "$f" ] && echo "$f = $(hexdump -e "1/4 \"%u\"" "$f")"; done'
```

属性不存在 = OFFLINE（clamp 缺省）；读出 0 = ONLINE（此时才需要担心本文）。
纯只读，不碰任何 MMIO。

## 修法

**如果你是撞上的那一方**，两种选择：

1. **补私有 ioctl**（最小改动）。在 `S_FMT` 之后、`STREAMON` 之前
   （源码注释 `vin_video.c:3039` 明确要求这个顺序）调用一次
   `VIDIOC_SET_TDM_SPEEDDN_CFG`，`tdm_speed_down_en = 1`。
   结构体见 `sunxi_camera_v2.h:281-286`，需把定义搬进本仓库，
   不能依赖 BSP 头文件。
   **必须先探测**：`ioctl` 返回 `-ENOTTY` 时静默跳过 —— 标准 UVC 摄像头
   本来就没有这个 ioctl，不能当致命错误。
2. **改 DT** 把 `work_mode` 设成 1（OFFLINE）或在 BSP 里删掉那个
   `if (!speed_dn_en) return 0;`。前者动设备树，后者动内核，
   都比方案 1 重。

**本板不需要修**（未命中）。

## 边界

- 本文的条件推理全部来自源码；**本板 `work_mode` 的实际取值尚未直接读取**
  （判据 C 没跑过）。判据 A 用的是日志反推，依赖「板端 `vin_tdm.c` 与本
  仓库 `bsp-vin/` 副本一致」这一前提（该副本来自同分支，但 `vin_tdm.c`
  不在早期逐字核对过的那 7 份文件里）。
- 同一文件里还有另外两处静默 `return 0`：
  - `:921` 输出格式是 RAW（`res->res_pix_fmt` 命中 `V4L2_PIX_FMT_S*`）时直接返回。
    产品走 NV12，不命中，但**任何切到 raw 采集的调试程序都会命中**。
  - `:928-930` / `:933-936` WDR 模式或通道模式检查失败时返回 0，
    虽然带 `vin_err`，但错误码仍是 0，上层同样看不见。
  查零帧时**三处都要查，别只查一处**。
- `speed_dn_en` 在 OFFLINE 下的真实作用本文没有展开。已知的相邻事实是
  `tdm_set_rx_data_rate()` 在 `!speed_dn_en` 时整函数早退（`:776`），
  于是 `tdm->tx_cfg.valid_num` 保持 0，`csic_tdm_set_tx_data_rate()`
  不会被调用。这对 OFFLINE 的 ISP 取数有没有影响，**未验证**。

## 教训

**`if (!x) return 0;` 是一种极难排查的失败方式。**
它同时抹掉了三样东西：错误码（上层无法反应）、日志（人无法察觉）、
以及「有没有碰过硬件」这条信息（事后无法从状态反推）。

对任何「启动成功」的判断，都要追问：**这个 0 是谁返回的，它返回前
真的碰过硬件吗？** 唯一可靠的判别是找一个**只可能由「真的做了」产生的
硬件侧痕迹** —— 本案是中断计数。中断不动，就等于没做。

（省下的那行 `vin_err`，代价是排查时多花几天。）

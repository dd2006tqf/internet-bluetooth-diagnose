# sunxi-vin BSP 源码参考副本

> 来源：`radxa/allwinner-bsp` 分支 `cubie-aiot-v1.4.6`（与板端内核
> `5.15.147-99btf1-a733` 对应的 VIN 驱动）
> 用途：**只读参考**。逐行对照板端 dmesg 与 ioctl 行为，定位摄像头链路根因。
> 这些文件**不参与编译**，也不属于任何构建目标。

## 为什么要有这个目录

摄像头 bring-up 的每一次结论都必须能回到源码行。板端 dmesg 给出的
只有 `EINVAL` / `width is 1869` 这类结果，没有源码就无法判断那是
「参数不对」还是「驱动缺陷」。2026-11-10 ~ 11-11 的多轮排查里，
正是因为能逐行读这些文件，才把三条错误结论（sensor 未上电、
PHY 无时钟、lane 数错）逐一推翻。

## 文件清单

| 文件 | 关键内容 |
| --- | --- |
| `imx219.c` | sensor 驱动：`sensor_win_sizes[]`、寄存器表、`sensor_power()` 上电时序、`sensor_g_mbus_config()`（硬编码 2-lane） |
| `sensor_helper.c` | `sensor_isp_input()`（唯一写 `width_input`）、`sensor_cfg_req()`（`large_image==3` 分支）、`reserved[0..2]` 落点 |
| `sensor_helper.h` | `struct sensor_info` / `struct sensor_win_size` 定义 |
| `vin.c` | media device 构链逻辑（`vinc->dma_merge_mode` 分支） |
| `vin_core.c` | vin_core 公共逻辑 |
| `vin_video.c` | `__vin_s_input`、`__vin_s_parm`（**强制 `reserved[2]=3`** 处）、`vidioc_s_parm` |
| `vin_video.h` | `struct vin_core` / `vin_vid_cap` 定义 |

> **2026-11-16 更新：完整的 `drivers/vin` 子树已入库，见 `../bsp-vin/`（363 个文件）。**
>
> 本目录仍是「精读摘录」：每份文件都带「为什么读它」的说明。
> 但**需要查任何不被摘录覆盖的定义时，直接去 `../bsp-vin/` 找**，
> 不要再靠猜或从 `/tmp` 临时副本翻。
>
> 起因：2026-11-16 排查 MIPI PHY 状态时，`CMB_PHY_CTL_REG_OFF` 等寄存器
> 偏移定义只存在于 `combo_csi_reg_i.h`，而它既不在本目录、也不在板端内核
> 头文件包里，全盘搜不到 → 寄存器读不了，排查硬阻塞。同一次还缺
> `vin_set_mclk_freq()` 实现，无法确认 skill MCLK 选源逻辑。
> 补齐整棵子树后两个缺口同时消失。
>
> 已核对：本目录 7 份 `.c`/`.h` 与 `../bsp-vin/` 中的同名文件**逐字一致**，
> 因此下文所有结论的代码锚点仍然有效。

另有一批早期散落的临时副本（`vin_tdm.c`、`tdm200_reg.c`、`sunxi_csi.c` 等）
曾放在 `/tmp/bsp-extra/`。**现在不必再用了** —— `../bsp-vin/` 是同一分支的
完整原文，`vin-tdm/`、`vin-mipi/`、`vin-csi/` 等目录各就各位。

## 已从这些源码得到的关键结论

1. **必须调 `VIDIOC_S_INPUT`**：`pipe.sd[VIN_IND_SENSOR]` 的唯一填充入口是
   `vin_video.c` 的 `__vin_s_input`；不调它，`S_FMT` 必 `EINVAL`。
2. **私有参数走 `S_PARM.reserved[]`**：`reserved[0]` = `use_current_win`
   （en-awisp）、`reserved[1]` = `isp_wdr_mode`、`reserved[2]` = `large_image`
   （en-largemode）。
3. **`dma_merge_mode == 1` 会强制覆盖 `reserved[2] = 3`**
   （`vin_video.c:2569`），即使应用填 0 也无效。这是目前唯一已定位的
   「把宽度改小」机制，与 TDM 实测 `1869x1063 ≠ 1920x1080` 相关。
4. **TDM 的 `frame error` 两个数是硬件寄存器原值**，不是软件几何：
   `csic_tdm_rx_get_size()` 读 `TDM_RX_FRAME_ERR_REG_OFF`。

详细推导见 `../sunxi-vin-摄像头链路根因分析.md`。

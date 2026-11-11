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

另有一批未收进本目录、放在 `/tmp/bsp-extra/` 的分册
（`vin_tdm.c`、`tdm200_reg.c`、`sunxi_mipi.c`、`sunxi_csi.c`、
`combo_csi_reg.c`、`dphy_reg.c` 等）。它们是**临时副本，不进仓库**
——需要时重新从上游取。

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

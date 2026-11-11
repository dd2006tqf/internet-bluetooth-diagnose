# sunxi-vin 摄像头链路根因分析（Radxa Cubie A7A / IMX219）

> 结论日期：2026-11-10（2026-11-11 更新）
> 依据：`radxa/allwinner-bsp` 分支 `cubie-aiot-v1.4.6` 的 VIN 驱动源码
> （本地副本在 `vin-references/`）+ 板端实测探针结果
> 状态：**第一层根因已修复并真机验证通过；第二层卡点（TDM 几何不匹配）已从
> 「零帧」收敛到「TDM 实测 1869x1063 ≠ 协商 1920x1080」，根因尚未证实。**
> 官方 `gst-launch` 命令在本板同样拿不到帧。详见第七节。

## 一句话结论

`VIDIOC_S_FMT` / `VIDIOC_ENUM_FRAMESIZES` 在 sunxi-vin 上失败的根因**不是**
DTB/overlay、**不是** sensor 未上电、**不是** 格式不匹配，而是：

> **应用必须在 `S_FMT` 之前先调用 `VIDIOC_S_INPUT`。**
> 该 ioctl 是 sunxi-vin 里**唯一**会填充 `vid_cap.pipe.sd[]` 的入口。
> 不调用它，`pipe.sd[VIN_IND_SENSOR]` 恒为 `NULL`，
> 于是 `S_FMT` 在 `vin_pipeline_try_format()` 的空指针检查处返回 `EINVAL`，
> `ENUM_FRAMESIZES` 在 `vidioc_enum_framesizes()` 里返回 `-1`（= 用户态看到的 `EPERM`）。

这条结论**推翻了此前写在
`openspec/tasks/2026-11-10-v4l2-real-board-bringup-and-tunnel-incident.md`
里的三条判断**（DTB 缺定义、sensor 未上电、NV12 与 RG10 格式不匹配）。
订正明细见该文件末尾。

## 二、为什么是 `S_INPUT`：逐行证据

### 2.1 `pipe.sd[]` 只有两个填充入口，其中只有一个可由用户态触发

`drivers/vin/vin.c:83` 定义了填充函数：

```c
static void vin_md_prepare_pipeline(struct vin_pipeline *p,
                                    struct media_entity *me)
```

它在 `drivers/vin/vin.c:119` 处写入 sensor：

```c
			p->sd[VIN_IND_SENSOR] = sd;
```

全仓检索 `vin_md_prepare_pipeline` 的调用点，只有两处：

| 位置 | 触发条件 | 用户态能否触发 |
| --- | --- | --- |
| `vin.c:2268` | `vin_setup_default_links()`，在 `vin_probe()` 结尾执行一次 | 否（probe 期，走的是 `&vinc->vid_cap.vdev.entity`） |
| `vin.c:1259` | `__vin_pipeline_open(p, me, prepare=true)` | 是 |

`__vin_pipeline_open` 的调用点见 2.3。关键在于：`vin.c:2268` 那次调用发生在
**probe 期**，此时 `vin_create_media_links()` 已经把 sensor→mipi 的链路建好了，
但——见 2.2——那条链路是 **disabled**，所以 probe 期那次填充**填不出 sensor**。

### 2.2 sensor→mipi 链路的建链标志位是 `0`，天生 disabled

`drivers/vin/vin.c:2035`：

```c
	ret = media_create_pad_link(source, SENSOR_PAD_SOURCE, sink, 0, 0);
	                                                          ^  ^
	                                                          |  └─ flags = 0 → disabled
	                                                          └──── sink pad
```

对比同一函数里其它链路，**全都带 `MEDIA_LNK_FL_ENABLED`**：

| 链路 | 源码行 | flags |
| --- | --- | --- |
| `mipi → csi` | `vin.c:2086-2088` | `MEDIA_LNK_FL_ENABLED` |
| `scaler → vin_cap` | `vin.c:2119-2121` | `MEDIA_LNK_FL_ENABLED` |
| `vin_cap → vin_video` | `vin.c:2153-2154` | `MEDIA_LNK_FL_ENABLED` |
| `isp → stat` | `vin.c:2207-2208` | `IMMUTABLE \| ENABLED` |
| **`sensor → mipi`** | **`vin.c:2035`** | **`0`（disabled）** |

这与真机 media 拓扑完全吻合：唯一 4 条 `disabled` 的链路正是
`imx219[0] → sunxi_mipi.1[0]`，其余主干链路全绿。

**所以 sensor→mipi 是靠运行时 `MEDIA_IOC_SETUP_LINK` 才打开的**，而这个动作
由 `__vin_sensor_setup_link()` 完成（`vin_video.c:1982`），它的调用点全部在
`__vin_s_input()` / `vidioc_streamon()` 里。

### 2.3 `vin_md_prepare_pipeline` 靠 `media_pad_remote_pad_first()` 逐跳上溯

`vin_md_prepare_pipeline` 的循环体（`vin.c:92-146`）：

```c
	while (1) {
		struct media_pad *pad = NULL;

		/* Find remote source pad */
		for (i = 0; i < me->num_pads; i++) {
			struct media_pad *spad = &me->pads[i];

			if (!(spad->flags & MEDIA_PAD_FL_SINK))
				continue;
			pad = media_pad_remote_pad_first(spad);
			if (pad)
				break;
		}

		if (pad == NULL)
			break;          /* ← 断在这里就是 NULL，整条链不再上溯 */

		sd = media_entity_to_v4l2_subdev(pad->entity);
		...
		case VIN_GRP_ID_SENSOR:
			p->sd[VIN_IND_SENSOR] = sd;
			break;
		...
		me = &sd->entity;
		if (me->num_pads == 1)
			break;
	}
```

`media_pad_remote_pad_first()` 语义：**只在 ENABLED 的 link 上找对端 pad**，
找不到（链路 disabled）返回 `NULL`。

于是 probe 期（sensor→mipi 仍是 disabled）：
`vin_cap` 的 sink pad 上溯到 scaler 成功，一路往上到 csi / mipi 都成功，
**到 mipi 的 sink pad 时，因为 sensor→mipi 是 disabled，`pad == NULL`，循环 break**。
结果 `p->sd[VIN_IND_SENSOR]` **保持 NULL**。

这就是 `pipe.sd[VIN_IND_SENSOR]` 为 NULL 的**直接机制**。

### 2.4 `S_FMT` 在空指针检查处返回 `EINVAL`

`vin_video.c:806-821`：

```c
static int vin_pipeline_try_format(struct vin_core *vinc,
				    struct v4l2_mbus_framefmt *tfmt,
				    struct vin_fmt **fmt_id,
				    bool set)
{
	struct v4l2_subdev *sd = vinc->vid_cap.pipe.sd[VIN_IND_SENSOR];   /* ← 811 */
	...
	if (WARN_ON(!sd || !tfmt || !fmt_id))                            /* ← 820 */
		return -EINVAL;
```

**第 820 行是 `WARN_ON(!sd || ...)`，即空指针检查**，不是格式比较。
板端 dmesg 抓到的那条 WARNING 精确指向
`vin_video.c:820 vin_pipeline_try_format.constprop.0+0x38`，
所以它报警的原因就是 **`sd == NULL`**。

调用链：`vin_video.c:1095`（`__vin_set_fmt`）→
`vin_pipeline_try_format()` → 第 820 行 `WARN_ON` → `return -EINVAL` →
`vin_err("vin_pipeline_try_format failed")` → `vin_err("set fmt error")` →
用户态拿到 `EINVAL(22)`。

此前把寄存器里的 `x11=0x3231564E`（"NV12"）与 `x4=0x30314752`（"RG10"）
解释成「格式不匹配」是**过度解读**：那两个寄存器只是 WARN 现场的残留值，
WARN 的判据是 `!sd`，与格式无关。

### 2.5 `ENUM_FRAMESIZES` 返回 `-1` → 用户态 `EPERM`，这是自洽的交叉验证

`vin_video.c:727-752`：

```c
static int vidioc_enum_framesizes(struct file *file, void *fh,
				  struct v4l2_frmsizeenum *fsize)
{
	...
	ret = v4l2_subdev_call(vinc->vid_cap.pipe.sd[VIN_IND_SENSOR], pad,
				enum_frame_size, NULL, &fse);
	if (ret < 0)
		return -1;                    /* ← 注意：返回 -1，不是 -EINVAL */
	...
```

`-1` 在 Linux 系统调用返回约定里就是 `-EPERM`（`errno = 1`）。
板端实测 `ENUM_FRAMESIZES` 返回 **`EPERM`** 且 **root 下同样 `EPERM`**，
与这个 `return -1` 精确对应——不是「权限问题」，是驱动把
`v4l2_subdev_call` 的失败码统一压成了 `-1`。

这也解释了为什么 `EPERM` 与 `S_FMT` 的 `EINVAL` 同时出现：
两者都源自同一个 `pipe.sd[VIN_IND_SENSOR] == NULL`。

### 2.6 `S_INPUT` 是唯一修复入口

`vin_video.c:2372-2412`（`__vin_s_input`），关键三步：

```c
	if (__vin_sensor_setup_link(vinc, module, valid_idx, 1) < 0) {   /* 2039-ish：打开 sensor→mipi */
		vin_err("sensor setup link failed\n");
		return -EINVAL;
	}
	if (__csi_isp_setup_link(vinc, 1) < 0) {                          /* 打开 csi→tdm_rx */
		vin_err("csi&isp setup link failed\n");
		return -EINVAL;
	}
	...
	mutex_lock(&cap->vdev.entity.graph_obj.mdev->graph_mutex);
	ret = vin_pipeline_call(vinc, open, &cap->pipe, &cap->vdev.entity, true);
	                                                    /* ↑ prepare=true → 重新填充 pipe.sd[] */
	if (ret < 0) {
		vin_err("vin pipeline open failed (%d)!\n", ret);
		return ret;
	}
	mutex_unlock(&cap->vdev.entity.graph_obj.mdev->graph_mutex);
```

**顺序至关重要**：先 `__vin_sensor_setup_link(..., 1)` 把 sensor→mipi 置
ENABLED，再 `vin_pipeline_call(open, ..., prepare=true)` 触发
`vin_md_prepare_pipeline()` 重新上溯——这一次 sensor→mipi 已是 ENABLED，
于是成功写入 `p->sd[VIN_IND_SENSOR]`。

`vidioc_streamon()`（`vin_video.c:2183-2190`）里也有同样的
`__vin_sensor_setup_link + __csi_isp_setup_link`，但它**不调用
`vin_pipeline_call(open, prepare=true)`**，所以**只调 STREAMON 无法补回 `pipe.sd[]`**。
唯一补回的路径是 `S_INPUT`（或运行时 PM resume 的 `vin_core.c:1996`，
那条路径同样带 `prepare=true`，但需要设备先被挂起再恢复）。

## 三、被推翻的三条旧判断

| 旧判断 | 实际 | 推翻依据 |
| --- | --- | --- |
| 「sensor 未上电，三路 regulator 0mA 是根因」 | regulator 0mA 是**未开流时的正常状态**。`sensor_power(PWR_ON)` 由 `__vin_subdev_set_power()` 在 pipeline open 时调用；pipeline 没打开，自然 0mA。 | `vin.c:1084-1137` `__vin_subdev_set_power` + `vin.c:1139` `vin_pipeline_s_power` 的开关序列表；且 `sensor_power` 里的 `vin_set_pmu_channel()` 只在 PWR_ON/ PWR_OFF 分支执行 |
| 「DTB 缺 `vind_*pin-supply`，资源获取不完整是根因」 | 板端 `vind@5800800` **确实有** 三个 `vind_*pin-supply` 属性（phandle 296）。early dmesg 的 `Get isp bus clk failed` 是 probe 早于 clock provider，属正常 deferred 序列。 | `/proc/device-tree/soc@3000000/vind@5800800/` 实测存在 `vind_mclkpin-supply`/`vind_mcsipin-supply`/`vind_mipipin-supply` |
| 「`S_FMT` 失败是因为请求 NV12 而上游是 RG10」 | WARN 判据是 `!sd`（空指针），不是格式。寄存器里的 NV12/RG10 只是现场残留。 | `vin_video.c:820` 原文 `if (WARN_ON(!sd || !tfmt || !fmt_id))` |

## 四、对应用层的直接影响

### 4.1 修复前的序列（**已失效**，留作对照）

`server/src/v4l2_frame_source.cpp` 的 `start()` 原序列是：

```
open → QUERYCAP → [判定单/多平面] → S_FMT → G_PARM/S_PARM → setupBuffers → STREAMON
```

**完全没有 `VIDIOC_S_INPUT`**（全文件零处 `S_INPUT`/`ENUMINPUT`）。
按 2.6，这个序列在 sunxi-vin 上必然在第一步 `S_FMT` 就失败，
即使格式、分辨率、平面数全部正确。

### 4.2 修复内容（2026-11-10 已落地）

在「判定单/多平面」之后、`S_FMT` 之前插入 `S_INPUT`，现在序列是：

```
open → QUERYCAP → [判定单/多平面] → S_INPUT → S_FMT → G_PARM/S_PARM → setupBuffers → STREAMON
```

顺序由 `strace -e trace=ioctl` 实证（vivid）：

```
ioctl(3, VIDIOC_QUERYCAP, ...) = 0
ioctl(3, VIDIOC_S_INPUT, [0])   = 0
ioctl(3, VIDIOC_S_FMT, {...NV12 1280x720...}) = 0
ioctl(3, VIDIOC_REQBUFS, {... count=4 => 4}) = 0
ioctl(3, VIDIOC_STREAMON, [V4L2_BUF_TYPE_VIDEO_CAPTURE]) = 0
```

**必须**放在 `S_FMT` 之前——放在之后无效（`S_FMT` 已经失败）。
`G_INPUT` 返回 0 只能证明默认选中，**不调用 `S_INPUT` 就不会触发
`vin_pipeline_call(open, prepare=true)`**，所以不能靠「默认值已经是 0」跳过。

### 4.3 容错要求（否则会把能用的设备判死）

`VIDIOC_S_INPUT` 在标准 V4L2 里是可选 ioctl，多数 UVC / mem2mem 设备
不实现（返回 `ENOTTY`）。因此实现采用「尽力而为 + 留痕」：

| 返回值 | 处理 |
|---|---|
| `0` | 继续 |
| `ENOTTY` / `EINVAL` | 设备没有 input 概念，静默跳过，不写进 `describe()` |
| 其它 errno | 不中断启动，但记进 `describe()` 的 `[input: ...]` 段 |

第三种是刻意保留的：真机上「`S_FMT` 报 `EINVAL` 却看不出为什么」时，
`S_INPUT` 有没有意外失败是唯一线索。若它因 `EPERM`/`EBUSY` 静默失败，
根因又得从零重推一遍。

### 4.4 已验证的行为

| 场景 | 结果 |
|---|---|
| vivid 正常路径 | `S_INPUT(0)` 生效，采集成功 |
| 驱动返回 `ENOTTY`（LD_PRELOAD 注入模拟 UVC） | `start()` 仍成功，`describe()` 不出现噪音 |
| 驱动返回 `EPERM`（LD_PRELOAD 注入） | `start()` 仍成功，`describe()` 带 `[input: VIDIOC_S_INPUT: Operation not permitted]` |
| 删除 `S_INPUT` 调用（变异） | 新单测立刻失败 |
| `ENOTTY` 判成致命（变异） | 10 个真机用例同时失败 |

新增单测：`V4l2LiveTest.StartSelectsInputZeroBeforeFormatting`。
它以「把驱动切到输入 1 → `start()` → 读回输入索引」的形式黑盒验证，
不依赖任何 sunxi 私有接口（vivid 的输入索引跨 `open()` 保留）。

**尚未验证**：真实 IMX219 + sunxi-vin 上 `S_FMT` 是否真的转绿。
本节的机制推导来自 BSP 源码，vivid 只能证明「ioctl 确实按正确顺序下发」，
不能证明板端行为。板端复验步骤见第五节。

## 五、板端复验步骤（下一步，需板子在线）

按 §4 的结论，最小复验只需三步，且**不需要改 DTB**：

```bash
# 1. 确认 pipe.sd 为空导致的 WARN 可复现
#    先只 S_FMT（预期 EINVAL + dmesg WARNING at vin_video.c:820）
# 2. 同一进程内 S_INPUT(0) 之后立刻 S_FMT
#    预期 S_FMT 成功，且 dmesg 无 WARNING
# 3. 成功后 G_FMT / ENUM_FRAMESIZES 应恢复正常
```

风险提示：此前的板级卡死发生在**驱动处于坏状态**时（`pipe.sd` 为 NULL 的
路径上反复触发 WARN）。复验探针仍必须走 fork + 超时 SIGKILL 保护。
另据第三方 A733 项目记录，`v4l2-ctl --all` 会触发 VIN 驱动 oops 并 panic 板子，
**不要用 `v4l2-ctl --all` 做验证**，只用 `--list-formats-ext` 等窄命令或自建探针。

## 六、参考源

本目录 `vin-references/` 是 `radxa/allwinner-bsp` 分支 `cubie-aiot-v1.4.6` 的
原始源码副本（未修改），用于离线 diff 与行号引用：

| 文件 | 与本文相关的内容 |
| --- | --- |
| `vin.c` | `vin_md_prepare_pipeline`(83)、`vin_create_media_links`(2042)、`sensor_link_to_mipi_csi`(2028)、`vin_setup_default_links`(2226)、`__vin_subdev_set_power`(1084)、`vin_pipeline_s_power`(1139) |
| `vin_video.c` | `vidioc_enum_framesizes`(727，返回 -1)、`vin_pipeline_try_format`(806，820 行 WARN_ON)、`__vin_set_fmt`(1095)、`vidioc_streamon`(2140)、`__vin_s_input`(2372)、`vidioc_s_input`(2493)、`vin_open`(3680)、`__vin_sensor_setup_link`(1982) |
| `vin_core.c` | runtime PM resume 里的 pipeline open(1996) |
| `imx219.c` | `sensor_power`(698，PWR_ON/STBY_ON)、`sensor_detect`、`sensor_init` |
| `sensor_helper.c` | `sensor_enum_frame_size`（`win_size_num` 越界返回 `-EINVAL`） |

上游仓库：<https://github.com/radxa/allwinner-bsp>（分支 `cubie-aiot-v1.4.6`）

# 七、第二层卡点（2026-11-11 新增）：`S_INPUT` 转绿后的 TDM 零帧

## 7.1 第一层已闭环

2026-11-11 板端实测确认：`V4l2FrameSource::start()` 成功，
`S_FMT` **不再 `EINVAL`**，ioctl 顺序为
`QUERYCAP → S_INPUT → S_FMT → S_PARM → REQBUFS → STREAMON`。
第四节「尚未验证」的边界到此关闭——**第一层修复真机成立**。

## 7.2 新症状：`received = 0`

```
tdm0 rx0 frame error! width is 0, height is 716
tdm0 rx0 hblank short! min is 1, max is 1
```

这两个数是**硬件寄存器原值**，不是软件几何：

| 打印 | 寄存器 | 取法 | 源码 |
| --- | --- | --- | --- |
| `width` / `height` | `TDM_RX_FRAME_ERR_REG_OFF` | `width = reg & 0x3fff`，`height = (reg >> 16) & 0x3fff` | `tdm200_reg.c` `csic_tdm_rx_get_size()` |
| `hb_min` / `hb_max` | `TDM_RX_HB_SHORT_REG_OFF` | `hb_max = reg & 0xffff`，`hb_min = (reg >> 16) & 0xffff` | `tdm200_reg.c` `csic_tdm_rx_get_hblank()` |

`width = 0` 的含义是**硬件一行像素都没收到**。这是当时的观测值（720p 请求）。

> **2026-11-11 晚间订正**：改用 `1920x1080` 后，同样两个寄存器读出的是
> `width is 1869, height is 1063`——**不再是 0**。说明 TDM 确实收到了像素数据，
> 只是行/场的边界判断与预期几何差 51 像素宽、17 行高。故障态已从
> 「零帧」演进为「几何不匹配」，见 7.7。

**格式协商是对的**，可排除：`sensor_s_stream on = 1, 1280*720 300f` 中
`300f` 即 `0x300f` = `MEDIA_BUS_FMT_SRGGB10_1X10`（RAW10），
与 `imx219.c:999-1007` 的格式串 `"%s on = %d, %d*%d %x"` 对应。

## 7.3 三条被推翻的假设（2026-11-11）

| 假设 | 推翻依据 |
| --- | --- |
| `csi-master0/1/2` 卡在 24 MHz 是 MIPI PHY 时钟故障 | `csi-masterN` 是 **sensor MCLK**；IMX219 的 INCK 本就要求 24 MHz。这是正常态 |
| DT 缺 `csi_mipi` / `csi_mipi_src` 导致 PHY 无时钟 | `clk_summary` 里**没有任何 mipi 时钟**。`vin.c:219-228` 拿不到就把 `.clock = NULL` 并只 `vin_warn`，是**设计上允许缺失**的分支 |
| `phy_link_mode: 0x0 (unrecognized)` 说明 PHY 没进任何模式 | `sunxi_mipi.c` 的 `phy_common_status_dump()` 中，该 `switch` 的**全部 `case` 被 `#if IS_ENABLED(SUN55IW3) \|\| SUN55IW6` 包住**，`sun60iw2` 编译时必然落到 `default` |

## 7.4 官方文档给出的决定性对照

`docs.radxa.com/cubie/a7a/accessories/camera-8m-219` 的官方可用命令：

```bash
DISPLAY=:0 gst-launch-1.0 v4l2src device=/dev/video0 en-awisp=1 en-largemode=0 \
  ! video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 ! xvimagesink
```

两处结构性差异：

**（1）私有参数 `en-awisp` / `en-largemode`**

它们走 `VIDIOC_S_PARM` 的 `v4l2_captureparm.reserved[]`：

| 字段 | 落点 | 源码 |
| --- | --- | --- |
| `reserved[0]` | `info->use_current_win` | `sensor_helper.c:922` |
| `reserved[1]` | `info->isp_wdr_mode` | `sensor_helper.c:923` |
| `reserved[2]` | `info->large_image` | `sensor_helper.c:924` |
| `reserved[2]` | `vinc->large_image` | `vin_video.c:2576` |
| `reserved[2]` | `tdm_rx->large_image` | `vin_tdm.c:1282` |
| `reserved[2]` | `csi->large_image` | `sunxi_csi.c:1163` |

**我们的 `start()` 从未填过 `reserved[]`（全零）。** 而 `large_image` 会改变
TDM/CSI 的数据通路配置（`sensor_helper.c:54` 的 `large_image == 3` 分支会改
`width_input` 与 overlayer），这是当前最高可疑度的方向。

**（2）分辨率**

官方示例用 1920x1080 / 3280x2464，探针用 1280x720。
720p 在 `sensor_win_sizes[]` 中存在（`hts=2560, vts=1303, bin_factor=2`），
所以不是「不支持」，但**官方路径从未验证过 720p**。

## 7.5 已排除的方向

- **lane 数**：`imx219.c:1016` 的 `sensor_g_mbus_config()` 硬编码
  `V4L2_MBUS_CSI2_2_LANE | V4L2_MBUS_CSI2_CHANNEL_0`，与模组 2-lane 一致。
- **端口映射**：DT 中 `mipi@5810200`(mipi1) 与 `csi@5821000`(csi1) 是唯一
  `okay` 的节点，与 `mpp/vi` 的 `mipi1 → csi1 → tdm_rx0` 自洽。
- **sensor 未上电**：`/sys/bus/i2c/devices/3-0010` 名为 `imx219`，I²C 已 probe。
- **ISP 未参与**：`mpp/vi` 显示 `isp_mode: NORMAL`，ISP 在链路中。
- **格式不匹配**：见 7.2，RAW10 协商正确。

## 7.7 2026-11-11 晚间：官方命令同样跑不通 + 几何差值定位

### 7.7.1 第五轮设计的对照实验，结论是「证伪」

第五轮把官方 `gst-launch` 命令当作决定性对照，预期它能跑通。**实测相反。**

| 运行 | 规模 | 退出码 | 说明 |
| --- | --- | --- | --- |
| run1 | 10 帧 | 124 | 超时 |
| run2 | 10 帧 | 124 | 超时 |
| run3 | 10 帧 | 124 | 超时 |
| run4 | 60 帧 | 124 | 超时 |

命令形态完全按官方示例：

```bash
gst-launch-1.0 -q v4l2src device=/dev/video0 num-buffers=N \
  en-awisp=1 en-largemode=0 \
  ! video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 \
  ! fakesink sync=false
```

第一次跑出「`rc=0` 成功」是**假阳性**：`$?` 取的是管道末端 `tail` 的退出码。
去掉管道后立刻暴露超时。

**因此「我们没填 `reserved[]`」不构成与官方路径的差别——官方也失败。**

### 7.7.2 新的故障态：几何不匹配

```
tdm0 rx0 frame error! width is 1869, height is 1063
tdm0 rx0 hblank short! min is 1, max is 1
tdm0 rx0 hblank short! min is 65535, max is 0
```

| 来源 | 宽 | 高 |
| --- | --- | --- |
| 请求 / 协商（`media-ctl` / `G_FMT`） | 1920 | 1080 |
| TDM 硬件实测（`TDM_RX_FRAME_ERR_REG_OFF`） | 1869 | 1063 |
| 差 | −51 | −17 |

累计报错 1433 条；`frame lost` / `buffer full` / `compose error` 均为 0。

### 7.7.3 逐级 pad 格式核对（确定性证据）

`media-ctl -d /dev/media0 -p`：

| 实体 | pad 格式 |
| --- | --- |
| `imx219`（`/dev/v4l-subdev0`） | `SRGGB10_1X10/1920x1080` |
| `sunxi_isp.0`（`/dev/v4l-subdev11`）× 3 pad | 均 `SRGGB10_1X10/1920x1080` |

**前两级几何完全正确。** 偏差只可能出在
`sunxi_mipi.1 → sunxi_csi.1 → sunxi_tdm_rx.0` 这一段或 TDM 寄存器配置。
（`media-ctl` 输出里这三个实体**没有 `fmt:` 行**，也是线索。）

### 7.7.4 唯一已定位的「把宽度改小」机制：`dma_merge_mode`

`vin_video.c:2569-2576`（`__vin_s_parm`）：

```c
if (vinc->dma_merge_mode == 1)
        parms->parm.capture.reserved[2] = 3;      /* 强制 large_image = 3 */
...
vinc->large_image = parms->parm.capture.reserved[2];
```

而 `large_image == 3` 会改宽度（`sensor_helper.c:54-57`、`vin_tdm.c:388-395`）：

```c
overlayer = vin_set_large_overlayer(width);
cfg->width = width_input + ((width_input / 2 + overlayer) << 16);
/* vin_tdm.c: width = width / 2 + overlayer */
```

**关键推论：即使应用把 `reserved[2]` 填成 0，只要 `dma_merge_mode == 1`，
驱动也会强制覆盖成 3。** 第五轮「我们没填 `reserved[]`」的归因因此不完整。

`dma_merge_mode` 的赋值入口在 `vin_video.c:3550`，来自一个 ioctl。
**本轮未确认板上的实际取值——这是下一步最高优先级。**

## 7.8 下一步（按优先级）

1. **读出 `dma_merge_mode` 的实际值**。若为 1，则 `large_image` 被强制成 3，
   宽度按 `/2 + overlayer` 计算——这是当前最具体、最可验证的假设。
2. 补取 `vin_set_large_overlayer()` 实现（不在已取回的 BSP 文件里），
   算出 `overlayer` 到底是多少，看能否凑出 `1869`。
3. 确认 mipi / csi / tdm_rx 三个实体不打印 `fmt:` 是 `get_fmt` 未实现
   还是格式真未设置。
4. TDM `hblank short` 阈值来源未查到（BSP 里只有读、无写）。

**不排除最终发现是这颗 sensor 在 sun60iw2 上 1080p 模式本就存在 BSP 缺陷**
——官方命令也跑不通，是支持该假设的一条证据。

**在 `DQBUF` 真正拿到一帧之前，不得宣称采集已打通。**

---

## 相关

- `openspec/tasks/2026-11-10-v4l2-real-board-bringup-and-tunnel-incident.md`
  —— 探针记录、三轮推翻链条与第五轮（2026-11-11）真机复验
- `server/src/v4l2_frame_source.cpp` —— `S_INPUT` 修复的落点
- `openspec/tasks/2026-11-08-stage2-overlay-drill-playbook.md` —— 串口救援演练
- 官方文档：<https://docs.radxa.com/cubie/a7a/accessories/camera-8m-219>
  —— `en-awisp` / `en-largemode` 的唯一权威来源与可用分辨率

## 7.9 2026-11-11：**采集已打通** —— `bin_factor=1` 模式可用，`bin_factor=2` 模式全零帧

> 本节是本轮**唯一的正面突破**，也是第六轮「TDM 几何不匹配」卡点的答案。
> 所有结论都来自板端真实帧数据，不是推断。

### 7.9.1 决定性实验结果（基准守卫法，排除了设备坏状态）

实验设计：每次变体测试前后各跑一次**已知可用基准**（3280x2464 + en-awisp=1
+ en-largemode=0）。基准不达标则丢弃该轮变体结果。这解决了前面反复出现的
「设备进入坏状态后所有测试都失败」的污染问题。

| 模式 | imx219 `bin_factor` | 结果 | 输出字节 | 期望字节 | TDM 报错 |
| --- | --- | --- | --- | --- | --- |
| 3280x2464 | **1** | rc=0 | 36,368,640 | 36,368,640 | 0 |
| 1640x1232 | **1**（驱动按 3280x2464 采集后缩放） | rc=0 | 9,092,160 | 9,092,160 | 0 |
| 1920x1080 | **2** | rc=124 | 0 | — | 711 条 |
| 1280x720 | **2** | rc=124 | 0 | — | 710 条 |
| 640x480 | **2** | rc=124 | 0 | — | 710 条 |

**字节数完全吻合**（`width*height*3/2*3帧`），不是「看起来跑通」，是真出帧。

### 7.9.2 帧数据有效性验证（证明不是全黑/全零缓冲）

3280x2464 的 Y 平面（8,081,920 字节）统计：

```
mean=32.58  min=16  max=235  nonzero=8,081,920 (100.0%)
```

`min=16` / `max=235` 是**有意义的图像动态范围**（不是全零、不是全 255），
100% 非零，可确认是真实感光数据。

### 7.9.3 根因：`bin_factor=2` 的 1080p/720p 模式在 sun60iw2 上不通

`sensor_win_sizes[]`（`imx219.c:877-963`）里只有 4 个模式，其中：

- `bin_factor=1`：3280x2464（满分辨率）
- `bin_factor=2`：1080P / SXGA / 720p

**实测规律 100% 干净**：`bin_factor=1` 全部出帧，`bin_factor=2` 全部零帧，
且后者必然伴随 `tdm0 rx0 frame error! width is 1869, height is ...`。

1869 这个数在寄存器层面有落点：1080p 模式表里裁剪窗口
`0x0164/0x0165 = 0x02A8 = 680`、`0x0168/0x0169 = 0x02B4 = 692`，
即水平方向只有 `692 - 680 = 12` 个像素的 margin；而满分辨率表是
`0x0164=0x03E8=1000` → `0x0168=0x02F0=752`。**binning 模式下 sensor 的实际
读出窗口与 TDM 期望的整帧几何不一致**，这才是第六轮记录里「1920 → 1869」
（−51）的真实来源，与 `dma_merge_mode` / `large_image` **无关**。

> 第六轮把 `dma_merge_mode == 1` 强制 `reserved[2]=3` 当作唯一嫌疑，本条实测
> **证伪**：`en-largemode=0` 与 `en-largemode=1` 在 1080p 上都是零帧，
> 而 `en-largemode=0` 在 3280x2464 上完全正常。`large_image` 不是分水岭，
> `bin_factor` 才是。

### 7.9.4 `en-awisp` 的真实作用（纠正第六轮理解）

| 配置 | 3280x2464 结果 |
| --- | --- |
| `en-awisp=1`（显式） | rc=0，出帧 |
| `en-awisp=0`（显式） | rc=0，出帧 |
| **完全不设该属性** | **rc=124，零帧** |

即：**必须显式下发 `S_PARM`（无论 `reserved[0]` 是 0 还是 1），不设就零帧。**
这说明缺的不是 `reserved[]` 的取值，而是 **`S_PARM` 这个调用本身**——
它触发 `__vin_s_parm()` → `sensor_s_parm()` → `sensor_cfg_req()` 这条
把几何写进 TDM 的链路。

`en-largemode` 是**布尔 GObject 属性**，只能取 0/1；传 `3` 会直接被
gst-launch 拒绝（`could not set property "en-largemode" ... to "3"`）。
第六轮以为它可以取 3（对应 `reserved[2]=3`），这是对属性类型的误读。

### 7.9.5 对产品代码的直接影响

`server/include/v4l2_frame_source.hpp:72-73` 的默认分辨率原本是 **1280x720**，
正是零帧模式。注释里写的理由是「要的是帧统计，不是画质，满分辨率会让
ISP/内存带宽成为瓶颈」。

**这条理由在真机上被推翻**：720p 在本板**根本出不了帧**（bin_factor=2 缺陷），
而 3280x2464 能稳定出帧。

**2026-11-11 已改**：默认值改为 **1640x1232**（依据见 7.9.7），
并新增单测 `DefaultResolutionAvoidsBinningModes` 逐条排除实测零帧的模式。

### 7.9.6 已排除的其他解释

- **不是 `dma_merge_mode`**：见 7.9.3，`large_image` 开关不影响 1080p 的失败。
- **不是 `en-largemode` 取值**：`largemode=0/1` 在 1080p 都是零帧。
- **不是设备坏状态**：8 次基准全部 rc=0、字节数精确。
- **不是格式问题**：全部测试都用 NV12；3280 与 1640 的差别只在尺寸。

### 7.9.7 产品同款 ioctl 序列实测（2026-11-11 第二轮）

前面 7.9.1 用的是 `gst-launch`；本节改用**产品代码自身走过的 ioctl 序列**
（`S_INPUT → S_FMT → S_PARM(reserved[0]=1, reserved[2]=0) → REQBUFS(4) →
QUERYBUF/mmap/QBUF → STREAMON → DQBUF`）在板端直连测量，确认「gst 能跑通」
不等于「产品能跑通」这个疑虑不成立。

| 请求尺寸 | 实际 S_FMT 结果 | 帧数/6s | fps | 带宽 | Y 平面非零占比 | TDM 报错 |
| --- | --- | --- | --- | --- | --- | --- |
| 3280x2464 | 3280x2464 | 110 | 18.33 | ≈222 MB/s | 87.0% | 0 |
| 1640x1232 | **1640x1232**（但 dmesg 记 `s_fmt set width = 3280, height = 2464`） | 110 | 18.33 | ≈56 MB/s | 96.9% | 0 |

三条新事实：

1. **驱动代劳了缩放（Plan B 不必要）**。请求 1640x1232 时，交付尺寸确实是
   1640x1232，但 `dmesg` 的 `s_fmt set width = 3280, height = 2464` 说明
   驱动内部仍按 `bin_factor=1` 的满分辨率采样、再缩到 1640x1232 交付。
   所以「小尺寸」与「走可用通路」在本板是同一件事，应用层**不需要**再做
   一次软件缩放。

2. **两种尺寸帧率完全相同（18.33fps）**，而带宽差 4 倍。这直接决定默认值
   该选 1640x1232 而不是 3280x2464：同样信息量，1/4 的 DMA / 内存带宽。

3. **18.33fps 是硬上限，不是配置问题**。`G_PARM` 回报 `tpf=1/30`、
   `S_PARM(30)` 返回成功，实测仍稳定 18.33fps（`avg_gap=53.9ms`，
   `max_gap=55.9ms`，抖动极小 —— 是稳定节拍而非丢帧）。
   **这构成一个必须记录的归因陷阱**：`v4l2_frame_source.cpp` 的
   「用 G_PARM 实际值回填 `cfg.fps`」逻辑在此时**不会触发**
   （驱动回报的仍是 30），于是 `frames_expected` 按 30 推，
   得到约 `(30-18.33)/30 ≈ 39%` 的**恒定假丢帧率**。
   这是设备基线，换任何分辨率都一样，归因时必须先扣除。

> **未解决**：18.33fps 上限的来源（MIPI 带宽 / ISP / sensor 内部节拍）
> 本轮未定位。它不影响采集可用性，但影响丢帧率绝对值的解释。

### 7.9.7 订正（同日）：分水岭是「sensor 最终跑在哪个模式」，不是 `bin_factor` 字面值

7.9.3 说「`bin_factor=1` 全通、`bin_factor=2` 全挂」，**这条表述不够准确**。

补充实测（基准守卫法，7 次基准全绿）：

| 请求尺寸 | `en-awisp` | dmesg 里 sensor 实际模式 | 结果 |
| --- | --- | --- | --- |
| 3280x2464 | 1 | `s_fmt set width = 3280, height = 2464` | rc=0，36,368,640 B |
| 1640x1232 | 1 | **`s_fmt set width = 3280, height = 2464`** | rc=0，9,092,160 B |
| 1640x1232 | 0 | **`s_fmt set width = 3280, height = 2464`** | rc=0，9,092,160 B |
| 1920x1080 | 0 | binning 模式（TDM 报 `1869`） | rc=124，零帧 |

**`1640x1232` 虽然也是 `bin_factor=2` 的 SXGA 模式表，驱动却没走它**——
`sensor_try_format()`（`sensor_helper.c:425-445`）在 `use_current_win`（即
`reserved[0]` / `en-awisp`）为真时，直接沿用 `info->current_wins`，而
`current_wins` 此时是满分辨率。所以 1640x1232 实际是「满分辨率采集 + ISP
缩放到 1640x1232」，输出才与 `1640*1232*3/2*3 = 9,092,160` 精确吻合。

因此准确的判据是：

> **能出帧的条件是 sensor 最终落在 3280x2464 满分辨率模式。**
> `bin_factor=2` 的模式表只有在 `use_current_win=0` 且请求尺寸命中该模式时才会被选中，
> 而一旦被选中就零帧。`1640x1232` 因为尺寸不命中任何精确模式（SXGA 表是 1640x1232，
> 但 `use_current_win` 优先），侥幸绕开了坏模式。

这也解释了为什么 `en-awisp=0` 时 1640x1232 **仍然**能出帧——我原以为它是
靠「不填 `use_current_win`」才绕开的，实测证否：两种取值都能出帧，
因为 1640x1232 在任何一条路径上最终都没落进 binning 模式表。

**未验证的边界**：没有单独测过「`en-awisp=0` + 精确命中 SXGA 或 720p 表」
的组合。要构造这个组合需要绕开 gst 直接填 `S_PARM.reserved[0]=0` 且请求精确的
`1280x720` / `1640x1232`——而 1280x720 在 `use_current_win=0` 时已实测零帧。

### 7.9.8 产品路径真机闭环（2026-11-11 第三轮，**最终确证**）

7.9.7 及之前所有帧率数据都来自 `gst-launch` 或裸 ioctl 探针，
**产品自己的 `V4l2FrameSource` 从未在真机上跑过**。本节补上这个空白，
并顺带作废一个由测量工具造成的错误数字。

**⚠ 先作废一批数字**：本轮早期用 `gst-launch` 测得约 6.5~6.9 fps，
并据此质疑「18.33 fps」的可靠性。该数字是 **gst 自身的假象**，不得引用：

| 测量路径 | 实测 fps | 发 `S_PARM` |
| --- | --- | --- |
| `v4l2-ctl --stream-mmap` | 17.2 ~ 18.6 | 否 |
| `gst-launch v4l2src`（4 种 `io-mode` 全试） | 6.5 ~ 6.9 | 依 `en-awisp` |
| 裸 ioctl（产品同款序列） | 18.29 | 是 |
| **产品 `V4l2FrameSource`** | **18.58** | 是 |

gst 的 6.86 fps 与 `io-mode`、`en-awisp`、曝光三档**全部无关**
（三组实验分毫不差），且每组前后基线守卫均通过（16.83 / 16.75 / 16.82 fps）。

**产品路径实测**（探针直链 `v4l2_frame_source.cpp`，交叉编译上板，
连续调用 `snapshotAndReset()`——该函数自身阻塞一个 `window_ms`）：

| 分辨率 | REAL_FPS | 带宽 | seq_gaps | corrupt | max_gap | p95_dequeue |
| --- | --- | --- | --- | --- | --- | --- |
| 1640x1232 | 18.58 | 54.0 MiB/s | 0 | 0 | 54 ms | 0~1 ms |
| 3280x2464 | 18.58 | 214.8 MiB/s | 0 | 0 | 54 ms | 0 ms |

三条结论：

1. **默认 1640x1232 正确**：与 3280x2464 帧率完全相同，带宽省 4 倍。
2. **帧统计链路零丢帧**：`seq_gaps=0`、`corrupt=0`、`max_gap≈54ms≈1000/18.58`。
   `loss_expected=38.10%` 完全来自「驱动报 30fps、实际只出 18.58fps」，
   是**基线偏差而非链路丢帧**，两种分辨率都是 38.10%，归因时必须扣除。
3. **`num_planes` 不是阻塞点，且代码已正确采用驱动回报值**：驱动对 NV12
   回报 `num_planes=1`（`sizeimage=3030720`），`buf.length=2` 仍被接受、
   `p0` 正常出帧，仅 `p1` 长度为 0 使 `mmap` 报 EINVAL。产品代码
   `v4l2_frame_source.cpp:946-948` 在 `S_FMT` 之后、`REQBUFS` 之前已用
   `fmt.fmt.pix_mp.num_planes` 覆盖初始值，符合「不信配置信事实」。
   **此处无待办。**

**已知未覆盖**：最长连续采集 9 秒，无分钟级/小时级长稳数据。

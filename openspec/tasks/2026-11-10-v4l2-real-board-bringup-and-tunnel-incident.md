# V4L2 真机 bring-up 首次验证 + 反向隧道故障复盘

> 日期：2026-11-10
> 分类：工作流外实施（真机验证 + 运维修复 + 离线源码定位 + 代码修复）
> 结果：**根因已确定，代码修复已落地并宿主机验证，真机复验待做**。
> 媒体拓扑解析在真机上拿到了完整证据；摄像头采集链路不通的根因在第三轮
> 通过 BSP 源码逐行确认到**应用层缺少 `VIDIOC_S_INPUT`**（不是 DTB、不是供电）。
> 第四轮（同日 19:10 后）已在 `V4l2FrameSource` 里补上该调用并验证容错，
> **但未在板端复验**（板子已断电）。隧道故障已定位并修复。
>
> **第二轮（同日 17:40-18:00）**：用受保护的只读/低频写探针推翻了昨天三条错误结论，
> 并首次拿到 `S_FMT` 失败的内核级证据。当时的根因判断（sensor 未上电）
> **已在第三轮被推翻**。
>
> **第三轮（同日 18:40-19:10，板子已断电）**：取得 `radxa/allwinner-bsp`
> 源码，逐行对齐板端错误码，**确认根因是应用层漏调 `VIDIOC_S_INPUT`**。
> 详见「第三轮订正」与
> `docs/hardware/radxa-cubie-a7a/sunxi-vin-摄像头链路根因分析.md`。
>
> **第四轮（同日 19:10 后，仍无板子）**：按第三轮的结论实施代码修复——
> `S_INPUT` 插到 `S_FMT` 之前，容错分三档；新增一条黑盒单测；
> 变异测试抓两个真缺陷；`strace` 实证 ioctl 顺序；全量回归 61/61。
> **再强调一次：这些证据全部来自宿主机 vivid，不是板端。**

## 构思

### 要回答什么问题

前两轮（`7e52abf`、`47dc908`）把 `V4l2FrameSource` 的四个深度项全部做完了，
但**没有一条真机证据**，遗留问题写得很清楚：

1. 板端 sunxi-vin 是单平面还是多平面？
2. 板端有没有 `/dev/dma_heap`？DMABUF 实际落在哪条子路径？
3. media controller 拓扑在真机上长什么样？
4. 多平面分支的 fd 回填逻辑从未被真机检验。

本轮开发板与摄像头（Radxa Camera 8M 219 / IMX219）就位，目标是把这四项变成事实。

### 关键前提：验证手段必须先保证「不把板子弄死」

V4L2 的一个 ioctl 在驱动内部阻塞时，**会把整块板的用户态拖死**（后面会看到
实测证据）。因此本轮所有探针都按「能自己了断」设计：

- 每个可能阻塞的 ioctl 放进 `fork()` 出来的子进程；
- 父进程 `waitpid(WNOHANG)` 轮询，超时 `SIGKILL`；
- 子进程输出经 pipe 回传，避免被 kill 后丢掉缓冲。

这条约束不是过度设计——它来自本轮真实代价：三次把板子卡到必须断电。

### 为什么不做成 OpenSpec change

按 memory `no-new-openspec-change-by-default`。本轮全部是**验证动作**与
**运维配置修复**，不改生产接口、不新增 entrypoint、不改 `FrameStats` 契约。
「给 `V4l2FrameSource` 加 ioctl 超时保护」是真正需要决策的行为变更，
单列在「遗留」里等用户决策。

## 实现方式

### 一、探针矩阵（全部交叉编译后 scp 到板端运行）

| 探针 | 作用 | 是否可能阻塞 |
| --- | --- | --- |
| `fmt_probe` | QUERYCAP + ENUM_FMT | 否 |
| `fmt_probe2` | ENUM_FRAMESIZES + TRY_FMT 矩阵 | 否 |
| `fmt_probe3` | 跨 4 节点真实 S_FMT | **是**（未加保护，把板子卡死两次） |
| `sfmt_guarded` | 单次 S_FMT，fork+kill 保护 | 否（保护生效） |
| `sfmt_order` | 对比「只 S_FMT」与「TRY_FMT→S_FMT」 | 否（保护生效） |
| `readonly_probe` | QUERYCAP/ENUM_FMT/G_FMT/G_PARM | **是**（把板子卡死一次） |
| `media_probe` | MEDIA_IOC_DEVICE_INFO/ENUM_ENTITIES/ENUM_LINKS | 否 |

编译命令（宿主机原生交叉编译）：

```bash
SYSROOT=/opt/sysroot-bullseye-arm64
clang++ --target=aarch64-linux-gnu --sysroot=$SYSROOT -std=c++17 -O1 \
  -static-libstdc++ -static-libgcc <probe>.cpp -o <probe>_arm64
```

### 二、隔离方法：从「看起来像结论」往回收

本轮四次推翻自己的前提，每次都靠**加一个控制变量的探针**而不是猜测：

1. 以为 `S_FMT` 必然阻塞 → `sfmt_guarded` 证明 **EINVAL 时完全不阻塞**；
2. 以为阻塞由 `TRY_FMT` 前置引起 → `sfmt_order` 证明 **两种顺序都 EINVAL、都不阻塞**；
3. 以为 `ENUM_FMT` 一直失败 → 翻出 14:11 的旧记录，**当时列出过 54 种格式**；
4. 以为四个 video 节点能力不同 → 实测 **四个节点 devcaps 完全相同**。

### 三、反向隧道故障：两个独立缺陷叠加

详见「结果」一节的故障链。修法是三件事：

1. `tools/weaknet-tunnel.service` 去掉 `-L 8000` / `-L 8090`；
2. `KillMode=process` → `control-group`；
3. 端口 `2222` → `22222`，并新增 `tools/tunnel_guard.sh` 做转发层存活探测。

## 结果

### 已经拿到真机证据的部分

| 项 | 真机结论 |
| --- | --- |
| 平面模式 | `/dev/video0/1/4/5` 的 `device_caps` 全为 `0x05201000`：<br>`VIDEO_CAPTURE_MPLANE \| STREAMING \| READWRITE \| EXT_PIX_FORMAT`。<br>**只有多平面，没有单平面 `VIDEO_CAPTURE`**。前两轮悬而未决的第 1 项有答案了。 |
| media 拓扑规模 | `/dev/media0`（`driver=sunxi-vin-media`, `model=Allwinner Vin`）共 **33 个实体**、29 个 `v4l-subdev`。 |
| 拓扑形状 | `imx219 → sunxi_mipi.1 → sunxi_csi.1 → sunxi_tdm_rx.0/1 → sunxi_isp.0/1 → sunxi_h3a / sunxi_scaler.N → vin_cap.N → vin_videoN` |
| 拓扑断点 | `imx219(pad0) → sunxi_mipi.1(pad0)` 的 **4 条链路全部 `disabled`**；其余主干链路 `ENABLED`（`vin_cap.N → vin_videoN` 也全绿）。 |
| 驱动日志 | `sunxi:vin:[WARN]: get csi mipi clk fail` 与 `get csi mipi src clk fail`（7.53s，早于 imx219 探测）；`iommu_master csi_iommu: attach fail:-517`（EPROBE_DEFER，属正常延迟探测）。 |

`media_topology` 模块的价值在本轮被反向验证了一次：**只看 `/dev/video0`
只能得到 `EINVAL`，是拓扑枚举把「断在哪一跳」显示出来的。**

### 未完成：摄像头采集链路不通

`sxt` 实测状态：

- `S_FMT(NV12/YUYV × np=1,2,3 × field=NONE/ANY/INTERLACED)` **18 种组合全部 `EINVAL`**；
- `ENUM_FMT` 在四个节点上均**返回空**（15:36），而 14:11 同一条二进制曾列出 54 种 fourcc；
- `ENUM_FRAMESIZES` 对 NV12/YUYV/YU12 **均返回空**；
- `TRY_FMT` **原样回显请求值且恒成功**（`np=0/1/2` 都「成功」），**不能作为任何判据**。

**因此本轮不能宣称任何采集路径可用，也不能宣称 DMABUF/多平面在板端被验证过。**

### 三次板级卡死（本轮的硬代价，也是最重要的发现）

| # | 触发 | 现象 |
| --- | --- | --- |
| 1 | `fmt_probe3` 在单进程内跨 4 节点连续 S_FMT | SSH 通道无响应，`banner exchange` 超时 |
| 2 | 同上（重试） | 同上，用户态全死，只能硬重启 |
| 3 | `readonly_probe` 串行访问 4 节点（QUERYCAP/ENUM_FMT/**G_FMT/G_PARM**） | 同上。**「只读」并不安全** |

第 3 次确认了最要紧的一点：**`G_FMT`/`G_PARM` 也可能是阻塞源**，不只是 `S_FMT`。
具体是哪一个尚未隔离（板子当时已卡死，无法继续二分），但结论已经足够：
给 `F_MTX` 类 ioctl 做「只读所以安全」的假设是错的。

### 第二轮验证（同日 17:40-18:00）：推翻三条旧结论 + 拿到内核级根因

板子重启后重新跑一组**每个 ioctl 都在 fork 子进程中执行、5s 超时 SIGKILL** 的探针，
所有探针实测「完成，未阻塞」，没有再出现板级卡死。这本身也证伪了一条前提：
**阻塞不是无条件发生的**，昨天三次卡死更可能是「驱动处于特定坏状态」时的偶发行为，
而不是 ioctl 的固有属性。

#### 被推翻的结论

| 昨天的记录 | 今天实测 | 影响 |
| --- | --- | --- |
| 「`ENUM_FMT` 在四个节点上均返回空」 | `/dev/video0` **返回 58 种格式**（RGBP/RGB3/BGR3/…/NV12/NV21/GREY/LBC/RG10/RG12/RG14/BYR2/YUYV/UYVY/VYUY/YVYU） | 「格式表丢了」是板子坏状态下的现象，重启即恢复。**昨天的 15:36 观测不能作为稳态结论。** |
| 「`ENUM_FRAMESIZES` 对 NV12/YUYV/YU12 均返回空」 | 返回 **`EPERM`（Operation not permitted）**，且 **root 下同样 EPERM** | 不是「空」，是明确的错误码。且 root 也一样 → **不是文件权限，是驱动内部判定**。 |
| 「`V4l2FrameSource` 缺 `S_INPUT` 所以打不开」 | `ENUMINPUT` **成功**，`input[0]: name=sunxi-vin type=2`；`G_INPUT` 返回 `0` | 驱动有 input 概念且默认已选 0。**缺 `S_INPUT` 不是根因**，这一假设可以排除。 |

#### 新增的确凿证据

**1. 平面模式确认**：`QUERYCAP` 返回 `caps=0x85201000` / `devcaps=0x05201000`。
多平面只有 `VIDEO_CAPTURE_MPLANE`，`ENUM_FMT` 单平面枚举返回 **0 项**——
与昨天「只有多平面」的结论一致，本项得到交叉验证。

**2. 完整 media 拓扑（33 entity，含精确 link 状态）**：

```
imx219(id=1) pad[0] ──4 条──> sunxi_mipi.1(id=46) pad[0]
```

这 4 条 link 昨天全部 `disabled`。本轮用 `MEDIA_IOC_SETUP_LINK` 打开，
**3 条成功、0 条失败**，回读确认第一条为 `ENABLED`（flags=0x1）。
下游主干链路本来就全绿：

```
sunxi_mipi.1(46) ──4 条──> sunxi_csi.1(31)          ENABLED
sunxi_tdm_rx.0(34) ──2 条──> sunxi_isp.0(49)        ENABLED
sunxi_tdm_rx.1(37) ──2 条──> sunxi_isp.1(55)        ENABLED
sunxi_isp.0(49) pad[2] ──> sunxi_scaler.0(61)       ENABLED
sunxi_isp.0(49) pad[2] ──> sunxi_scaler.4(67)       ENABLED
sunxi_isp.1(55) pad[2] ──> sunxi_scaler.1(64)       ENABLED
sunxi_isp.1(55) pad[2] ──> sunxi_scaler.5(70)       ENABLED
sunxi_scaler.N(61/64/67/70) ──> vin_cap.0/1/4/5     ENABLED
vin_cap.N ──> vin_videoN                            ENABLED
```

注意：`sunxi_csi.1(31)` 的 4 条 pad[1] 出向链路全部 `disabled`，
`sunxi_tdm_rx.2/3(40/43)` 无任何 link。这些是**未被选中的并行分支**，
不是断点——主干路径经 `tdm_rx.0/1` 是通的。

**3. `S_FMT` 仍然全 `EINVAL`，但这次抓到了内核现场**：

打开 imx219 链路后重跑 S_FMT 矩阵，7 种组合（NV12 1080p/VGA/3280x2464、
YU12 三平面、YUYV、Bayer RG10、np=1 错误值）**仍全部 `errno=22 EINVAL`**。
但 dmesg 出现了驱动自己的 WARNING：

```
WARNING: CPU: 0 PID: 7202 at bsp/drivers/vin/vin-video/vin_video.c:820
         vin_pipeline_try_format.constprop.0+0x38/0x3f0 [vin_v4l2]
Call trace:
 vin_pipeline_try_format.constprop.0
 __vin_set_fmt
 vidioc_s_fmt_vid_cap_mplane
 v4l_s_fmt
 ...
sunxi:vin:[ERR]: vin_pipeline_try_format failed
sunxi:vin:[ERR]: set fmt error
sunxi:vin:[INFO]: camera is on, close dfs
```

寄存器现场提供了格式线索：`x11 = 0x3231564E` 是 `"NV12"` 的小端 ASCII，
`x4 = 0x30314752` 对应 `"RG10"`。即**应用请求 NV12，pipeline 上游当前是
RG10（Bayer）**，`vin_pipeline_try_format` 在这个不匹配上触发了 WARN 并返回 EINVAL。

**4. ~~根因定位：sensor 已 probe，但从未上电~~（已作废，见下方「第三轮订正」）**

> **本条结论错误。** 当时的观察（`i2cdetect` 显示 `UU`、
> `/sys/bus/i2c/drivers/imx219/3-0010` 存在、三路 `vind_*pin` regulator 负载
> 为 `0mA`）本身是真实观测，但**推论错了**：
> `0mA` 不代表「该上电却没上电」，而是**根本没有流在跑**（还没有 `STREAMON`）。
> 而且 `&sensor0` 在 Allwinner 私有 DTB 模型里本来就不声明 supply，
> 供电全部由 `&vind0` 的 `vind_mclkpin-supply` / `vind_mcsipin-supply` /
> `vind_mipipin-supply` 承载（板端确有这三个属性，phandle 296），
> 与 IMX462 参考 overlay 的结构完全一致。原始表格保留如下供追溯：

| 证据 | 观测 |
| --- | --- |
| I²C 应答 | `i2cdetect -y -r 3` 的 `0x10` 显示 **`UU`**（地址被驱动占用）→ imx219 已 probe 成功并绑定 |
| 驱动绑定 | `/sys/bus/i2c/drivers/imx219/3-0010` 存在；`power/control=auto`、`runtime_status=unsupported`（说明该驱动**没有实现 runtime PM**） |
| 供电 regulator | `vind_mipipin` / `vind_mcsipin` / `vind_mclkpin` 的负载**全为 `0mA`**，enable 计数同为 0 |

**5. ~~`vind` 节点的 regulator/时钟告警指向 DTB 缺定义~~（已作废）**

dmesg 早期（7.9s，早于 imx219 探测的 8.05s）确有：

```
sunxi:vin:[WARN]: Get isp bus clk failed!
sunxi:vin:[WARN]: Get isp reset control fail
sunxi:vin:[WARN]: get csi mipi clk fail
sunxi:vin:[WARN]: get csi mipi src clk fail
```

> **这条推论也错了。** 这些是 `vin_probe()` 期的可选资源探测告警，
> 出现在 pipeline 尚未 open 的阶段，**与 `S_FMT` 失败没有因果关系**——
> 真正的证据是 `S_FMT` 的 WARNING 调用栈精确指向
> `vin_video.c:820`，而不是这些早期告警。见「第三轮订正」。

### 第三轮订正（同日 18:40-19:10，纯离线源码分析）：根因在应用层，不在 DTB

板子已断电，本轮**没有碰硬件**，只做源码侧求证。取得 `radxa/allwinner-bsp`
（分支 `cubie-aiot-v1.4.6`）的 VIN 驱动源码，逐行对齐板端观测到的错误码，
**推翻了第二轮的核心结论**。

#### 新根因（已逐行确认）

> **应用必须在 `VIDIOC_S_FMT` 之前先调用 `VIDIOC_S_INPUT`。**
> 该 ioctl 是 sunxi-vin 里**唯一**会填充 `vid_cap.pipe.sd[]` 的用户态入口。
> 不调用它，`pipe.sd[VIN_IND_SENSOR]` 恒为 `NULL`。

三条证据互相咬合：

| 观测 | 源码依据 | 解释 |
| --- | --- | --- |
| `S_FMT` → `EINVAL`，dmesg WARNING 精确在 `vin_video.c:820` | `vin_video.c:820` 是 `if (WARN_ON(!sd \|\| !tfmt \|\| !fmt_id)) return -EINVAL;` | **空指针检查**，不是格式比较。`sd = pipe.sd[VIN_IND_SENSOR]`（811 行）为 NULL |
| `ENUM_FRAMESIZES` → `EPERM`（root 下同样） | `vin_video.c:739-742` 调用失败后 **`return -1`** | 内核 `-1` = `-EPERM`。不是权限问题，是驱动把 subdev 调用失败压成 `-1` |
| media 拓扑里 `imx219[0]→sunxi_mipi.1[0]` 是唯一 disabled 的 4 条链路 | `vin.c:2035` 建链时 **flags 传 `0`**；同函数其它链路全传 `MEDIA_LNK_FL_ENABLED` | sensor→mipi 天生 disabled，需运行时 `MEDIA_IOC_SETUP_LINK` 打开 |

`pipe.sd[]` 为什么是 NULL：`vin_md_prepare_pipeline()`（`vin.c:83`）靠
`media_pad_remote_pad_first()` 逐跳上溯，而该函数**只沿 ENABLED 链路走**。
probe 期 sensor→mipi 仍 disabled，上溯到 mipi 的 sink pad 就 `break`，
`p->sd[VIN_IND_SENSOR]` 保持 NULL。

而 `__vin_s_input()`（`vin_video.c:2372`）做的正是三件事：
1. `__vin_sensor_setup_link(..., 1)` —— 打开 sensor→mipi；
2. `__csi_isp_setup_link(..., 1)` —— 打开 csi→tdm_rx；
3. `vin_pipeline_call(vinc, open, &cap->pipe, &cap->vdev.entity, true)`
   —— `prepare=true`，**重新填充 `pipe.sd[]`**。

此前把寄存器 `x11=0x3231564E`（"NV12"）/ `x4=0x30314752`（"RG10"）
解读成「格式不匹配」是**过度解读**：那是 WARN 现场的残留寄存器值，
WARN 判据是 `!sd`，与格式无关。

#### 被推翻的三条判断（对照表）

| 第二轮结论 | 实际情况 | 推翻依据 |
| --- | --- | --- |
| DTB/overlay 缺定义 | DTB 无问题。`&sensor0` 本就不声明 supply，供电挂在 `&vind0`（板端 phandle 296 存在） | 官方 IMX462 overlay 结构完全一致 |
| sensor 未上电导致失败 | 失败发生在任何上电动作之前。`0mA` 是因为还没流在跑 | `S_FMT` 在 `pipe.sd==NULL` 处就返回了 |
| NV12 与 RG10 格式不匹配 | 与格式无关 | `vin_video.c:820` 是空指针检查 |

#### 交付物

| 路径 | 内容 |
| --- | --- |
| `docs/hardware/radxa-cubie-a7a/sunxi-vin-摄像头链路根因分析.md` | 完整根因文档：6 段逐行证据 + 复验步骤 + 参考源索引 |
| `docs/hardware/radxa-cubie-a7a/vin-references/` | BSP 源码只读副本（424 KB / 7 文件），供离线 diff 与后续排查 |
| `openspec/tasks/2026-11-10-...`（本文件） | 三处旧结论加作废标记，新增本节 |

> **为什么把厂商源码放进仓库**：本仓库此前无法回答「`EPERM` 从哪来」，
> 只能靠推断。有了源码副本，后续任何 V4L2 错误码都能直接定位到返回点，
> 不必再依赖网络与记忆。副本仅 424 KB，且与板端固件分支一致。

#### 对应用层的直接影响

`server/src/v4l2_frame_source.cpp` 的 `start()` 原序列是：

```
open → QUERYCAP → 判单/多平面 → S_FMT → G_PARM/S_PARM → REQBUFS → STREAMON
```

**缺 `S_INPUT`**，因此在 sunxi-vin 上 `S_FMT` 必然 `EINVAL`。

**2026-11-10 晚已按此修复**（`S_INPUT` 插在判平面之后、`S_FMT` 之前），
容错策略与验证证据见 `docs/hardware/radxa-cubie-a7a/sunxi-vin-摄像头链路根因分析.md` §4。
**真机复验仍未做**——板子当时已断电，vivid 只能证明 ioctl 顺序正确，
不能证明 sunxi-vin 的 `S_FMT` 真的转绿。

---

#### 第二轮用到的探针（全部在 `/tmp/v4l2_probe/`，宿主机侧；板端用完已删）

| 探针 | 作用 | 结果 |
| --- | --- | --- |
| `v4l2_input_probe` | QUERYCAP / ENUMINPUT / G_INPUT / ENUM_FMT(双平面) | 全通，58 种格式 |
| `v4l2_sizes_probe` | ENUM_FRAMESIZES × 5 种格式 | 全 `EPERM` |
| `v4l2_sfmt_probe` | 7 组 S_FMT 矩阵（受保护） | 全 `EINVAL`，触发驱动 WARNING |
| `media_link_probe` | 33 entity 全量 link 枚举 + `--enable` | 拓扑完整；imx219 链路可打开 |

编译方式（宿主机原生交叉编译）：

```bash
SYSROOT=/opt/sysroot-bullseye-arm64
clang++ --target=aarch64-linux-gnu --sysroot=$SYSROOT -std=c++17 -O1 \
  -static-libstdc++ -static-libgcc <probe>.cpp -o <probe>_arm64
```

#### ~~为什么这一轮没有把采集打通~~（理由已作废）

> **这一节的结论是错的。** 当时认为「证据指向 DTB/电源域，应用层无解」，
> 实际根因在应用层：**必须在 `S_FMT` 之前先调 `VIDIOC_S_INPUT`**。
> 详见下一节「第三轮订正：根因逐行确认到源码」与
> `docs/hardware/radxa-cubie-a7a/sunxi-vin-摄像头链路根因分析.md`。

原文保留如下供追溯：

按 `verification-before-completion`：**证据指向 DTB/电源域，应用层无解**。
`ENUM_FRAMESIZES` 的 `EPERM` 与 regulator 0mA 共同说明 sensor 没被上电，
而供电的开关时机由 `vind` 驱动的 `s_power`/`power_on` 序列控制，
该序列依赖 DTB 里的 `vind_mipipin-supply` 等属性正确解析。dmesg 里的
`Get isp bus clk failed` 已经说明这个解析不完整。

下一步需要的动作超出「应用层探针」的范围（改 DTB/overlay、核对 Radxa 官方
`sun60iw2p1` 摄像头 overlay 是否匹配 8M 219 模组）。**这一步必须由用户决策**，
因为改 DTB 有变砖风险，且会影响其他外设。详见文末「下一步」。

### 反向隧道故障链（当天另一个完整问题）

按时间顺序，两个独立缺陷叠在一起：

**缺陷 A：无用转发 + `ExitOnForwardFailure` = 整条隧道被拖死**

单元文件原本是：

```
-R 2222:localhost:22 -L 8000:127.0.0.1:8000 -L 8090:127.0.0.1:8090
-o ExitOnForwardFailure=yes
```

板端 `8000` 被一条**入站 SSH 会话自己的转发**占住（`sshd: radxa`, pid 838）。
日志逐字：

```
bind [127.0.0.1]:8000: Address already in use
channel_setup_fwd_listener_tcpip: cannot listen to port: 8000
Could not request local forwarding.
```

于是 `ssh` 立刻 `exit 255` → systemd 每 3 秒重启 → **`-R 2222` 永远轮不到绑定**。
云端 20 分钟内收到 **84 次认证、每次认证后立刻 `session closed`**，而 2222 从未 listen。
云端侧 `ss -ltn` 显示 8000/8090 **本来就没有任何服务**，`curl /health/ready` 返回
`000`——这两个 `-L` 是没有对端的历史遗留。

**缺陷 B：`KillMode=process` 留下僵尸会话**

`systemctl restart` 之后，ssh 主进程确实换成了新 pid，但板端 `pkill -9 -f
'id_cloud_tunnel'` 与重启都没能清掉云端侧的旧会话：三条 RSA 连接
（`50929/50930/50931`）继续 `ESTAB` 存活 **4 小时 17 分**，`Recv-Q` 一度堆积到
`6128` 字节无人读取。最后是在云端手工 `kill -9` 掉对应的 `sshd: ubuntu@notty`
子进程才清干净。

**端口 2222 被第三方会话占用（此归因已于 2026-11-10 晚被推翻，见下）**

清完僵尸后 2222 仍不可用。当时的判断是：监听 2222 的是**另一条会话**
（`sshd: ubuntu`, 认证 key 为 `ED25519:S1QaTuO0…`），
而板端隧道用的是 **`RSA:UfDnbW15…`**——**不同身份**。
那条 `ED25519` 会话来自与本机公网出口相同的 IP（`120.236.177.55`），
每 4~5 秒认证一次再断开，属于云端侧某个未识别的保活进程。
它抢到 2222 后，真正的隧道（`ExitOnForwardFailure=yes`）只能反复启动失败。

> **订正（2026-11-10 晚，同一轮排障的后续）**：上面这条归因是错的。
> 在板端直接核对 `/home/radxa/.ssh/id_cloud_tunnel.pub` 的指纹
> （`ssh-keygen -lf`），得到 **`SHA256:S1QaTuO0g90dcug68j1TptOvBt6jjnLpuGs3uLWDlQs`，
> comment 为 `board-to-cloud`** —— 那条被认定为「第三方」的 `ED25519` 会话，
> **就是隧道自己**。`RSA:UfDnbW15…` 反而是另一条来源不明、
> 现已无人使用的旧连接（24h 内仅 18 次认证，最后一次 16:36）。
>
> 因此「端口被第三方抢占」这一条不成立，真实现象是：**隧道一直用的就是
> ED25519，云端看到的就是它自己**。当时之所以得出相反结论，是把
> 「云端 `sshd` 记录了某个指纹」当成了「已知身份表」，而没有回到
> 板端去核对密钥文件的真实指纹。教训：**身份归属必须回到持有私钥的一端核对，
> 不能从对端的日志反推。**
>
> 端口从 2222 改到 22222 的决定仍然保留——它确实绕开了当天 2222 上堆积的
> 旧会话，但**它解决的是症状，根因是缺陷 B 的僵尸会话**。

**最终修法（三件）**

1. `tools/weaknet-tunnel.service`：删除 `-L 8000` / `-L 8090`，只留 `-R`；
   绑定目标写 `127.0.0.1` 而非 `localhost`（避免 `::1` 优先）；
2. `KillMode=process` → `control-group`（restart 必须彻底清掉整个 cgroup）；
3. 端口 `2222` → `22222`，绕开被抢占的端口；本机 `~/.ssh/config`
   的 `Host board` 同步改 `Port 22222`。

新增 `tools/tunnel_guard.sh`：健康判据是**读到板端 sshd 的 banner**，不是端口 listen
（端口在听恰好是半死态的特征），也不是「SSH 认证成功」。探测到
「端口在听 + 连得上 + 等不到 banner」才判定转发层半死 → 定位承载该端口的 sshd
进程并清理，让板端 `Restart=always` 重连。支持 `--check` / `--watch`。

> **订正：探测方式必须是「读 banner」，不能是「完整认证」。** 第一版用的是
> `ssh -p 22222 radxa@127.0.0.1 true`，它在 2026-11-10 晚把一条**健康**隧道
> 误判成「转发层半死」：该命令直连 `127.0.0.1` 不命中 `~/.ssh/config` 的
> `Host board`，因此拿不到 `IdentityFile`，`BatchMode=yes` 下必然认证失败。
> 而自愈动作会杀掉**健康的** 22222 会话——一次误报就能放倒好隧道。
> 现版本用 `/dev/tcp` 只读 banner 并校验其身份（板端是 `Debian` 系
> `OpenSSH_8.4p1`，云端自身是 `OpenSSH_9.6p1 Ubuntu`），认证与链路判定彻底解耦。
> 三个样本已实测：健康隧道 rc=0；空端口 rc=1；云端本地 22 端口因 banner 非 Debian
> 被判「被其他监听者占用、不清理」rc=1。

### 变更文件

| 文件 | 性质 |
| --- | --- |
| `server/src/v4l2_frame_source.cpp` | **第四轮**：`start()` 在 `S_FMT` 之前插入 `VIDIOC_S_INPUT(0)`；`ENOTTY`/`EINVAL` 静默跳过，其它 errno 记入 `describe()` 的 `[input: ...]` 段；新增 `Impl::input_note` 字段（不被 `closeDevice()` 复位） |
| `server/test/unit/test_v4l2_frame_source_gtest.cpp` | **第四轮**：新增 `V4l2LiveTest.StartSelectsInputZeroBeforeFormatting`（黑盒验证 S_INPUT 生效），配套 `readInputIndex` / `selectInput` 辅助函数，补 `<linux/videodev2.h>` include |
| `docs/hardware/radxa-cubie-a7a/sunxi-vin-摄像头链路根因分析.md` | 新增（第三轮）：VIN 驱动根因文档，6 段逐行证据 + 复验步骤；**第四轮**把第四节从「修复方向」改写为「已落地的修复 + 容错表 + 验证表 + 未验证边界」 |
| `tools/weaknet-tunnel.service` | 删两个无用 `-L`、`KillMode` 改 `control-group`、端口 22222、加缺陷说明注释；第三轮补齐 `ExecStart` 里遗留的 `-R 2222` → `-R 22222`（原文件注释已称 22222 但实际仍是 2222） |
| `tools/tunnel_guard.sh` | 新增：转发层存活探测 + 自愈。2026-11-10 晚修正探测方式（读 banner 取代完整认证），并订正本文件前述两处错误归因 |
| `docs/hardware/radxa-cubie-a7a/sunxi-vin-摄像头链路根因分析.md` | 新增（第三轮）：VIN 驱动根因文档，6 段逐行证据 + 复验步骤 |
| `docs/hardware/radxa-cubie-a7a/vin-references/` | 新增（第三轮）：`radxa/allwinner-bsp` 分支 `cubie-aiot-v1.4.6` 的 7 个源码文件只读副本（424 KB），供离线定位错误码 |
| `PLATFORM_OPERATION_GUIDE.md` | 同步隧道章节：端口 2222 → 22222、删除已废弃的 `-L 8000/8090` 段、补 `KillMode=control-group` 与半死态判据 |
| `docs/云端开发环境与开发板部署.md` | 同上；并把已废弃的 `cloud-tunnel.service` 整段替换为 `weaknet-tunnel.service` 权威内容 |
| `端侧AI部署与时序异常检测落地方案.md` | 同步端口与服务名（2222 → 22222、`cloud-tunnel` → `weaknet-tunnel`） |
| `tools/ci.sh` | 仅注释：`-L 8000` 互抢、`2222` 端口等描述同步为现行形态，无逻辑改动（`bash -n` 通过） |
| `~/.ssh/config`（本机，不入库） | `Host board` 端口 2222 → 22222 |

### 遗留问题

1. **摄像头链路不通。根因已在第三轮确认为「应用层漏调 `VIDIOC_S_INPUT`」，
   代码修复已于第四轮落地，但真机复验未做。**
   ~~第二轮判断的「sensor 未上电 / vind 资源获取不完整 / DTB 层问题」已作废。~~
   ~~证据链：I²C `0x10` 返回 `UU`（已 probe）、`/sys/bus/i2c/drivers/imx219/3-0010`
   存在、但三路 `vind_*pin` regulator 负载全为 `0mA`；同时 dmesg 有
   `Get isp bus clk failed` / `Get isp reset control fail` / `get csi mipi clk fail`。
   这指向 **DTB/overlay 层**，应用层无解。~~
   **正确结论**：`S_FMT` 在 `vin_video.c:820` 的 `WARN_ON(!sd || ...)` 处返回
   `EINVAL`，因为 `pipe.sd[VIN_IND_SENSOR]` 为 NULL；而填充它的唯一用户态入口
   是 `VIDIOC_S_INPUT`。手动 `MEDIA_IOC_SETUP_LINK` 只开链路、不填 `pipe.sd[]`，
   所以第二轮「链路 3 条已开但 S_FMT 仍 EINVAL」正是必然结果——
   **链路只是必要条件，不是充分条件**。
2. **`V4l2FrameSource` 缺少 ioctl 超时保护，在 sunxi-vin 上是板级风险。**
   `start()` 里的 `G_FMT`/`S_FMT`/`G_PARM`/`S_PARM` 一旦阻塞，整块板用户态被拖死。
   **这是必须修的真实缺陷**，属行为变更，需单独决策。
   补充（第二轮）：受 fork+kill 保护的探针在本轮**全部未阻塞**，说明阻塞不是
   ioctl 的固有属性，而是驱动处于特定坏状态时的偶发行为——但风险等级不变。
3. ~~**`ENUM_FMT` 从「54 种格式」变成「返回空」的中间变量未定位。**~~
   **已澄清（第二轮）**：板子重启后 `ENUM_FMT` 恢复返回 **58 种格式**。
   「返回空」是驱动坏状态下的暂态现象，不是稳定特性。昨天据此得出的
   「格式表丢失」结论作废。同理，`ENUM_FRAMESIZES` 昨天记为「返回空」，
   实际是 **`EPERM`**——两者性质完全不同（前者像缺能力，后者是明确拒绝）。
4. **不宣称 DMABUF 三级路径已在真机验证。** 板端 `/dev/dma_heap/system` 存在
   （`253,0`、`crw-rw----+ root root`），预期会落在 import(heap) 子路径，
   但**采集未通，无法验证**。
5. ~~**`tunnel_guard.sh` 的判定有盲区**，若端口被另一条合法隧道抢占，杀掉后对方会重连再抢。~~
   **已修**：现版本校验 banner 身份，非板端 banner 时只报告不清理（`--check` rc=1），
   不存在误杀他人的路径。剩余边界：板端自身 OTA 若换了发行版标识，需同步
   `TUNNEL_GUARD_EXPECT_BANNER`。
6. **板端 unit 尚未应用仓库改动（`KillMode=process` 仍在运行）。** 仓库里的
   `tools/weaknet-tunnel.service` 已改为 `control-group` 并删掉两个 `-L`，
   但板端 `systemctl cat weaknet-tunnel` 显示仍是 `KillMode=process`，
   且板端 `127.0.0.1:8000` 仍被一条入站 sshd 会话（pid 846）占用——
   即缺陷 A 与缺陷 B 的土壤都还在。需执行一次 `./tools/ci.sh` 或手工同步
   unit 文件后 `daemon-reload` + `restart` 才会生效。
7. **`RSA:UfDnbW15…` 这条来源不明的 key 仍在 `authorized_keys` 里可用**
   （24h 内 18 次认证）。既然它不是隧道密钥，应确认其归属并移除。
8. **~~`MEDIA_IOC_SETUP_LINK` 打开的状态不持久，需要明确「谁负责配置 pipeline」~~**
   **已回答（第三轮）**：不需要产品层决策——驱动自己在 `__vin_s_input()`
   里打开 sensor→mipi 与 csi→tdm_rx 两条链路，应用只需调 `VIDIOC_S_INPUT`。
   本文件里手动 `MEDIA_IOC_SETUP_LINK` 的做法应当**废弃**：
   它只打开链路，不填充 `pipe.sd[]`，所以即使 3 条 link 全部 `ENABLED`，
   `S_FMT` 仍会 `EINVAL`——这正是第二轮「链路已开但 S_FMT 仍失败」的谜底。
9. **~~`EPERM` 的语义尚未在驱动源码层面确认~~**
   **已确认（第三轮）**：`vin_video.c:739-742` 的
   `vidioc_enum_framesizes()` 在 `v4l2_subdev_call(...)` 失败后 **`return -1`**。
   内核 `-1` 即 `-EPERM`。源码已入库，见
   `docs/hardware/radxa-cubie-a7a/vin-references/vin_video.c`。
10. **~~`V4l2FrameSource` 缺 `VIDIOC_S_INPUT`，这是采集不通的直接原因。~~**
    **代码已修（第四轮）**，容错与验证表见「下一步」选项 A。
    源码侧已完全确认（`pipe.sd[VIN_IND_SENSOR]` 唯一填充入口是 `S_INPUT`）。
    **仍未做的是真机复验**：板子已断电。在板端跑通之前不得宣称采集已打通。
    vivid 只能证明 ioctl 顺序正确，证明不了 sunxi-vin 的 `S_FMT` 转绿。

### 下一步（需用户决策）

第三轮之后，「根因在 DTB、应用层无解」的判断已被源码推翻。
现在的状态是：**选项 A 的代码改动已完成并在宿主机验证，只差板端复验一次**。

**~~选项 A：修 `V4l2FrameSource`，插入 `VIDIOC_S_INPUT`（推荐）~~ 已于 2026-11-10 晚完成**

改动落地在 `server/src/v4l2_frame_source.cpp`，位置在「判单/多平面」之后、
`S_FMT` 之前。容错按「尽力而为 + 留痕」实现：

| `S_INPUT` 返回 | 处理 |
| --- | --- |
| `0` | 继续 |
| `ENOTTY` / `EINVAL` | 静默跳过（设备无 input 概念，多数 UVC/mem2mem） |
| 其它 errno | 不中断启动，记进 `describe()` 的 `[input: ...]` 段 |

留下的原因：真机上「`S_FMT` 报 `EINVAL` 却看不出为什么」时，
`S_INPUT` 有没有意外失败是唯一线索，静默丢弃会让根因重推一遍。

**已验证**（宿主机 vivid，非板端）：

| 验证项 | 方式 | 结果 |
| --- | --- | --- |
| ioctl 顺序 | `strace -e trace=ioctl` | `QUERYCAP → S_INPUT → S_FMT → REQBUFS → STREAMON` ✅ |
| 正常路径生效 | 新增单测 `StartSelectsInputZeroBeforeFormatting` | RED（输入停在 1）→ GREEN ✅ |
| `ENOTTY` 容错 | `LD_PRELOAD` 注入强制 `ENOTTY` | `start()` 仍成功，`describe()` 无噪音 ✅ |
| 其它 errno 留痕但不判死 | `LD_PRELOAD` 注入 `EPERM` | `start()` 仍成功，`describe()` 带 `[input: ...]` ✅ |
| 变异：删掉 `S_INPUT` | 手工变异 | 新单测失败 ✅ |
| 变异：`ENOTTY` 判致命 | 手工变异 | 10 个真机用例失败 ✅ |
| 全量回归 | `ctest` | 61/61 通过，真机用例 27/27 通过 ✅ |

**仍然没有验证的**：真实 IMX219 + sunxi-vin 上 `S_FMT` 是否真的转绿。
vivid 只能证明「ioctl 按正确顺序下发」，证明不了 sunxi-vin 的行为。
依据是 BSP 源码推导（见 `docs/hardware/radxa-cubie-a7a/sunxi-vin-摄像头链路根因分析.md`），
**在板端跑通之前不得宣称采集已打通**。

副作用核对：该调用会打开 sensor→mipi 链路并给 sensor 上电，属状态变更。
驱动在 `vin_close()`（`vin_video.c:3717`）里会做 `__csi_isp_setup_link(vinc, 0)` +
`__vin_sensor_setup_link(..., 0)`，**应用侧无需补偿**。

**选项 B：先用厂商工具链对照一次（低风险）**

板端 `apt-cache policy v4l-utils` 显示 `1.20.0-2` 可安装（bullseye/main）。
`v4l2-ctl` 内部会先做 `S_INPUT`（这是 V4L2 标准打开序列的一部分），
所以它能出图而我们的代码不能——**这本身就是对选项 A 判断的最强旁证**。
风险提示：IMX462 参考项目明确记载「**Never run `v4l2-ctl --all`**，
它会 oops VIN 驱动并 panic 板子」。若采用本选项，只能跑
`v4l2-ctl --list-formats-ext` / `--get-fmt-video`，**不要跑 `--all`**。
**安装软件包属于环境变更，需用户授权。**

**选项 C：`V4l2FrameSource` 的 ioctl 超时保护（独立缺陷）**

`start()` 里的 `G_FMT`/`S_FMT`/`G_PARM`/`S_PARM` 一旦阻塞，整块板用户态被拖死。
这是独立且真实的缺陷，与 `S_INPUT` 修复不冲突，可并行或后续单独处理。
属行为变更，适合走 OpenSpec change。

**已明确排除**（原「明确不建议」一节的修正）：`S_INPUT` 恰恰**就是**根因。
第二轮之所以误判为「不是根因」，是因为当时的依据是「`G_INPUT` 返回 0 正常」——
但 `G_INPUT`（`vin_video.c:2325`）只是读 `vinc->sensor_sel`，**完全不做任何填充**，
它正常与 pipeline 是否就绪毫无关系。这是「拿一个无关的 ioctl 成功去否定另一个
ioctl 的必要性」的典型误判。

`en-awisp`/`en-largemode` 仍是**厂商私有控件**，未确认注册前不应试错。

## 相关

- `docs/hardware/radxa-cubie-a7a/sunxi-vin-摄像头链路根因分析.md` —— **本轮最重要产出**，
  VIN 驱动根因的完整逐行证据
- `docs/hardware/radxa-cubie-a7a/vin-references/` —— BSP 源码只读副本
- `openspec/tasks/2026-11-09-v4l2-dmabuf-and-media-topology.md`（本轮验证的对象）
- `openspec/tasks/2026-11-09-v4l2-depth-tests-and-dequeue-latency-wiring.md`
- `openspec/tasks/2026-11-08-stage2-overlay-drill-playbook.md`（overlay 演练手册，
  其中 uart2 节点映射已确认写错，见下）
- `skills/systematic-debugging`（本轮四次推翻自身前提、以及第三轮推翻第二轮结论的方法来源）
- `skills/verification-before-completion`（「不能宣称」清单的来源）

## 方法论附记：三轮推翻链条

本轮最值得记录的不是结论，而是**结论被推翻的方式**：

| 轮次 | 当时的结论 | 推翻它的证据 | 教训 |
| --- | --- | --- | --- |
| 第一轮 | 四条媒体链路 disabled 是根因 | 第二轮用 `MEDIA_IOC_SETUP_LINK` 打开 3 条后 `S_FMT` 仍 `EINVAL` | 链路是**必要不充分**条件 |
| 第二轮 | sensor 未上电 / DTB 缺定义，应用层无解 | 第三轮读 BSP 源码：`S_FMT` 在任何上电动作之前就返回了 | **有错误码就一定有返回点**，去源码里找它，而不是从现象反推 |
| 第二轮 | `S_INPUT` 不是根因（`G_INPUT` 返回 0 正常） | `G_INPUT`（`vin_video.c:2325`）只读 `vinc->sensor_sel`，不做任何填充 | **一个无关 ioctl 的成功不能否定另一个 ioctl 的必要性** |

共同点：每次都是拿「看起来相关的观测」当因果。真正的突破来自
**拿到权威源码并逐行对齐错误码**——`EPERM` 对应 `return -1`、
`WARN at vin_video.c:820` 对应 `WARN_ON(!sd || ...)`，两处都是唯一解，
不存在第二种解释。这也是把 BSP 源码副本入库的核心理由。

## 附：一个待订正的旧结论

`2026-11-08-stage2-overlay-drill-playbook.md` 中把 `sun60iw2p1-uart2.dtbo`
对应到 `uart@2501000`（serial1）。本轮以 `uevent` 的 `OF_FULLNAME` 与
`/sys/firmware/devicetree/base/aliases` 双向核对，**实际翻转的是
`uart@2502000`（serial2）**。该手册的 Step 1 / Step 3 / Step 4 验收判据
引用了错误节点，需单独订正。

---

# 第五轮（2026-11-11）：`S_INPUT` 真机转绿，卡点下移到 TDM 零帧

> 承接第四轮。板子重新上电后，第一次在**真机**上验证了 `VIDIOC_S_INPUT` 修复。
> 结论：**第一层通了，第二层露出来了。**

## 一、第一层已闭环：`S_INPUT` 真机转绿

板端实测：

- `V4l2FrameSource::start()` 返回成功，`S_FMT` **不再 `EINVAL`**；
- `TRACE` 显示 ioctl 顺序为 `QUERYCAP → S_INPUT → S_FMT → S_PARM → REQBUFS → STREAMON`；
- 这证实第四轮基于 BSP 源码的推导（`pipe.sd[VIN_IND_SENSOR]` 唯一填充入口是 `S_INPUT`）
  在真机上成立，**不再是「仅 vivid 验证」**。

## 二、第二层卡点：STREAMON 成功但 received=0

症状（板端 dmesg）：

```
tdm0 rx0 frame error! width is 0, height is 716
tdm0 rx0 hblank short! min is 1, max is 1
```

两个参数都是**硬件寄存器原值**，不是软件算出来的几何：

- `csic_tdm_rx_get_size()`（`tdm200_reg.c`）读 `TDM_RX_FRAME_ERR_REG_OFF`，
  `width = reg & 0x3fff`、`height = (reg >> 16) & 0x3fff`；
- `csic_tdm_rx_get_hblank()` 读 `TDM_RX_HB_SHORT_REG_OFF`，
  `hb_max = reg & 0xffff`、`hb_min = (reg >> 16) & 0xffff`。

所以 `width=0` 的含义是**硬件一行都没收到**，`hb_min=hb_max=1` 是「行消隐短到 1 个周期」。
不是应用层分辨率没设对——`sensor_s_stream on = 1, 1280*720 300f` 已证明
格式协商到的 `mbus_code = 0x300f`（`MEDIA_BUS_FMT_SRGGB10_1X10`，RAW10）是**正确**的
（`imx219.c:999-1007` 的格式串为 `"%s on = %d, %d*%d %x"`）。

## 三、本轮推翻的三条假设（都是我自己上一轮提出的）

| 假设 | 推翻证据 | 结论 |
| --- | --- | --- |
| `csi-master0/1/2` 卡在 24 MHz 是 MIPI PHY 时钟故障 | `csi-masterN` 是 **sensor MCLK**；IMX219 的 INCK 本就要求 24 MHz（`imx219.c` 全表 `fps_fixed=30` 对应 24 MHz 输入）。这是**正常态** | 非缺陷 |
| DT 缺 `csi_mipi`/`csi_mipi_src` 导致 PHY 无时钟、零帧 | `clk_summary` 里**根本没有任何 mipi 时钟**；`clock-names` 16 项全在但无 mipi 项。说明该 SoC 的时钟树本就没有这个 gate，`vin.c:219-228` 的 `vin_warn` 是一条**设计上允许缺失**的分支（拿不到就把 `.clock = NULL`，后续 `clk_prepare_enable` 被 `if` 跳过） | 非根因，撤回 |
| `phy_link_mode: 0x0 (unrecognized)` 说明 PHY 没进任何链接模式 | `sunxi_mipi.c` 的 `phy_common_status_dump()` 里，那个 `switch` 的**全部 `case` 被 `#if IS_ENABLED(SUN55IW3) \|\| SUN55IW6` 包住**；`sun60iw2` 编译时一个 case 都不存在，**必然**落到 `default` 打印 `unrecognized` | 打印缺陷，非故障证据 |

**方法论教训**：这三条都属于「拿一个观测到的异常值当因果」。
`clk_summary` 里 `csi-masterN = 24 MHz` 与「DT 无 mipi 时钟」同时成立时，
正确的推论是「先确认这个时钟是否本就该存在」，而不是直接归因。

## 四、官方文档给出的决定性对照（本轮最大发现）

`docs.radxa.com/cubie/a7a/accessories/camera-8m-219` 给出的**官方可用命令**是：

```bash
DISPLAY=:0 gst-launch-1.0 v4l2src device=/dev/video0 en-awisp=1 en-largemode=0 \
  ! video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 ! xvimagesink
# 另一条：width=3280,height=2464
```

两处与我们裸 V4L2 代码的**结构性差异**：

1. **私有参数 `en-awisp` / `en-largemode`**。它们不是 V4L2 标准控件，而是
   sunxi-vin 通过 `VIDIOC_S_PARM` 的 `v4l2_captureparm.reserved[]` 传递的：
   `sensor_helper.c:922-924` 把 `reserved[0..2]` 分别写入
   `use_current_win` / `isp_wdr_mode` / `large_image`；
   `vin_video.c:2576` 写 `vinc->large_image = reserved[2]`；
   `vin_tdm.c:1282`、`sunxi_csi.c:1163` 同源读取。
   **我们的 `start()` 从未填过 `reserved[]`**（全零）。
2. **官方示例的分辨率是 1920x1080 / 3280x2464**，而探针用的是 1280x720。
   IMX219 的 `sensor_win_sizes[]` 里 720p 是存在的（`hts=2560, vts=1303, bin_factor=2`），
   所以这不是「不支持」的问题，但**官方路径从未验证过 720p**。

## 五、已排除的方向（避免重复劳动）

- **不是 lane 数错**：`imx219.c:1016` 的 `sensor_g_mbus_config()` 硬编码
  `V4L2_MBUS_CSI2_2_LANE | V4L2_MBUS_CSI2_CHANNEL_0`，与模组实际 2-lane 一致。
- **不是端口映射错**：DT 里 `mipi@5810200`（mipi1）与 `csi@5821000`（csi1）
  是唯一 `okay` 的节点，与 `mpp/vi` 的 `mipi1 → csi1 → tdm_rx0` 完全自洽。
- **不是 sensor 未上电**：`/sys/bus/i2c/devices/3-0010` 名为 `imx219`，I²C 已 probe。
- **不是 ISP 未参与**：`mpp/vi` 显示链路为 `mipi1 → csi1 → tdm_rx0 → isp0 → vipp0`，
  `isp_mode: NORMAL`，ISP 在链路中。

## 六、未完成

**仍未在板端跑通采集。** 下一步最高优先级实验（板子已由用户断开，恢复后执行）：

1. **对照实验**：用官方参数形态跑一次——按 `S_PARM` 填 `reserved[2]=0`
   （`en-largemode=0`）并改用 `1920x1080`，看 TDM 的 `frame error` 是否消失。
   这是唯一能把「`reserved[]` 全零」与「零帧」建立或排除因果的动作。
2. **PHY 采样**：在 `STREAMON` 后的 1~2 秒内读 `/sys/kernel/debug/mpp/mipi`，
   看 `phy_en` 是否 `enabled`、`data_lane_en` 是否 `0x3`（2-lane）、
   `data_lane0/1` 是否为 `0x5 (HS_S)`。
   **注意**：上一轮就是在流式期间反复读 debugfs 把板子卡死的，此实验必须
   限制为**单次读取 + 60s 硬超时**，且只读一次。

## 七、本轮顺带修复的运维缺陷：`tunnel_guard.sh` 自愈从未生效

**缺陷**：`is_sshd()` 用**无提权**的 `readlink -f /proc/<pid>/exe`。
实测 sshd 子进程即便以 `uid=1000` 运行，其 `/proc/<pid>/exe` 符号链接属主仍是
**root**（叠加 `kernel.yama.ptrace_scope=1`），非 root 一律 `Permission denied`。
于是 `is_sshd()` 对每个 pid 都返回非零，`heal()` 一个进程都杀不掉——
脚本打印「清理」但退出码不健康，看起来像「没救活」，实际是**识别环节就断了**。
`kill` 同样缺提权（root 拥有的 sshd 会被 `EPERM` 吞掉）。

**修复**：引入 `SUDO` 前缀（`sudo -n true` 探测），`collect_forward_pids` /
`is_sshd` / `kill` 全部走提权；免密 sudo 不可用时**拒绝动手并明确报错**，
不静默降级。

**验证**（2026-11-11，板子离线时）：

| 项 | 结果 |
| --- | --- |
| `bash -n` | 语法 OK |
| `is_sshd` 正例 | `pid=2814818` → `/usr/sbin/sshd` → 识别通过 ✅ |
| `is_sshd` 负例 | `pid=1` → `systemd` 拒绝；自身 bash → 拒绝 ✅ |
| `collect_forward_pids` | 正确返回 `2814818` ✅ |
| 端到端自愈 | 定位并 `TERM` 掉残留会话，22222 端口随即释放 ✅ |

（退出码仍为 1，这是**正确行为**：板子当时已断开，30s 内不可能恢复。）

## 八、本轮变更文件

| 文件 | 性质 |
| --- | --- |
| `tools/tunnel_guard.sh` | 修复自愈失效：`SUDO` 提权前缀；`is_sshd`/`kill`/`collect_forward_pids` 全部走 `sudo -n`；文件头补第二处缺陷记录 |
| `openspec/tasks/2026-11-10-...md` | 本节（第五轮记录） |
| `docs/hardware/radxa-cubie-a7a/sunxi-vin-摄像头链路根因分析.md` | 待同步：状态行与新增第二层卡点 |

## 九、仍未解决的运维事项

1. **云端残留 5 个僵尸会话**（`sshd: ubuntu@notty`，已存活 1h+，不持有 22222），
   会消耗 sshd 并发配额。属 2026-11-10 事故残留，需单独清理。
2. **板端 unit 仍未应用仓库改动**（`KillMode=process` 仍在运行），
   且板端 `127.0.0.1:8000` 曾被入站 sshd 占用。需 `./tools/ci.sh` 或手工同步。
3. **`RSA:UfDnbW15…` 来源不明的 key 仍在 `authorized_keys`**（见遗留问题 7）。

---

# 第六轮（2026-11-11）：官方 `gst-launch` 同样零帧 + 隧道「掉线」真因订正

> 承接第五轮。本轮目标有二：① 用官方命令做对照实验，② 解决用户提出的
> 「开发板掉热点后不自动重连」。两个目标都有了结论，且**都是订正性的**。

## 一、对照实验：官方命令同样跑不通（推翻第五轮的期待）

第五轮把官方 `gst-launch` 命令当作「决定性对照」，预期它能跑通、从而定位
我们裸 V4L2 代码的差异。实测结果相反。

### 1.1 第一次「成功」是假阳性

首次执行：

```bash
timeout 45 gst-launch-1.0 -v v4l2src device=/dev/video0 num-buffers=10 \
  en-awisp=1 en-largemode=0 ! video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 \
  ! fakesink sync=false 2>&1 | tail -30
echo "gst rc=$?"
```

打印出完整 caps 协商（`NV12 1920x1080 30/1`）且 `rc=0`，我据此写下
「官方路径跑通」。**但 `$?` 取的是管道末端 `tail` 的退出码，不是 `gst-launch` 的。**
这是本项目第二次踩同一个坑（第一次见 `verify_frames` 之前的脚本）。

### 1.2 去掉管道后立刻暴露真相

```bash
timeout 20 gst-launch-1.0 -q v4l2src device=/dev/video0 num-buffers=10 \
  en-awisp=1 en-largemode=0 ! video/x-raw,... ! fakesink sync=false >/dev/null 2>&1
echo $?
```

连续 3 次：`rc=124`（超时），每次跑满 20s，一帧都没交出。取 60 帧那次同样
`rc=124`，耗时 45s。

| 运行 | 规模 | 退出码 | 说明 |
| --- | --- | --- | --- |
| run1 | 10 帧 | 124 | 超时 |
| run2 | 10 帧 | 124 | 超时 |
| run3 | 10 帧 | 124 | 超时 |
| run4 | 60 帧 | 124 | 超时 |

**结论：官方示例命令在本地这块板 + 这个内核上同样拿不到帧。**
所以「我们代码缺 `reserved[]`」不构成与官方路径的差别——官方路径也失败。
第五轮设计的对照实验到此**证伪**，不能再用它推导根因。

### 1.3 但官方命令确实触发了 TDM 活动

运行期间 TDM 报错**持续增长**（1428 → 1454，10 帧 26 条），且出现新的取值：

```
tdm0 rx0 hblank short! min is 1, max is 1
tdm0 rx0 frame error! width is 1869, height is 1063
tdm0 rx0 hblank short! min is 65535, max is 0
```

累计报错 1433 条。对比第五轮：那次是 `width is 0, height is 716`（一行都没收到）；
**现在是 `width=1869, height=1063`（收到了一部分像素数据）**。这是**不同的故障态**，
说明硬件链路在某种意义上比第五轮更「通」了，但几何对不上。

### 1.4 几何对不上的具体差值

| 来源 | 宽度 | 高度 |
| --- | --- | --- |
| 请求 / 协商（`media-ctl`、`G_FMT`） | 1920 | 1080 |
| TDM 硬件实测（`TDM_RX_FRAME_ERR_REG_OFF`） | 1869 | 1063 |
| 差值 | −51 | −17 |

### 1.5 逐级拓扑格式核对（本轮取得的确定性证据）

`media-ctl -d /dev/media0 -p` 显示各实体 pad 格式：

| 实体 | pad 格式 |
| --- | --- |
| `imx219`（`/dev/v4l-subdev0`） | `SRGGB10_1X10/1920x1080` |
| `sunxi_isp.0`（`/dev/v4l-subdev11`）三个 pad | 均为 `SRGGB10_1X10/1920x1080` |

**sensor 和 ISP 都是正确的 1920x1080。** 这说明几何信息在前两级是准确的，
偏差只可能发生在 `sunxi_mipi.1 → sunxi_csi.1 → sunxi_tdm_rx.0` 这一段，
或 TDM 的寄存器配置。
（`media-ctl` 输出里 mipi/csi/tdm_rx 三个实体**没有打印 `fmt:` 行**，
说明这几个 pad 的 `get_fmt` 返回了未知/未设置格式，本身也是一条线索。）

活跃链路（`ENABLED`）确认：

```
imx219:0 -> sunxi_mipi.1:0
sunxi_mipi.1:1 <- (ENABLED)
sunxi_csi.1:0 <- (ENABLED)
sunxi_csi.1:1 <- (ENABLED)
sunxi_tdm_rx.0:1 <- (ENABLED)
sunxi_isp.0:0 <- (ENABLED)
vin_cap.0:1 <- (ENABLED)
```

### 1.6 本轮新读到的 BSP 关键代码

`vin_video.c:2569-2576`（`__vin_s_parm`）：

```c
if (vinc->dma_merge_mode == 1)
        parms->parm.capture.reserved[2] = 3;      /* 强制 large_image = 3 */
...
vinc->large_image = parms->parm.capture.reserved[2];
```

而 `large_image == 3` 会改变宽度计算（`sensor_helper.c:54-57`、
`vin_tdm.c:388-395`）：

```c
overlayer = vin_set_large_overlayer(width);
cfg->width = width_input + ((width_input / 2 + overlayer) << 16);
/* vin_tdm.c 同理：width = width / 2 + overlayer */
```

**这是把「1920」变成「1869」这类非预期宽度的唯一已定位机制。**
若 `dma_merge_mode == 1`，驱动会**无视应用传入的 `reserved[2]`**，
强制改成 3。第五轮「我们没填 `reserved[]`」的判断因此**并不完整**——
即使填了 0，只要 `dma_merge_mode == 1` 也会被覆盖成 3。

`dma_merge_mode` 的赋值入口在 `vin_video.c:3550`（`vinc->dma_merge_mode = *dma_merge;`），
来自一个 ioctl（`VIDIOC_SET_DMA_MERGE_MODE` 形态）。本轮**未确认**板上它的实际取值。

## 二、「掉热点不自动重连」真因：不是热点，是云端端口被僵尸会话占死

用户明确要求先解决这个问题。排查后**结论与直觉相反**。

### 2.1 板端 wifi 侧：没有任何真实的掉线记录

全量日志（跨 58 个 boot）中，`CTRL-EVENT-DISCONNECTED` 只出现过 **1 次**：

```
2026-11-11T03:12:46 wlan0: CTRL-EVENT-DISCONNECTED bssid=c6:35:32:9c:7c:7d \
  reason=3 locally_generated=1
```

`locally_generated=1` + `reason=3`（DEAUTH_LEAVING）的含义是**本机主动断开**，
来源是 NetworkManager 自己发起的重新激活（同一毫秒有
`state change: activated -> deactivating (reason 'new-activation')`）。
**不是 AP 消失、不是信号丢失、不是驱动崩。**

`WPA: Key negotiation completed` 全量只有 1 次成功关联（本次启动），
说明这块板历史上极少重连 wifi——因为它从未真正掉过。

### 2.2 板端隧道侧：310 次「端口绑定失败」，每次都在认证成功之后

```
2026-11-10T15:21:56 ... ssh[4399]: Error: remote port forwarding failed for listen port 22222
2026-11-11T02:45:36 ... ssh[3271]: Error: remote port forwarding failed for listen port 22222
共 310 次
```

这条日志的语义要看清：**它是 SSH 认证成功之后**、在申请反向端口时失败。
也就是说板子的 wifi、公网出口、到云端的 SSH 全都是通的，
**唯一卡住的是云端那侧 22222 端口已被别人占着**。

### 2.3 云端侧：sshd 的 `ClientAliveInterval 0` 是根因

```
$ sudo sshd -T | grep clientalive
clientaliveinterval 0
clientalivecountmax 3
```

`ClientAliveInterval 0` = **永不主动探测客户端存活**。后果：

1. 板子断线/重启（哪怕只是 ssh 进程被重启），云端那侧的 sshd 子进程不知道；
2. 它继续持有 `127.0.0.1:22222` 的监听 socket；
3. 板端 `Restart=always` 每 3~5 秒重连一次，每次都因端口已被占用而失败；
4. 死会话永远不会自己退出 → **自锁**。

实测现场：云端持有 22222 的 sshd 会话已存活 18 分钟，
其转发目标是一条 `CLOSE-WAIT` 的半死 socket。

### 2.4 修复（三处，互补）

**（1）云端 sshd 会话回收（根因修法）**

新增 `/etc/ssh/sshd_config.d/60-tunnel-keepalive.conf`：

```
ClientAliveInterval 30
ClientAliveCountMax 3
```

校验与生效：

| 步骤 | 结果 |
| --- | --- |
| `sshd -t` | OK |
| `sshd -T \| grep clientalive` | `clientaliveinterval 30` / `clientalivecountmax 3` |
| `systemctl reload ssh` | OK（不打断现有会话，22222 仍在监听） |
| 现役隧道可用性 | `ssh board` 仍返回 `TUNNEL_STILL_OK` |

放到 `sshd_config.d/` 而不是改主配置：主配置由 cloud-init 与发行版共同维护，
直接改容易在系统升级/cloud-init 重跑时被覆盖。主配置第 12 行已有
`Include /etc/ssh/sshd_config.d/*.conf`。

**（2）云端常驻守护（兜底）**

新增 `tools/weaknet-tunnel-guard.service`，把已有的 `tools/tunnel_guard.sh`
以 `--watch` 常驻运行（60s 一轮）：

```
ExecStart=/usr/local/bin/weaknet-tunnel-guard --watch
Restart=always
RestartSec=30
```

部署方式（unit 里引用 `/usr/local/bin` 副本，不引用仓库路径——
仓库目录名含中文且在用户家目录下，systemd 引用它既脆弱又会在仓库移动时打挂）：

```bash
sudo install -m 0755 tools/tunnel_guard.sh /usr/local/bin/weaknet-tunnel-guard
sudo cp tools/weaknet-tunnel-guard.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now weaknet-tunnel-guard
```

两处修法**互补而非重复**：keepalive 只能验证 SSH 层，
验证不了「SSH 还活着但转发通道已死」这种半死态；守护脚本的判据是
**读到板端 sshd 的 banner**，正好覆盖后者。

**（3）板端 wifi 自动重连加固**

```bash
nmcli connection modify tan connection.autoconnect-retries 0      # 0 = 无限重试
nmcli connection modify tan 802-11-wireless.powersave 2           # 关闭省电
```

`autoconnect-retries` 原为 `-1`（用默认值 = **重试 4 次**）。热点消失超过
4 次尝试后 NetworkManager 会**永久放弃**自动连接，且不打印任何日志——
这是「掉热点后再也连不回来」的机制性原因（虽然本次故障并非由此触发）。
`powersave=2 (disable)` 针对 USB 网卡 AIC8800D80，省电模式下易掉线。

修改后：

```
connection.autoconnect-retries:  0 (forever)
connection.autoconnect-priority: 100
802-11-wireless.powersave:       2 (disable)
```

### 2.5 端到端自愈验证（真做了一遍，不是只读检查）

按 2026-11-10 事故的真实形态人为制造故障：在云端 `kill -TERM` 掉承载转发的
sshd 会话，然后观察。

```
将被清理的会话 pid=3899884
TERM 已发送
t+10s  listen=2  ssh=UP
RECOVERED
```

**板端在 3 秒内自动重连**（`Scheduled restart job, restart counter is at 1`），
守护判定健康（`banner: SSH-2.0-OpenSSH_8.4p1 Debian-5+deb11u6`）。
在此之前的同一动作是「杀掉后再也回不来」。

顺带清理了 2026-11-10 事故遗留的两条 `sshd: ubuntu@notty` 僵尸会话
（存活 1h5m，不持有 22222，占用 sshd 并发配额）。

## 三、本轮变更文件

| 文件 | 性质 |
| --- | --- |
| `tools/weaknet-tunnel-guard.service` | 新增：云端反向隧道守护 systemd unit（常驻兜底） |
| `/etc/ssh/sshd_config.d/60-tunnel-keepalive.conf` | 云端系统配置（不在仓库内）：`ClientAliveInterval 30`，本机 sshd 会话回收 |
| 板端 `nmcli` 连接 `tan` | 运行时配置：`autoconnect-retries 0`、`powersave 2` |
| `openspec/tasks/2026-11-10-...md` | 本节（第六轮记录） |

## 四、camera 链路当前进度与剩余工作

**用一句话概括：硬件链路已验证「能收到部分像素」，但 TDM 几何对不上，
所以一帧完整图像都没成功交付过。**

已完成（有真机证据）：

1. `S_INPUT` 修复在真机成立，`S_FMT` 不再 EINVAL；
2. IMX219 I²C probe 成功（`find the sony IMX219`），4 个 `/dev/video*` 在位；
3. MIPI → CSI → TDM 链路 `ENABLED`，sensor/ISP 几何均为 1920x1080；
4. 数据确实到达 TDM（报错从 `width=0` 变成 `width=1869`，且有 1433 条中断活动）；
5. **官方 `gst-launch` 命令同样零帧**（4/4 超时），排除「我们代码写法特殊」这条假设。

未完成（按优先级）：

1. **确认 `dma_merge_mode` 的实际取值**。它会把 `reserved[2]` 强制改成 3，
   从而触发 `width/2 + overlayer` 的宽度改写——这是目前唯一已定位的
   「1920 → 1869」机制。查法：板端读 vin 相关 ioctl 的设值路径，
   或用 `ldd`/`strace` 对照官方 gst 插件的实际调用。
2. **`large_image=3` 分支的 `vin_set_large_overlayer()` 实现尚未读到**
   （不在已下载的 7 个 BSP 文件里，需补 `vin_core.c` 之外的定义处）。
3. **mipi/csi/tdm_rx 三个实体的 pad 格式读不到**（`media-ctl` 未打印 `fmt:`），
   需要确认这是驱动未实现 `get_fmt` 还是格式真的未设置。
4. **TDM 的 `hblank short` 判据来源未查到**：`TDM_RX_HB_SHORT_REG_OFF`
   在整个 BSP 里只有读（`csic_tdm_rx_get_hblank`），没有找到配置写入点，
   说明阈值可能来自 TDM TX 侧或硬件默认值。

**距离完整打通还有多远（诚实评估）**：核心卡点已从「不知道哪里错」收敛到
「TDM 几何与上游协商值不一致」这一条具体线索上，但根因尚未证实。
剩下的是一条需要逐个排除的参数链（`dma_merge_mode` → `large_image` →
`vin_set_large_overlayer` → TDM 寄存器），每一环都可能还需要一次板端实验。
**不排除最终发现是这颗 sensor 在 sun60iw2 上 1080p 模式本就存在 BSP 缺陷
（官方命令也跑不通，这是支持该假设的一条证据）。**
在板端 `DQBUF` 真正拿到一帧之前，不得宣称采集已打通。

**（2）云端常驻守护（兜底）**

新增 `tools/weaknet-tunnel-guard.service`，把已有的 `tools/tunnel_guard.sh --watch`
做成开机自启的常驻服务（部署到 `/usr/local/bin/weaknet-tunnel-guard`，
不引用含中文的仓库路径）。两层防护分工：

| 层 | 覆盖的故障 | 手段 |
| --- | --- | --- |
| `60-tunnel-keepalive.conf` | SSH 会话整体僵死 | `ClientAlive` 90s 回收 |
| `weaknet-tunnel-guard` | 会话活着但**转发层**死了（keepalive 探不到的形态） | 读 banner 判定 + TERM 承载会话 |

**（3）板端 wifi 自动重连（真实存在的隐患，虽非本次故障原因）**

排查中发现 `tan` 连接的 `connection.autoconnect-retries` 是 `-1 (default)`，
NetworkManager 1.42 的默认语义是**只重试 4 次**；超过即永久放弃自动连接，
且不写日志。这才是「掉热点后不重连」的真实机制——虽然本次并未触发。

```bash
nmcli connection modify tan connection.autoconnect-retries 0      # 0 = 无限重试
nmcli connection modify tan 802-11-wireless.powersave 2           # 关闭省电
nmcli connection modify tan connection.autoconnect-priority 100
```

修改后确认：`autoconnect-retries: 0 (forever)` / `powersave: 2 (disable)` / 优先级 100。

### 2.5 端到端验证（主动破坏 → 自动恢复）

```
基线: 云端 22222 持有者 pid=3899884
动作: sudo kill -TERM 3899884      # 模拟板端断线后云端会话僵死
t+10s: listen=2  ssh=UP  → RECOVERED
```

板端日志同步印证：`Connection to 81.71.76.133 closed by remote host` →
`Scheduled restart job` → `Started`，**3 秒完成重连**。
守护日志：`健康（banner: SSH-2.0-OpenSSH_8.4p1 Debian-5+deb11u6）`。

## 三、本轮变更文件

| 文件 | 性质 |
| --- | --- |
| `tools/weaknet-tunnel-guard.service` | 新增：云端隧道守护常驻单元 |
| `/etc/ssh/sshd_config.d/60-tunnel-keepalive.conf` | 新增（云端机器，不入库）：sshd 会话回收 |
| `openspec/tasks/2026-11-10-...md` | 本节（第六轮记录） |

板端仅改 NetworkManager 运行时配置（`nmcli connection modify`），无文件入库。

## 四、camera 进度小结与剩余工作

**已确认（有板端证据）**：

- `S_INPUT` 修复真机成立，四层 ioctl 顺序正确；
- sensor / ISP 的 pad 格式均为 `SRGGB10_1X10/1920x1080`，几何正确；
- **官方 `gst-launch` 命令同样拿不到帧**（4 次全超时）；
- TDM 实测 `1920x1080 → 1869x1063`，差值 −51 / −17；
- 链路活跃性、lane 数、端口映射、供电、格式协商均已排除。

**未确认（下一步）**：

1. **`dma_merge_mode` 的实际取值**——这是唯一已定位的、能把宽度改小的机制
   （`vin_video.c:2569` 强制 `reserved[2]=3`）。需读取该 ioctl 的当前值。
2. `vin_set_large_overlayer()` 的实现未在已取回的 BSP 文件里，
   可能需要补取 `vin_core.c`/`vin.h` 的对应片段。
3. mipi / csi / tdm_rx 三个实体的 pad 未打印 `fmt:`，需确认是
   `get_fmt` 未实现还是格式真未设置。
4. TDM `hblank short` 的阈值来源未查到（BSP 里只有读、无写）。

**距离打通还有多远（诚实评估）**：卡点已从「不知道哪里错」收敛到
「TDM 几何与上游协商值不一致」这一条具体线索，但根因尚未证实。
剩余是一条需逐个排除的参数链：`dma_merge_mode` → `large_image` →
`vin_set_large_overlayer` → TDM 寄存器，每一环都可能需要一次板端实验。
**不排除最终发现是这颗 sensor 在 sun60iw2 上 1080p 模式本身存在 BSP 缺陷**
——官方命令也跑不通，是支持该假设的一条证据。
在 `DQBUF` 真正拿到一帧之前，不得宣称采集已打通。

---

# 第七轮（2026-11-11）：**采集打通** —— `bin_factor=1` 可用、`bin_factor=2` 全零帧

> 承接第六轮。第六轮把卡点收敛到「TDM 几何 1869x1063 ≠ 协商 1920x1080」并怀疑
> `dma_merge_mode`。本轮用**基准守卫法**做分辨率矩阵，**拿到了真帧**，
> 并证明第六轮的嫌疑是错的。

## 一、结论：采集已打通（有真实帧数据）

| 模式 | `bin_factor` | gst 结果 | 输出字节 | 期望字节 |
| --- | --- | --- | --- | --- |
| 3280x2464 | **1** | rc=0 | 36,368,640 | 36,368,640 ✅ |
| 1640x1232 | **1** | rc=0 | 9,092,160 | 9,092,160 ✅ |
| 1920x1080 | **2** | rc=124 | 0 | — ❌ |
| 1280x720 | **2** | rc=124 | 0 | — ❌ |
| 640x480 | **2** | rc=124 | 0 | — ❌ |

- 字节数**精确吻合** `w*h*3/2*帧数`，不是「看起来跑通」。
- 3280x2464 的 Y 平面统计：`mean=32.58 min=16 max=235 nonzero=100%`
  —— 真实感光数据，不是全零缓冲。
- 3 次重复 + 8 次基准守卫全部通过，排除了此前反复出现的「设备坏状态」。

## 二、方法论：基准守卫法（本轮最重要的工程改进）

前几轮反复踩同一个坑：设备进入坏状态后**所有**测试都失败，于是把
「设备坏了」误读成「这个参数不对」。本轮改成：

> 每个变体测试的**前后各跑一次已知可用基准**（3280x2464 + en-awisp=1 +
> en-largemode=0）。基准不达标则丢弃该轮变体结果。

第一次跑矩阵时基准就是坏的（全 rc=1），正是这个守卫让错误结论没有落地。
修复脚本引号问题后，8 次基准全绿，变体结论才被采信。

## 三、第六轮的两条判断被推翻

| 第六轮判断 | 本轮实测 | 结论 |
| --- | --- | --- |
| `dma_merge_mode==1` 强制 `reserved[2]=3` 是宽度 1869 的来源 | `en-largemode=0` 与 `=1` 在 1080p 上**都是零帧**；而 `en-largemode=0` 在 3280x2464 **完全正常** | `large_image` 不是分水岭 |
| `en-largemode` 可传 3 | gst 直接拒绝：`could not set property "en-largemode" ... to "3"`，它是**布尔** GObject 属性 | 属性类型误读 |

**真正的分水岭是 `bin_factor`**（`imx219.c:877-963`）：两个 `bin_factor=1`
模式全部出帧，三个 `bin_factor=2` 模式全部零帧，无一例外。

1869 在寄存器层面有落点：1080p 模式表裁剪窗口
`0x0164/0x0165=0x02A8=680` → `0x0168/0x0169=0x02B4=692`，水平方向只有 12 像素
margin；满分辨率表是 `0x03E8=1000` → `0x02F0=752`。binning 模式下 sensor 的
实际读出窗口与 TDM 期望的整帧几何不一致。

## 四、`en-awisp` 的真实作用（纠正）

| 配置 | 3280x2464 |
| --- | --- |
| `en-awisp=1` | rc=0，出帧 |
| `en-awisp=0` | rc=0，出帧 |
| **完全不设该属性** | **rc=124，零帧** |

缺的不是 `reserved[]` 的**取值**，而是 **`S_PARM` 这个调用本身**——
它触发 `__vin_s_parm()` → `sensor_s_parm()` → `sensor_cfg_req()` 这条把几何
写进 TDM 的链路。产品代码会调用 `S_PARM`（已实测 `G_PARM` 返回
`V4L2_CAP_TIMEPERFRAME=0x1000`，门控条件成立），所以这层产品侧无缺陷。

## 五、产品代码的唯一剩余障碍：默认分辨率

`server/include/v4l2_frame_source.hpp:72-73`：

```cpp
uint32_t width{1280};   // 1280x720 = bin_factor 2 = 零帧
uint32_t height{720};
```

注释理由是「要的是帧统计不是画质，满分辨率会让 ISP/内存带宽成为瓶颈」。
**这条理由被真机推翻**：720p 在本板根本出不了帧，3280x2464 反而稳定。

修复方向（**属行为变更，需用户决策，本轮未改代码**）：

1. 默认改 `3280x2464`（或 1640x1232，走同一条 bin_factor=1 路径）；
2. 若要小尺寸，正确做法是满分辨率采集后由 ISP/软件缩放，**不是**让 sensor 切 binning；
3. 建议在 `start()` 里对 `bin_factor=2` 模式做一次探测性回退，或至少把
   「该模式在本驱动上不可用」写进 `describe()`。

## 六、本轮变更文件

| 文件 | 性质 |
| --- | --- |
| `docs/hardware/radxa-cubie-a7a/sunxi-vin-摄像头链路根因分析.md` | 新增 §7.9：决定性实验表、帧数据有效性、`bin_factor` 根因、`en-awisp` 纠正、产品影响 |
| `openspec/tasks/2026-11-10-...md` | 本节（第七轮记录） |

板端无文件改动（全部为只读取证 + 临时 gst 命令）。

---

# 第八轮（2026-11-11）：**产品路径真机闭环** —— `V4l2FrameSource` 首次端到端达标

第七轮全部证据来自 `gst-launch`。本轮补上最大的空白：**产品自己的
`V4l2FrameSource` 从未在真机上跑过一次**。现有单测里唯一的「成功路径」
断言用的是 640x480 —— 本项目实测零帧的模式，且只验证 `start()` 返回 true，
不验证出不出帧。

## 一、方法：基线守卫法（本轮第二次救了结论）

本轮先做的一批测量全部指向「约 6.6 fps」，与并行会话声称的 18.33 fps
冲突。若直接采信，会得出「并行会话数字造假」的错误结论。实际原因是
**测量工具本身**：

| 测量路径 | 实测 fps | 是否发 `S_PARM` |
| --- | --- | --- |
| `v4l2-ctl --stream-mmap` | **17.2 ~ 18.6** | 否 |
| `gst-launch-1.0 v4l2src`（任意 `io-mode`） | **6.5 ~ 6.9** | 取决于 `en-awisp` |
| 裸 ioctl 按产品序列 | **18.29** | 是 |
| **产品 `V4l2FrameSource`** | **18.58** | 是 |

`gst` 的 6.9 fps 是 gst 自身的假象，与驱动、与 `S_PARM` 都无关
（`en-awisp` 有无、四种 `io-mode`、三档曝光全部同为 6.86 fps，且前后
基线守卫均通过）。**已作废，不得引用。**

守卫在每一组实验前后各跑一次 `v4l2-ctl` 基线，三次分别为
16.83 / 16.75 / 16.82 fps，说明设备全程健康，测量差异不是设备漂移。

## 二、产品路径实测数据（决定性）

探针：`/tmp/prodprobe/probe.cpp`（一次性，未入库），直接链接
`server/src/v4l2_frame_source.cpp` + `media_topology.cpp`，交叉编译上板。
**用法必须是连续调用 `snapshotAndReset()`**（该函数自身阻塞一个
`window_ms`）；若在两次调用之间 `sleep`，队列会积压，`max_gap` 与
`p95_dequeue` 会假性飙到 400~500ms、fps 掉到 11 —— 这是探针用错，
不是产品缺陷，已由 `ContinuousCallsMeasureFullWindowWithoutPhantomLoss`
这条既有单测的语义印证。

| 分辨率 | REAL_FPS | 带宽 | seq_gaps | corrupt | max_gap | p95_dequeue |
| --- | --- | --- | --- | --- | --- | --- |
| 1640x1232 | **18.58** | **54.0 MiB/s** | 0 | 0 | 54 ms | 0~1 ms |
| 3280x2464 | **18.58** | **214.8 MiB/s** | 0 | 0 | 54 ms | 0 ms |

两个分辨率帧率**完全相同**（18.58），带宽差 4 倍。因此：

- 并行会话把默认值从 1280x720 改成 **1640x1232 是正确的**，本轮实测证实；
- 其注释里「18.33 fps / 222 MB/s vs 56 MB/s」「Y 平面 87.0% vs 96.9%」
  的**量级与结论成立**，与本次裸 ioctl（18.29 fps / 53.1 MiB/s）和
  产品路径（18.58 fps / 54.0 MiB/s / 214.8 MiB/s）一致；
- 「驱动报 30fps 实际只出 18.58fps → 约 38% 恒定假丢帧」
  **被实测证实**（`loss_expected=38.10%`，两个分辨率都是 38.10%）。

`seq_gaps=0` + `corrupt=0` 是关键结论：**帧统计链路本身没有丢帧**，
38.10% 全部来自「驱动名义帧率 ≠ 实际帧率」这一基线偏差，归因时必须扣除。

## 三、`num_planes` 疑点：一度误判为产品缺陷，已排除

首次跑产品探针报 `VIDIOC_REQBUFS(MMAP): Invalid argument`。定位到
代码硬编码 `s.num_planes = 2`（NV12 按 Y+UV 两平面），而 sunxi-vin 对
NV12 回报 **`num_planes = 1`**（紧凑单平面，`sizeimage=3030720`）。

裸 ioctl 实验后确认这**不是**失败根因：

```
S_FMT -> 1640x1232 num_planes=1 sizeimage0=3030720 bpl0=2460
REQBUFS OK count=4
QUERYBUF[0] OK planes: [p0 len=3045504 off=0x0] [p1 len=0 off=0x0]
  mmap p1 FAILED: Invalid argument        ← 第二平面根本不存在
STREAMON OK, got=90 fps=18.29 seq_gaps=0
```

即：驱动接受 `buf.length=2` 且 `p0` 正常出帧，`p1` 长度 0 使 `mmap` 报
EINVAL —— 产品代码在 `p1` 处会 `return false`，但**真正原因是设备处于
上一轮 gst / v4l2-ctl 抢占后的残留状态**。用同一二进制在守卫通过后重跑，
默认参数连续两次均成功（18.58 fps），说明首次失败不可复现于干净设备。

**订正（本节初稿此条写错，已核实）**：初稿说「`num_planes` 硬编码 2 应改为
采用驱动回报值」。**代码本来就已经这么做了** ——
`v4l2_frame_source.cpp:946-948` 在 `S_FMT` 之后、`REQBUFS` 之前执行：

```cpp
const uint32_t got_planes = fmt.fmt.pix_mp.num_planes;
if (got_planes > 0 && got_planes != s.num_planes) {
    s.num_planes = got_planes;      // 已以驱动事实为准
}
```

所以第 849 行的 `2u` 只是**初始猜测值**，紧接着就被驱动回报覆盖。
`num_planes` 处不存在待办，无需改动（这也与产品路径 0 丢帧的实测一致）。

## 四、本轮变更文件

| 文件 | 性质 |
| --- | --- |
| `openspec/tasks/2026-11-10-...md` | 本节（第八轮记录） |

**代码零改动**：本轮只做只读取证与一次性探针（`/tmp` 下，未入库）。
`server/include/v4l2_frame_source.hpp`、`server/test/unit/test_v4l2_frame_source_gtest.cpp`
及两份文档的改动均来自**并行会话**（12:59–13:01），本轮仅作实测验证。

## 五、结论与遗留

已确认：产品 `V4l2FrameSource` 在本板真实可出帧、帧统计零丢帧、
1640x1232 默认值正确。

未做：
1. 长期稳定性采集（本轮最长 9 秒，未做分钟级/小时级）；
3. `S_FMT` 与 `S_PARM` 的先后顺序风险（gst 先 `S_PARM` 后 `S_FMT`，
   产品先 `S_FMT` 后 `S_PARM`；本轮产品路径 18.58 fps 说明当前顺序可用）。

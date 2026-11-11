# 定位「驱动报 30fps 实际只出 18.56fps」的根因

> 日期：2026-11-11
> 分类：探索 / 根因定位（工作流外）
> 结果：已完成（根因确定，物理上闭合）

## 构思

### 要解决的问题

`V4l2FrameSource` 在产品路径上稳定报 **38.15% 丢帧**，长稳 10 分钟不漂移。
这个数字恒定得太整齐，第一反应是「链路稳定丢了固定比例的帧」，于是自然
去查带宽、DMA、ISP 时钟、TDM 反压。

但有一组实验从一开始就与此矛盾：**1640x1232 与 3280x2464 的帧率完全相同**
（都是 18.56），而两者的 DMA 带宽差 4 倍。带宽瓶颈不可能对 4 倍流量变化无感。

### 为什么不做进 OpenSpec change

本轮全程是**只读排查**：读 BSP 源码、读硬件寄存器、跑一次性探针。
没有修改任何产品代码，也没有新增产品表面。根因一旦确定，若要落地修复
（模式表标定或 fps 校准路径），那才需要走工作流。当前阶段符合
「探索性工作」分类。

### 备选方案与取舍

| 方案 | 取舍 |
| --- | --- |
| 直接怀疑带宽/DMA，调 buffer 数量与 sizeimage | 已被 4 倍带宽实验证伪，继续走是浪费时间 |
| 读 `/sys/kernel/debug/` 的 vin 调试节点 | 板端 debugfs 无 vin/csi 目录，且需 sudo；且它不是真值来源 |
| **直接 I2C 读 sensor 寄存器 + 按 PLL 公式正推** | **唯一不经过任何抽象层的数据来源，选它** |
| 相信 `G_PARM` / `--get-ctrl` 回读 | 本条排查要证伪的正是这些，不能拿它们当判据 |

## 实现方式

### 关键实验与手段

全部为一次性探针，未入库（`/tmp/i2crd.cpp`、`/tmp/tsgap.cpp`），
交叉编译后部署到板端运行，用完即删。

1. **帧间隔直方图**（`tsgap`）：连续 DQBUF，对相邻 `v4l2_buffer.timestamp`
   求差并按 2.5ms 分箱。结果：1640x1232 / 3200x2400 / 3280x2464 三者
   **都是 `mean=53.87ms`，且全部样本落在唯一一个箱内，其余箱全零**。
   → 「下游周期性丢帧」被排除（丢帧会让周期变成整数倍，出现多箱）。
2. **裸 I2C 读寄存器**（`i2crd`）：`I2C_RDWR` 扫描 `/dev/i2c-0..24`，
   在 `/dev/i2c-3` 找到 `0x10`，读出 **VTS=2700、HTS=3448、shutter=1000**。
   → 推翻了我自己先前「VTS 被拉伸到 ≈4349」的假设。
3. **PLL 公式正推**：从 `sensor_hxga_regs` 的 `0x0305`（div=3）与
   `0x0307`（MPY=54）算出真实 pixel rate = `2×24MHz×54/3/5 = 172.8MHz`，
   代入 `3448×2700/172.8e6 = 53.875ms = 18.56fps`，**与实测吻到两位小数**。
4. **对照组验算**：同样的公式套 SXGA(MPY=87)/1080p(MPY=57) 分别得
   30.00/29.99 fps，与模式表声明的 30 一致 —— 证明公式与寄存器解读正确，
   只有满分辨率那一条是错的。
5. **时钟实测**：`/sys/kernel/debug/clk/isp/clk_rate` = **704MHz**，
   远高于 `sensor_get_clk()` 算出的 275MHz 需求 → ISP 时钟不是瓶颈。

### 排掉的假设（都曾被认为很可能）

- MIPI 链路带宽不足（`mipi_bps=720Mbps×2lane/10bit = 144 Mpixel/s`）；
  实际像素率只有 172.8MHz，144 < 172.8 看似吻合，但单箱分布已证明没有丢帧。
- TDM offline TX 反压：`tdm_set_rx_data_rate()` 整段被 `#if VIN_FALSE`
  包住，根本没编译。
- AE 长曝光拉长 `frame_length`：真实快门 1000 行 < `vts-4 = 2696`，不可能触发。
- `bin_factor=2` 模式零帧（720p/1080p/640x480）：已确认是另一回事，不影响本结论。

## 结果

### 根因

`docs/hardware/radxa-cubie-a7a/vin-references/imx219.c:877-899` 的
`sensor_win_sizes[]` 满分辨率条目同时声明了 `.pclk = 278MHz` 与
`.fps_fixed = 30`。这两个数在算术上互相配平（`3444×2700×30 ≈ 279MHz`），
所以读代码永远发现不了。而该模式的寄存器表只把 PLL 配到 MPY=54，
真实 pixel rate 是 **172.8MHz**，真实帧率 **18.56fps**。

`278MHz` 是 SXGA 模式的真实值，被复制到了满分辨率条目上，
典型的「复制一条模式、改了分辨率、忘了改 PLL」。

### 38.15% 假丢帧的完整链条

1. sensor 真的以 18.56fps 输出，**一帧都没丢**（单箱直方图 + `seq_gaps=0`）；
2. `sensor_s_parm()` 只把 `tpf` 存进结构体，`sensor_g_parm()` 根本不看它，
   直接返回模式表常数 `fps_fixed = 30`；
3. 所以 `S_PARM` 是空操作，`G_PARM` 永远报 30，
   `v4l2_frame_source.cpp:1018` 的「用驱动实际值回填」永远不触发；
4. `frames_expected` 按 30 推 → 恒定 `(30-18.56)/30 = 38.15%`。

### 遗留问题

- **产品侧尚未修复**。fps 仍按 30 推，38.15% 假丢帧仍在。修法有两条
  （见 `openspec/pitfalls/sensor-mode-table-fps-lie.md`）：改模式表声明，
  或给 `V4l2FrameSource` 加一条可标定的 fps 来源。**是否需要走正式
  OpenSpec change，待用户决策。**
- 接线动作（soak 已通过）仍卡在同一个决策上：5% 的
  `frame_loss_ratio_bad` 阈值对 38.15% 基线无意义，必须先定基线的处置方式。
- `V4l2LiveTest` 单测仍用 640x480（本板零帧模式），是遗留待清理项。

### 产出

- `openspec/pitfalls/` 新增六篇：
  `sensor-mode-table-fps-lie`、`subdev-parm-to-devnode-parm`、
  `fixed-cadence-rules-out-drop`、`ctrl-readback-vs-register`、
  `gst-v4l2src-fps-artifact`、`black-frame-is-lens-film`，
  并更新 README 索引与元教训一节。
- 候选升格：若决定修复模式表或加标定路径，应走正式 change
  （涉及产品行为变更与验收证据）。

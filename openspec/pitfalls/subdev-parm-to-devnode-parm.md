# 子设备的 s_parm 不生效，但父节点照样返回 rc=0

> 发现于：2026-11-11
> 位置：`docs/hardware/radxa-cubie-a7a/vin-references/vin_video.c:2557-2626`
> 影响：`VIDIOC_S_PARM` 的成功返回值不能当作「帧率已改」的证据；任何依赖它的自动化配置都会被静默忽略

## 现象

`v4l2-ctl --set-parm=30` 返回 0，`--get-parm` 回报 `Frames per second: 30.000 (30/1)`。
但同一设备实测帧间隔 53.87ms（18.56fps），且改 S_PARM 前后**实测帧间隔一毫秒都不变**。

更迷惑的是：换个分辨率（3280×2464 → 1640×1232），帧间隔仍然是 53.87ms。
一个「生效了」的帧率设置不该对分辨率完全不敏感。

## 成因

`__vin_s_parm()`（`vin_video.c:2557`）把同一个 `parms` 依次转给五个子设备：

```c
ret = sensor_s_parm(cap->pipe.sd[VIN_IND_SENSOR], parms);
if (ret) vin_warn("v4l2 subdev sensor s_parm error!\n");
ret = sunxi_csi_subdev_s_parm(cap->pipe.sd[VIN_IND_CSI], parms);
if (ret) vin_warn("v4l2 subdev csi s_parm error!\n");
ret = sunxi_tdm_subdev_s_parm(cap->pipe.sd[VIN_IND_TDM_RX], parms);
...
ret = sunxi_isp_s_parm(cap->pipe.sd[VIN_IND_ISP], parms);
ret = sunxi_scaler_subdev_s_parm(cap->pipe.sd[VIN_IND_SCALER], parms);
```

而 sensor 那一层的 `sensor_s_parm()`（`sensor_helper.c:911`）**只把值存进
`info->tpf`，之后没有任何代码读它**。也就是说：

- 子设备返回 0 —— 它确实「成功处理了请求」，只是处理方式是存起来不用；
- 父节点日志里没有一条 `s_parm error` —— 因为根本没出错；
- `v4l2-ctl --get-parm` 拿到 30 —— 那是 `sensor_g_parm()` 从模式表
  `fps_fixed` 拿的常数，与刚才 s_parm 存进去的 `tpf` 无关。

**三层「成功」全部成立，功能一次都没有生效。** 这是返回值语义的真空：
内核 V4L2 契约只要求「不支持则返回错误」，没有要求「支持则必须真的改」。
驱动选择「接受并忽略」，这在内核里完全合法。

## 修法

**不能靠 ioctl 返回值判断参数是否生效，只能用可观测量验证。**

对帧率这类参数，唯一有效的验证是**测帧间隔**：

```bash
# 设完立刻测，比较前后均值
v4l2-ctl -d /dev/video0 --set-parm=15
# 然后跑一段采集，看 mean interval 是否变成 66.7ms
```

帧间隔没变 = 设置没生效，无论 ioctl 返回了什么。

在本仓库的产品代码里，`v4l2_frame_source.cpp:1018` 的「用驱动实际值回填 `cfg.fps`」
逻辑建立在一个错误假设上：**`G_PARM` 会反映实际帧率**。当驱动返回模式表常数时，
`actual == cfg.fps`，回填分支永不进入，`frames_expected` 一直按 30 推。

正确的做法是让 fps 可标定：允许配置里显式给一个实测帧率，
或从 `snapshotAndReset()` 的帧间隔分布反推——但后者有边界（见
`window-span-vs-elapsed-time.md` 与 `sensor-mode-table-fps-lie.md`：
用实测值当分母会把真实丢帧吸收掉）。

## 边界

- 本条说的是「子设备接受但忽略」，**不是**「所有驱动的 S_PARM 都不可信」。
  vivid 虚拟设备上 S_PARM 是真正生效的（`v4l2_frame_source.cpp:974` 的注释记录了
  实测 `expected=30 received=15`，说明那里确实改了帧率）。差别在于：
  **vivid 的帧率是软件节拍，改了就改了；sensor 的帧率由 PLL 与 VTS/HTS 决定，
  想改必须重写寄存器表。**
- 判断一个驱动属于哪一类，不需要读源码：**改参数 + 测观测量**。
  观测量变了就是真支持，没变就是接受并忽略。
- 即使 S_PARM 真的生效，也可能被别的层覆盖。比如 AE 长曝光会把 `frame_length`
  推到 `shutter+4`（`imx219.c:343`），帧率跟着掉——那是正常行为，不是 bug。

## 教训

**`rc=0` 证明的是「调用被接受」，不是「效果已产生」。**

这条在本仓库已经踩过不止一次：`oracle-exit-code-as-truth.md` 说的是拿工具退出码
当 ground truth，本条是拿 ioctl 返回值当生效证据——同一个病的两个变体。
区别在于这一条的迷惑性更强：**乐观路径上每一个信号都是对的**，
只有把「设置后的观测量」和「设置前」比一比，才会发现它纹丝不动。

推论：任何「配置成功」的判据，都必须包含一次**改变后的观测**。
配完不测，等于没配。

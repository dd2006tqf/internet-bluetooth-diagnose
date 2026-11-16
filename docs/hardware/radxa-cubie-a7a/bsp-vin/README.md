# sunxi-vin BSP 完整驱动源码（只读参考副本）

> 来源：`https://github.com/radxa/allwinner-bsp` 分支 `cubie-aiot-v1.4.6`
> 对应内核：`5.15.147-99btf1-a733`（A733 平台官方 BSP）
> 许可：文件头均为 `SPDX-License-Identifier: GPL-2.0-or-later`，
> 版权归 Allwinner Technology Co., Ltd.
> 用途：**只读参考**。不参与编译，不属于任何构建目标。

## 为什么要有这个目录

已有 `../vin-references/` 是**按需摘录**的十来份关键文件。摘录的代价是：
一旦排查走到摘录范围之外，就只能靠猜。

2026-11-16 就撞上了这个边界。当时要读 IMX219 的 MIPI PHY 状态，需要
`CMB_PHY_CTL_REG_OFF` 等寄存器偏移定义，但它们在不曾摘录的
`combo_csi_reg_i.h` 里 —— 仓库没有、板端内核头文件包也没有、全盘搜不到。
寄存器偏移绝不能猜（MMIO 地址错会直接打死板子），于是整条排查线**硬阻塞**。

同一次还缺 `vin_set_mclk_freq()` 的实现，导致关于 sensor MCLK 的假设
只能靠运行时探针反推，无法直接读代码确认。

补齐整棵 `drivers/vin` 子树之后，这两个缺口同时消失：

| 之前缺的 | 现在的位置 |
| --- | --- |
| `CMB_PHY_CTL_REG_OFF` / `CMB_PHY0_EN` / `CMB_PHY_LANECK_EN` | `vin-mipi/combo_csi/combo_csi_reg_i.h` |
| `vin_set_mclk_freq()`（24M/PLL 源选择逻辑） | `utility/vin_supply.c:267` |
| D-PHY 完整寄存器表 | `vin-mipi/dphy/dphy_reg_i.h` |

## 与 `vin-references/` 的分工

| 目录 | 定位 |
| --- | --- |
| `../vin-references/` | **精读笔记型**摘录：每份文件都带"为什么读它"的说明，配套 `README.md` 列结论 |
| `bsp-vin/`（本目录） | **完整原文**：363 个文件全量，保证任何一层都能查到源码行 |

两者不冲突。`vin-references/` 里已有的文件在本目录里有同一份原文，
内容相同（同一分支同一版本）。

## 目录结构

```
bsp-vin/
├── vin.c / vin.h              media device 构链、时钟获取（csi_mclk0/_24m/_pll）
├── top_reg.c/.h/_i.h          顶层寄存器
├── utility/                   公共设施
│   ├── vin_supply.c/.h        ★ vin_set_mclk / vin_set_mclk_freq 实现
│   ├── vin_io.c/.h            寄存器读写封装
│   └── ...
├── platform/                  平台配置（platform_cfg.h 决定编译哪条分支）
├── vin-mipi/                  ★ MIPI CSI-2
│   ├── sunxi_mipi.c/.h        子设备主逻辑、status dump
│   ├── combo_csi/             ★ combo PHY 寄存器（CMB_PHY_* 宏在这里）
│   ├── combo_rx/
│   ├── dphy/                  ★ D-PHY 寄存器（dphy_reg_i.h）
│   └── protocol/
├── vin-csi/                   CSI 控制器
├── vin-tdm/                   ★ TDM 解包（frame error / hblank short 的出处）
├── vin-isp/                   ISP
├── vin-vipp/                  VIPP 后处理
├── vin-video/                 video 节点、ioctl（S_INPUT / S_PARM）
├── vin-cci/                   CCI（sensor I2C 控制器）
├── vin-stat/                  统计
├── vin-rp/                    rp 相关
├── modules/                   ★ sensor 驱动等（233 文件）
│   ├── sensor/                含 imx219.c、sensor_helper.c/.h
│   ├── sensor_power/          上电时序（另有 vin_set_mclk_freq 同名静态函数）
│   ├── actuator/  flash/  sensor-list/
└── vin_test/                  厂商自测代码
```

★ = 排查摄像头零帧时的高频入口。

## 使用方式

直接 `grep` / `Read`，**不要**尝试编译：

```bash
# 查某个宏的定义
grep -rn "CMB_PHY_LANECK_EN" docs/hardware/radxa-cubie-a7a/bsp-vin/

# 查某个函数的实现
grep -rn -A40 "^int vin_set_mclk_freq" docs/hardware/radxa-cubie-a7a/bsp-vin/
```

本目录**没有**顶层 Kconfig 和 Makefile 依赖链（只拉了 `drivers/vin` 子树），
单独拿出来编不了。它只用于阅读。

## 重要提示：平台分支

`drivers/vin` 里有大量 `#if IS_ENABLED(CONFIG_ARCH_SUNxxx)` 条件编译。
本板是 **A733 / sun60iw2**，所以要找 `CONFIG_ARCH_SUN60IW2` 或
`CONFIG_ARCH_SUN60IW1` 那一支，其它平台的代码分支**不会**被编译进来。

已有教训：`vin.c` 里 `sensor_get_clk()` 整个函数被
`#if IS_ENABLED(CONFIG_ARCH_SUN8IW16P1)` 包住，在 A733 上根本不存在。
读源码时必须先确认分支归属，否则会把不存在的代码当成当前行为。

## 未收录的部分

以下目录**没有**拉进来，需要时按同样方式单独取：

| 目录 | 什么情况下需要 |
| --- | --- |
| `drivers/clk` | 要查 CCU 时钟树（`sys24M`、`pll-video0` 怎么来） |
| `arch/arm64/boot/dts` | 要看 DTS 源（板上只有编译后的 DTB） |
| `drivers/phy` | 要看平台 PHY 框架层（`phy@6b00000` 的 provider） |

取法（raw 直连，单文件秒级）：

```bash
curl -sS "https://raw.githubusercontent.com/radxa/allwinner-bsp/cubie-aiot-v1.4.6/<路径>"
```

## 更新方式

上游有新分支时不覆盖本目录，另建 `bsp-vin-<branch>/`。原因同
`vin-references/README.md`：结论必须能追溯到确切版本，覆盖会让旧结论失去锚点。

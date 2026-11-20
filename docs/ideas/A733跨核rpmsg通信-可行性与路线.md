# A733 跨核 rpmsg 通信：可行性与落地路线

> 状态：孵化中（**未动工**）
> 日期：2026-11-17
> 分类：外设/内核能力孵化
> 相关：[`外设驱动全链路-irrx平台驱动.md`](外设驱动全链路-irrx平台驱动.md)、
> [`喂狗条件检查-系统健康看门狗.md`](喂狗条件检查-系统健康看门狗.md)

## 一句话摘要

A733（sun60iw2）的 msgbox 硬件真实存在，Radxa 也开源了完整的
`msgbox/remoteproc/rpmsg/rpbuf` 驱动栈，但**对端 E902 没有 rproc 驱动、
板级 DTS 两层把它关着、E902 当前被 ARISC 电源管理固件占用**。
可行路径不是"从 Tina 抽私有驱动"，而是**先验证 msgbox 通道归属，
再决定 coexisting（共用 E902）还是接管（替换 E902 固件）**。

---

## 零、执行入口（交给执行方 Agent 时先读这一节）

> 本文档是**移交说明**。若你是在 Windows 电脑上打开它并被要求"把这件事做完"，
> 先按本节定位，再跳到对应章节，**不要从头顺序读**。

### 你要做的事，只有一件

在 **WSL2 的 Ubuntu-22.04** 里，复用既有的 `~/linux-a733` 源码树，做一次
**增量内核编译**，只多做两件事：

1. 打开 `CONFIG_MAILBOX` + `CONFIG_AW_MSGBOX`（msgbox 驱动）；
2. 把版本号从 `5.15.147-99btf1-a733` 改成 `5.15.147-100btf1amp-a733`。

**硬约束：现有 BTF 配置一个字节都不能变。** 现有内核的 BTF 是当前 eBPF 能力的
唯一来源，改坏了整套可观测性就没了。

**在哪编**：WSL2 原生。**不要用 Docker Desktop** —— 既有 `.o` / `vmlinux` /
deb 全在 WSL 的 ext4 里，换容器是倒退。理由与探测命令见
**4.3.6「为什么是 WSL2，不是 Docker Desktop」**。

### 直接跳转

| 你的处境 | 去看 |
| --- | --- |
| 第一次接手，想知道为什么这么干 | 第二节（已核验事实） |
| **动手前第一步：探测工具链在不在** | **4.3.6「为什么是 WSL2」末尾的 `which` 探测表** |
| 只要动手编译 | **4.3.7 增量构建流程**（步骤 0–10，逐条可复制） |
| 不知道怎么连开发板 | **4.3.7.1 开发板 SSH 访问方式** |
| 要装到板子上 | 4.3.7 的**步骤 10**，或 4.3.8（板端本机视角，二选一） |
| 源码树没了 / 要重建 | 4.3.7 末尾「附注：完整重建」 |

### 开工前必须先补的四项信息

前三项决定**配置改在哪一层**，第四项决定**用哪套工具链编**。
在 WSL 里执行并把结果贴回来：

```bash
# ① 空间是否够（要 ≥ 20 GB 可用）
df -h ~ | tail -1

# ② 当前 .config 的 BTF 块与版本号（决定增量的起点）
grep -E '^(# )?CONFIG_DEBUG_INFO|^CONFIG_LOCALVERSION' ~/linux-a733/src/.config

# ③ ★ 最关键：上次开 BTF 到底改了哪个文件
cat ~/linux-a733-btf-config.patch

# ④ 工具链在哪：决定直接 make 还是回 devcontainer
which devenv nix pahole aarch64-linux-gnu-gcc
```

第 ③ 项决定后续所有改法：

- 若 patch 改的是 `src/.config` ⇒ 直接 `scripts/config` 改 `.config`；
- 若改的是 `arch/arm64/configs/radxa.config` ⇒ 改同一文件；
- 若新增 `radxa_custom.config` fragment ⇒ 加到同一 fragment。

**走错层的后果**：`bindeb-pkg` 会重新跑 defconfig，`.config` 里的手工改动被覆盖，
BTF 静默丢失，编出来的内核没有 BTF —— 而且编译过程**不报错**。

第 ④ 项的判读表见 **4.3.6「为什么是 WSL2，不是 Docker Desktop」**：
全都有就原地编；缺 `pahole`/`nix` 回 devcontainer；只缺 `pahole` 则装 `dwarves`。

### 纪律

- **在哪编：WSL2 原生**，不用 Docker Desktop（理由见 4.3.6）；
- 不要在 `/mnt/c`、`/mnt/e` 上构建（DrvFs 小文件 I/O 慢一个数量级）；
- 不要盲目 `apt install` 破坏既有的 Nix/devenv 环境；
- **工具链先探测再动手**（`which devenv nix pahole aarch64-linux-gnu-gcc`），别等到链接阶段才发现没有 `pahole`；
- 编译产物先在 WSL 内验证 `.BTF` 段存在，再打包、再上板；
- **上板前必须重建 DKMS**，否则 `aic8800_fdrv` 缺失 ⇒ 无 WiFi ⇒ 板子失联。

---

## 一、需求

在 A 核（Linux）与另一颗协处理器之间建立双向消息通道，
上层形态参照 OpenAMP rpmsg（`/dev/rpmsgX` + 共享内存 payload）。

典型诉求（按提出顺序）：

- sensor 上电/复位时序搬到协处理器；
- 实时性隔离：让不能被调度延迟打断的工作离开 Linux；
- 更一般地：为将来留一条跨核通路。

> **注意**：其中"把 sensor 时序搬走"这条诉求，在本仓库
> 已于 2026-11-16 被证明**不是必要的**——零帧根因是 CSI 排线物理接触不良，
> Linux 侧 `modules/sensor/imx219.c::sensor_power()` 本就实现了完整时序
> （PWDN/RESET/MCLK/四路 VDD，含 1ms/10ms/30ms 显式延时）。
> 见 `openspec/tasks/2026-11-16-bsp-vin-source-localization.md` 追加四。

---

## 二、已核验事实

### 2.1 板端运行时（只读实测，2026-11-17）

| 项 | 实测值 |
| --- | --- |
| 内核 | `5.15.147-99btf1-a733`，`SMP PREEMPT aarch64` |
| vermagic | `5.15.147-99btf1-a733 SMP preempt mod_unload aarch64` |
| `CONFIG_MAILBOX` | `# is not set` |
| `CONFIG_REMOTEPROC` / `CONFIG_RPMSG_VIRTIO` | 均 `is not set` |
| `CONFIG_AW_MSGBOX` / `CONFIG_AW_REMOTEPROC` / `CONFIG_AW_RPMSG_*` / `CONFIG_AW_RPBUF_*` | 全部 `is not set` |
| `CONFIG_ISP_SERVER_MELIS` / `CONFIG_VIN_INIT_MELIS` | 均 `is not set`（VIN 的 rpmsg 客户端因此没编进去） |
| `/sys/class/remoteproc/` | 不存在 |
| `/dev/rpmsg*` | 不存在 |
| `/lib/modules/.../kernel/drivers/{mailbox,remoteproc,rpmsg}` | 不存在 |
| 内核源码 | `/usr/src/linux-a733-5.15` 是**空 git 仓库**（remote `radxa/kernel.git`，无 commit） |
| CPU | 8 核：6× `cortex-a55` + 2× `cortex-a76`，**DT 中无 RISC-V 节点** |

### 2.2 DT（只读实测）

| 项 | 实测值 |
| --- | --- |
| DT 中 `msgbox` / `remoteproc` / `rpmsg` / `e902` 节点 | **全部没有** |
| 深度搜索唯一命中 | `arisc_config`（含 `s_uart_config`：`function=<3 3>`、`pins="PL2","PL3"`、`status="disabled"`） |
| 根节点 `#address-cells` / `#size-cells` | **均为 2**（64 位），不是 1 |
| `reserved-memory/bl31` | **`0x48000000`，长度 `0x01000000`（16 MiB）** |
| `reserved-memory` 其它节点 | 无（没有任何 AMP 共享内存区） |

> ⚠️ 网上流传的 DT 示例 `rproc_fw@48000000`（1 MiB）**正好压在 BL31 上**，
> 直接抄会开机即死。

> **2026-11-19 修正：2.1 表里有两行已经过期。** 新内核 `5.15.147-100btf1amp-a733`
> （构建于 2026-11-17 18:43）的实测结果变了：
>
> | 项 | 2.1 原记录（99btf1） | 当前实测（100btf1amp） |
> | --- | --- | --- |
> | `CONFIG_MAILBOX` | `is not set` | **`=y`** |
> | `CONFIG_AW_MSGBOX` | `is not set` | **`=y`**（驱动已绑定 `3004000.msgbox`，中断 432/433 已注册，计数 0/0） |
> | `CONFIG_SUN6I_MSGBOX` | — | **`=y`** |
> | `CONFIG_REMOTEPROC` / `RPMSG_VIRTIO` / `AW_REMOTEPROC` / `AW_RPBUF_*` / `AW_RPMSG_*` | 均 not set | **仍然均 not set**（未变） |
>
> 也就是说：**传输层（msgbox 驱动）已就绪，生命周期与通信层（rproc/rpmsg/rpbuf）仍然全空。**
> 中断计数 0/0 说明驱动初始化完成但一次中断都没收到——对端始终不存在。
>
> 另：`cpu` 一行是对的。实测 MIDR 确认 `cpu0-5` = `0xd05`(A55, capacity 385)、
> `cpu6-7` = `0xd0b`(A76, capacity 1024)。`lscpu` 的 "Model name" 只显示 cpu0，
> 容易误读成「8×A55」。

### 2.3 源码与打包（`radxa-pkg/linux-a733`，已核验）

真正的配方仓库是 `radxa-pkg/linux-a733`，不是 `radxa-build/radxa-a733`
（后者只是调用 `RadxaOS-SDK/rsdk` 的 CI 壳）。

`.gitmodules`：

```
[submodule "bsp"]         url = radxa/allwinner-bsp      branch = cubie-aiot-v1.4.6
[submodule "src"]         url = radxa/kernel             branch = allwinner-aiot-linux-5.15
[submodule "device-a733"] url = radxa/allwinner-device   branch = device-a733-v1.4.6
```

`.github/local/Makefile.local` 给出拼接方式：

```make
CUSTOM_MAKE_DEFINITIONS := BSP_TOP=bsp/ LICHEE_KERN_DIR=./ KBUILD_DEFCONFIG=bsp.config
KERNEL_DEFCONFIG := defconfig radxa.config radxa_custom.config

src/bsp:
	ln -sf ../bsp $@                       # ★ BSP 以软链挂进内核树
src/arch/arm64/boot/dts/allwinner/sun60i-a733-cubie-a7a.dts:
	ln -sf ../../../../../../device-a733/configs/cubie_a7a/linux-5.15/board.dts $@
```

内核侧挂载点三处：

| 文件 | 行 | 内容 |
| --- | --- | --- |
| `Kconfig` | 7 | `source "bsp/Kconfig"` |
| `Makefile` | 519 | `-I$(srctree)/bsp/include` |
| `Makefile` | 677 | `drivers-y += bsp/` |

BSP 自身是 overlay 树（根 `Kconfig` 用 `source "$(BSP_TOP)..."`），
不是完整内核。

### 2.4 BSP 里的 AMP 组件

`radxa/allwinner-bsp@cubie-aiot-v1.4.6`：

| 目录 | 内容 |
| --- | --- |
| `drivers/msgbox/sunxi-msgbox.c` | compatible 表含 `allwinner,sun60iw2-msgbox`（:717，复用 `sun55iw5_hwdata`） |
| `drivers/remoteproc/` | `sunxi_rproc.c` + `_boot.c` + `_e906_boot.c` + `_e907_boot.c` + `_hifi4_boot.c` + `_standby.c` + `_trace.c` + `_firmware.c` |
| `drivers/rpmsg/` | `rpmsg_master.c` / `rpmsg_client.c` / `rpmsg_notify.c` / `rpmsg_heartbeat.c` |
| `drivers/rpbuf/` | `rpbuf_core.c` / `rpbuf_dev.c` / `rpbuf_controller_sunxi.c` / `rpbuf_service_rpmsg.c` / `rpbuf_sample_sunxi.c` |

**remoteproc compatible 表对比**：

| 版本 | compatible |
| --- | --- |
| v1.4.6 | `hifi4-rproc` / `e906-rproc` / `e907-rproc` |
| v1.5.0 | 上面三个 + `c906-rproc` / `arm-rtos-rproc` / `arm-baremetal-rproc` |

**两版都没有 `e902-rproc`，也没有 `arm64-rproc`。**

### 2.5 传输栈的真实形态

BSP 的 AMP 通信不是裸 rpmsg，而是三层：

```
业务客户端（vin_rp / isp rp / rpbuf_sample）
     ↓  rpbuf_buffer_va / rpbuf_transmit_buffer
rpbuf_core
     ↓  rpbuf_service_rpmsg.c:46  rpmsg_trysend(rpdev->ept, msg, msg_len)
rpmsg（rpmsg_master.c:808  drv.name = "sunxi,rpmsg_master"
                    :795  id_table = { "sunxi,rpmsg_ctrl" }）
     ↓
rpmsg_virtio over msgbox
     ↓
sunxi-msgbox → 共享内存
```

payload 内存走 `dma_alloc_coherent`（`rpbuf_controller_sunxi.c:45`），
IOMMU 是可选分支（`:128` 读 `iommus` 属性）。

### 2.6 最关键的一条：msgbox 只有两个处理器

`sun55iw5_hwdata`（sun60iw2 复用的那份）：

```c
static const struct sunxi_msgbox_hwdata sun55iw5_hwdata = {
	.processors_max = 2,          /* ★ 只有两个 */
	.channels_max   = 4,
	.fifo_msg_max   = 8,
	.mbox_num_chans = 4,
	.to_coef_n    = { {-1,0,-1,-1}, {0,-1,-1,-1}, {-1,-1,-1,-1}, {-1,-1,-1,-1} },
	.to_remote_id = { { 1,-1,-1,-1}, {0,-1,-1,-1}, {-1,-1,-1,-1}, {-1,-1,-1,-1} },
};
```

推论：

1. **`local_id` 只能取 0 或 1** ⇒ 这颗 SoC 的 msgbox 只能连**两个**处理器。
   一侧是 A 核，另一侧就是 E902/ARISC。**不存在"A核 ↔ 另一颗 A55"的通路**，
   DT 里那个 `a55_rproc@0` 节点没有任何 msgbox 可用。
2. **带宽极小**：`sunxi_msgbox_send_data()` 只写 `*(u32 *)data`，
   即**一条消息 4 字节**，FIFO 深度 8。
   ⇒ msgbox 的正确定位是 **doorbell + 4 字节 header**，payload 必须走共享内存。
3. `to_coef_n` 只定义了 (0,1) 和 (1,0) 两格，通道 1/2/3 在软件层是"未定义"。

### 2.7 板级 DTS 把 msgbox 关了两层

`allwinner-device@device-a733-v1.4.6/configs/cubie_a7a/linux-5.15/board.dts:710`：

```dts
&msgbox {
	status = "disabled";
};
&hwspinlock {
	status = "disabled";
};
```

上游 `sun60iw2p1.dtsi` 里 `msgbox@3004000` 与 `a55_rproc@0` 也**都是 disabled**。

### 2.8 真正的 DT 参数（BSP 原文，非示例值）

```dts
msgbox: msgbox@3004000 {
	compatible = "allwinner,sun60iw2-msgbox";
	#mbox-cells = <1>;
	reg = <0x0 0x03004000 0x0 0x1000>,
	      <0x0 0x07094000 0x0 0x1000>;
	interrupts = <GIC_SPI 0   IRQ_TYPE_LEVEL_HIGH>,
	             <GIC_SPI 211 IRQ_TYPE_LEVEL_HIGH>;
	clocks = <&ccu CLK_MSGBOX0>;
	clock-names = "msgbox";
	resets = <&ccu RST_BUS_MSGBOX0>;
	reset-names = "rst";
	local_id = <0>;
	status = "disabled";
};

a55_rproc: a55_rproc@0 {
	compatible = "allwinner,arm64-rproc";   /* 无驱动认领 */
	status = "disabled";
};
```

对比网上流传的示例：`0x02900000` / `SPI 132` / `#address-cells=<1>` **全错**。

### 2.9 E902 的当前归属

- A733 规格书标注集成 1 颗玄铁 RISC-V E902（200 MHz）。
- BSP 无 E902 rproc 驱动，DT 无 E902 节点。
- DT 中唯一痕迹是 `arisc_config`（`s_uart_config`，PL2/PL3，**实测为空值**）。

> ⚠️ **原先此处的推断「E902 运行原厂 ARISC 电源管理固件」已被 2026-11-18
> 的实测定性推翻**：E902 侧**没有任何能与 A 核通信的活体**。完整实验与
> 证据见 **2.10**。结论摘要：
>
> | 问题 | 实测答案 | 证据强度 |
> | --- | --- | --- |
> | E902 是否向 A 核发过消息 | ❌ **从未**（8 个 chan/proc 组合的 `MSG_ST` 全为 0，静置 10s 不自增） | 只读快照，强 |
> | msgbox A→E902 写通路能否入队 | ✅ **可以**（写入后 `MSG_ST` 递增，`RD_IRQ_ST` 置 pending 位） | 强 |
> | E902 是否在监听 msgbox | ❌ **没有**（两侧 `RD_IRQ_EN=0`，消息 10s 无人取） | 强 |
> | E902 是否在运行 | ⚠️ **无法证明**，也**无任何正面证据** | **弱** |
> | E902 跑什么固件 | ❌ **完全未知**（BSP 无 E902 驱动、DT 无节点、无保留内存） | 强（证否） |
>
> ⇒ 两点必须分清：
>
> 1. **「写进 FIFO 成功」只证明 msgbox 硬件通路正常，不证明对端有活体。**
>    pending 位是 msgbox 控制器置的，与 E902 是否取数无关。此前把这两件事
>    当成一件事，是本节最大的错误。
> 2. **「E902 存活」没有证据支撑**。它的时钟在硬件层是使能的
>    （`r-mbox`/`riscv`/`riscv-24m` 位为 1），但 Linux 侧 `enable 0`、
>    无驱动认领、无固件、无保留内存 —— 时钟开着只说明「没被关掉」，
>    不说明「有人在跑」。

### 2.10 msgbox 通路实测（2026-11-18，决定性实验）

#### 背景：新内核已就位

`5.15.147-100btf1amp-a733` 已编译、上板、运行，**BTF 零损失**：

| 验证项 | 结果 |
| --- | --- |
| `/sys/kernel/btf/vmlinux` | 4.4 MB |
| `/sys/kernel/btf/` 条目数 | 67（含模块 BTF） |
| `CONFIG_DEBUG_INFO_BTF=y` / `_MODULES=y` | 均在 |
| **运行中的 BPF 程序** | **51 个**（eBPF 功能未回退的最强证据） |
| `DEBUG_INFO` 块与旧内核差异 | 仅多 `# CONFIG_DEBUG_INFO_DWARF5 is not set`，**反而更严谨** |
| 配置总差异 | 19 行（全部为 msgbox 及其 Kconfig 依赖项） |
| 业务服务 | server / tunnel / kmod 全 active |
| 回退路径 | `extlinux.conf` 的 `l1` 指向旧 `-99btf1` |

#### 使能方式：不是改 DTS，而是 DT overlay

关键是板子上有一个 **`/boot/dtbo/msgbox-enable.dtbo`**（282 字节），
已挂在 `extlinux.conf` 的 `fdtoverlays` 里：

```dts
/ {
	compatible = "radxa,cubie-a7a\0arm,sun60iw2p1\0allwinner,sun60i-a733";

	fragment@0 {
		target-path = "/soc@3000000";
		__overlay__ {
			msgbox@3004000 { status = "okay"; };
		};
	};
};
```

⇒ **不需要改板级 DTS 源文件**，overlay 只翻转 `status`。
运行中的 DT 节点确认：`soc@3000000/msgbox@3004000`，`compatible=allwinner,sun60iw2-msgbox`，
`local_id=0`，`#mbox-cells=1`，`status=okay`，**无任何 `mboxes` 消费者**。

#### 驱动 probe 成功的四项硬证据

| 证据 | 实测值 |
| --- | --- |
| ioremap 两块地址 | `/proc/iomem`：`03004000-03004fff` 与 `07094000-07094fff` |
| 中断已注册 | **IRQ 432 / 433**（不是 `(0|211)`——`GIC_SPI 0` 映射为 432，`211` 映射为 433） |
| 时钟已使能 | `clk_summary`：`msgbox0` 26 MHz，enabled |
| 驱动已绑定 | `/sys/bus/platform/drivers/sunxi-msgbox/3004000.msgbox` |

> **两个曾误导我的检查方式，记下来避免重犯**：
> 1. `/sys/class/mailbox/` 和 `/sys/bus/mailbox/devices/` 在 5.15 的 mailbox 框架里
>    **本来就不存在**（框架只维护链表，不建 class）。用它们判断 probe 会得到假阴性；
> 2. 按 `/proc/interrupts` 的 `0:` / `211:` 去 grep IRQ **是错的**，
>    `GIC_SPI n` 在 Linux 里的 IRQ 号不是 n，必须按名字 grep。

#### 探针设计与安全边界

**为什么不能用 `/dev/mem` 用户态探针**：实测

```
od -An -tx4 -j 0x03004000 -N 16 /dev/mem
→ read error: Bad address
```

`CONFIG_STRICT_DEVMEM` 未开、`/dev/mem` 存在，但设备内存区已被驱动 `ioremap`，
用户态读不了。**必须走内核模块。**

**为什么探针只轮询、不开中断**：驱动只在 `sunxi_msgbox_startup()` 里置
`RD_IRQ_EN`，而 `startup()` 由 `mbox_request_channel()` 触发——
当前**没有任何 DT 消费者**，所以 `startup()` 从未执行。
若探针自行置位 `RD_IRQ_EN`，驱动的 IRQ handler 会遍历 `controller.chans[]`
并调用 `sunxi_msgbox_read_handler(chip, chan, ...)`，而 `chan->cl` 为 NULL
⇒ **内核 oops**。因此探针**只读状态寄存器并直接写 FIFO**，绕开中断路径。

**探针行为**：`mode=0` 纯只读 dump；`mode=1` 额外写一个 4 字节 magic
（`0xA733C0DE`）到远端 FIFO，然后每 50 ms 轮询 60 次（共 3 秒）。

#### 只读快照（写入前）

```
LOCAL 0x03004000 (A 核, local_id=0)        REMOTE 0x07094000 (E902, remote_id=1)
  RD_IRQ_EN[0] = 0x00000000                  RD_IRQ_EN[0] = 0x00000000
  RD_IRQ_ST[0] = 0x00000000                  RD_IRQ_ST[0] = 0x00000040  ← p=3 有残留 pending
  WR_IRQ_EN[0] = 0x00000000                  WR_IRQ_EN[0] = 0x00000000
  WR_IRQ_ST[0] = 0x000000aa                  WR_IRQ_ST[0] = 0x000000aa
  DBG_REG[0]   = 0x00000000                  DBG_REG[0]   = 0x00000000
  p=0..3 MSG_ST = 0（FIFO 全空）             p=0..3 MSG_ST = 0（FIFO 全空）
```

判读：**两侧都没有使能中断**；`WR_IRQ_ST=0xaa` 与 `REMOTE RD_IRQ_ST=0x40`
都是规律位模式，**均为复位默认值**。`0x40` 曾被我解读成「p=3 曾有消息到达、
pending 未清」——这个解读是错的：它在每次冷启动都精确复现，而 p=3 通道
实测**未实现**（见下），根本不可能是通信痕迹。

#### 写入实验结果（mode=1, send_chan=0）

```
before send, remote FIFO queued = 0          ← 写入前远端 FIFO 是空的
wrote 0xA733C0DE to remote FIFO p=0
remote FIFO changed at t=2250ms (queued=1)   ← 消息确实进了远端 FIFO
t=2300..3000ms 持续 queued=1                 ← 3 秒内无人取走
RESULT got=1  remote_queued=1  local_queued=0
RD_IRQ_ST[0] = 0x40 → 0x41                   ← 硬件为 p=0 置起新消息 pending 位
```

#### ⚠️ 本节曾被一次硬件假象整体带偏（必须记住）

上表的 `queued=1` 一度被解读为「A→E902 通了、E902 收下了」。**这个解读是错的。**

`MSG_ST` 的 `[3:0]` 是一个 **4 位环形计数器，读空 FIFO 会下溢回绕（0 − 1 = 15）**。
于是任何「先读 `MSG_ST`、再读 `MSG_FIFO`」的轮询都会自己制造出「有消息」的假象：
读一次空队列，计数从 0 跳到 15；再读一次变 14……一路递减回 0，再读又跳 15。
它**永远不会报错**，看起来完美地像一个「有 15 条积压」的队列。

`before=0` → 读一次 → `after=15`，这个三段记录是铁证：

```
UNDERFLOW loc p=0 before=0 read=0x00000000 after=15
UNDERFLOW loc p=1 before=0 read=0x00000000 after=15
```

之前从 FIFO 里「读出来的 ASCII」`0x33386430 / 0x31356162 / ...` 解码是
`0d83 / ba51 / c6c5 / 1afe / 2db2` —— 不是文本，是下溢后读到的残留垃圾。
「E902 发来了可读字符串」这个念头必须彻底放弃。

⇒ 详见 `openspec/pitfalls/msgbox-fifo-read-underflow.md`。

#### 重做的决定性实验：严格只读，全程不碰 `MSG_FIFO`

改正后重写探针（`msgbox_probe3.c`），**不写任何寄存器、不读任何数据 FIFO**，
只快照状态寄存器；并在干净启动（`up 103 s`）后连做两次快照、间隔 10 秒。

```
===== T0 =====
LOCAL  0x03004000 RD_IRQ_EN=0x00000000 RD_IRQ_ST=0x00000000 WR_IRQ_EN=0x00000000 WR_IRQ_ST=0x000000aa DBG=0x00000000
REMOTE 0x07094000 RD_IRQ_EN=0x00000000 RD_IRQ_ST=0x00000040 WR_IRQ_EN=0x00000000 WR_IRQ_ST=0x000000aa DBG=0x00000000
  chan0 p0..p3  L_msg=0 R_msg=0  L_fifo=0x00000000 R_fifo=0x00000000  L_thr=0x00000000
  chan1 p0..p3  L_msg=0 R_msg=0  L_fifo=0x00000000 R_fifo=0x00000000  L_thr=0x00000000
===== T+10s =====        （与 T0 逐字段完全相同）
```

#### 四条数据联合出的结论

| # | 事实 | 排除了什么 |
| --- | --- | --- |
| 1 | **A 核本地 8 个 chan/proc 组合 `MSG_ST` 全为 0** | **E902 从未向 A 核发过任何消息** |
| 2 | 两侧 `RD_IRQ_EN = 0` | **E902 侧根本没有在监听**（不是「收到了没处理」，是「没开收」） |
| 3 | 静置 10 秒逐字段不变 | 没有任何后台心跳/周期通信 |
| 4 | `REMOTE RD_IRQ_ST = 0x40` 每次冷启动必现 | 是**复位默认值**，不是 ATF 残留、不是通信痕迹 |

⇒ **没有 E902→A 的通信，过去没有，现在也没有。**

#### 通道 p=3 的「异常」也解释清楚了：它压根没实现

上一轮怀疑 p=3 有残留。做了连续六次写 `0x0709407C` 的对照实验：

```
p=3:  MSG_ST 始终 0      ← 六次写入，计数一次没动
p=1（对照）: 4 → 5 → 6 → 7 → 8 → 9，第 5 次写入后 FIFO_ST 翻成 1
```

⇒ **p=3 通道未实现，写进去被静默丢弃。** 之前看到的 `p=3 got=1` 全是
下溢假象。`RD_IRQ_ST=0x40`（bit6 = p=3 pending）只是复位默认值，与 p=3
是否可用无关。

#### 对方案的影响

| 原假设 | 实测结论 |
| --- | --- |
| msgbox 硬件通路可用（A→E902 入队） | ✅ **确认**（入队、置 pending 位；但**入队**只是控制器行为） |
| E902 向 A 核发过消息 | ❌ **从未**（本地队列结构性全 0） |
| E902 在监听 msgbox | ❌ **没有**（两侧 `RD_IRQ_EN=0`） |
| E902 存在且正在运行 | ⚠️ **无证据**。时钟在硬件层使能但 Linux 侧 `enable 0`、无驱动认领 |
| E902 跑 ARISC PM 固件 | ❌ **不成立**（PM 固件必然要用 msgbox 与 A 核交互，实测零交互） |
| 可「共用 msgbox」做 coexisting（不碰 E902 固件） | ❌ **走不通**：对端没有实现协议，发过去没人读 |

**成功率重新分解**：

- **A→E902 写入通路**：**100% 确认**（硬件层）
- **E902→A 回传**：**从未发生过**，且 E902 侧未监听 ⇒ **当前不存在这条链路**
- **coexisting 双向通信**：**需 E902 侧配合固件**，当前**不存在**

#### 架构层：rpmsg 在这块板子上结构性不可达

不只是「没跑起来」，是**没有可跑的构件**：

| 检查项 | 实测 |
| --- | --- |
| `CONFIG_AW_REMOTEPROC` / `CONFIG_REMOTEPROC` | **not set** |
| `CONFIG_RPMSG_VIRTIO` | **not set** |
| 全部 `CONFIG_AW_RPBUF_*` | **not set** |
| BSP 是否有 E902 boot 驱动 | ❌ **没有**。只有 `AW_REMOTEPROC_HIFI4_BOOT` / `_E906_BOOT` / `_E906_STANDBY` / `_E907_BOOT`；`sunxi_rproc_match[]` = `{hifi4-rproc, e906-rproc, e907-rproc}`，**全库搜不到 e902** |
| sun60iw2 的 dtsi 有无 E902 节点 | ❌ **没有**。只有 `a55_rproc`（`compatible="allwinner,arm64-rproc"`），且 `status="disabled"` |
| sun60iw2 有无 riscv SRAM 保留 | ❌ **没有**。`reserved-memory` 里**只有 `bl31`**（0x48000000 + 16MB） |
| 对比：sun60iw1p1（有协处理器的平台） | `e906_rproc@7130000` + `memory-region=<&riscvsram0_reserved>`；两块 `0x80000` 的 riscv SRAM；msgbox 有**四个**寄存器 bank |

⇒ **sun60iw2 的 BSP 里，协处理器这条线是被整段裁掉的。**
不是配置没开，是**驱动、DT 节点、保留内存三样全缺**。

#### R-CCU 侧：时钟是「开着但没人认领」

```
r-mbox     0x0701017C = 0x00010001   ← BIT(0) gate=1, BIT(16) reset released
riscv      0x0701021C = 0x00010003   ← BIT(0)/BIT(1) gate=1
riscv-24m  0x07010210 = 0x80000000   ← BIT(31) gate=1
```

但 `clk_summary` 里 `riscv` / `riscv-cfg` / `riscv-24m` / `r-mbox` 全是
26 MHz、`enable 0 prepare 0`，硬件位却是 `Y`。

⇒ 解读：**这些时钟是 boot 阶段（boot0/ATF）打开的，之后没有任何 Linux
驱动去 `clk_prepare_enable`**。时钟开着 ≠ 有人在跑。它同样可能是
「本来就这样、没人去关」。

#### 未做的验证（明确边界）

- **E902 是否真的在执行指令**：仍**无法证明**。需要它的 CFG/version 寄存器，
  而 sun60iw2 的该基址未知；**此前盲扫 R 域地址直接把板子扫挂了**，不再尝试。
- 未测 E902→A 方向（无法从 A 核侧触发，且 E902 侧未监听）。
- 未开 ARISC 串口（`s_uart_config.status = "disabled"`，空值，需改 DT 才有日志）。
- **E902 到底跑什么固件，仍然未知。**

---

## 三、三条路线的真实边界

### 路线 A：完全接管 E902（替换 ARISC 固件）

从 Tina SDK 抽 E902 rproc 适配 + 自己烧 Melis/裸机 OpenAMP。

| 项 | 评估 |
| --- | --- |
| 前提 | 拿得到 Tina SDK 的 E902 固件源码 |
| 代价 | 电源管理、休眠唤醒全部重写 |
| 风险 | 废掉原厂 PM 栈，可能整机不稳定 |
| 结论 | **不建议**。投入产出比极低，且与"诊断网关"产品目标无关 |

### 路线 B：coexisting（不碰 E902 固件，共用 msgbox）

**这是用户提出的"低风险路线"，本方案认为方向正确，但需要一处修正。**

原始表述是"前提是能拿到 ARISC 固件的扩展接口，否则被原厂黑盒卡脖子"。

**修正**：真正的硬约束不是"协议文档"，而是**地址视图**。

理由：

1. msgbox 是**双向对称**的（`to_coef_n` 已给出两个方向的独立映射），
   ARISC 并不知道你这一侧是否上线；
2. 你完全可以在 header 里带完整自描述（magic + 共享内存偏移 + 长度 + CRC），
   ARISC 侧即使不认识也不会崩（FIFO 不是共享状态机，最坏是被忽略）；
3. **但 payload 必须落在 E902 能访问的物理地址上** —— 这需要知道
   "E902 能看到哪段 DDR"，这是 SoC 手册问题，不是固件接口问题。

| 项 | 评估 |
| --- | --- |
| 前提 | SoC 手册给出 E902 地址视图；msgbox 通道 1/2/3 未被 ARISC 物理占用 |
| 代价 | 中等：不碰 PM，但要实现一侧协议栈 |
| 风险 | 通道冲突；reserved-memory 需自行划区（板端目前只有 bl31） |
| 结论 | **首选**，但必须先做归属探测 |

### 路线 C：只验 msgbox 驱动（不算 AMP）

不启 remoteproc，不碰 E902，只把 `sunxi-msgbox` probe 起来。

| 项 | 评估 |
| --- | --- |
| 用途 | 验证驱动能否在这块板上 request_irq、拿到时钟复位 |
| 代价 | 极低（一个 DT overlay + modprobe） |
| 风险 | 若 ARISC 正用该通道，可能有干扰 |
| 结论 | **推荐作为第一步**，信息量最大 |

---

## 四、分阶段执行计划

### 4.1 阶段 0：归属探测（只读，不编译，不改 DT）

板子在线后执行：

```bash
# GIC 里 SPI 0 / 211 有没有在计数
cat /proc/interrupts | grep -E '^\s*(0|211):'

# msgbox 两块寄存器区有没有被 ioremap（间接证据）
cat /proc/iomem | grep -E '3004000|7094000'

# arisc_config 当前状态
ls /proc/device-tree/arisc_config/
```

**判读**：

- `SPI 0` 或 `211` 有计数 ⇒ ARISC 正在通信 ⇒ 路线 B 需先摸清它用哪条通道；
- 均为 0 ⇒ 链路静默 ⇒ 风险更低。

### 4.2 阶段 1：msgbox 驱动验证（路线 C）

用 DT overlay 打开 `&msgbox`，**只 modprobe `sunxi-msgbox`**，
不加载 remoteproc / rpmsg。

验收：

```
dmesg | grep -i msgbox                 # probe 成功
ls /sys/bus/mailbox/devices/           # 出现 controller
cat /proc/interrupts | grep 211        # request_irq 成功
```

失败即停——说明硬件归属与推测不符，回阶段 0 重查。

### 4.3 阶段 2：内核增量编译（保留 BTF + 双内核）

**前置约束（用户明确要求）**：

- **BTF 配置一个字节都不能变**；
- 在现有 `5.15.147-99btf1-a733` config 上做增量；
- 新内核**新增 extlinux 条目**，保留现有内核。

#### 4.3.1 前提：现有 `-99btf1` 的 BTF 块**本来就是对的**，必须原样继承

先立一条不可动摇的基线：**`5.15.147-99btf1-a733` 的 BTF 配置已经可用，它是本次
增量的起点，一个字节都不改。**

实测两版 config 的 DEBUG_INFO 块：

| 内核 | DEBUG_INFO 块 | 状态 |
| --- | --- | --- |
| `-21`（官方出厂） | `# CONFIG_DEBUG_INFO is not set` | 无 BTF |
| `-99btf1`（当前运行） | `DEBUG_INFO=y` / `DWARF4=y` / `BTF=y` / `BTF_MODULES=y` | **BTF 可用** |

两个 config 的**全部差异只有 9 行**（BTF 那几行 + `PAHOLE_VERSION 0→120`）。
⇒ 上次重编内核**没有改任何业务源码，只把 BTF 打开了**。这个块就是我们要保留的成果。

#### 4.3.2 陷阱在哪里：走官方打包路径会把 BTF 重新关掉

`radxa-pkg/linux-a733/debian/patches/linux/0001-feat-Radxa-common-kernel-config.patch:254-259`：

```
+# Disable debug kernel
+CONFIG_DEBUG_INFO_NONE=y
+CONFIG_DEBUG_INFO=n
+CONFIG_DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT=n
+CONFIG_DEBUG_INFO_REDUCED=n
```

而 5.15 的 Kconfig 依赖链：

```
DEBUG_INFO_BTF → depends on DEBUG_INFO → depends on !DEBUG_INFO_NONE
```

⇒ **只要 `DEBUG_INFO_NONE=y` 还在，加 `CONFIG_DEBUG_INFO_BTF=y` 会被静默吞掉**，
`olddefconfig` 不报错、不警告，编完才发现 eBPF 全废。

这个陷阱的含义是：**危险不在\"忘了开 BTF\"，而在\"换了一条构建路径，把已开好的又关掉了\"。**

#### 4.3.3 两条增量路径，只能走第一条

| | 路径 1：以运行中内核 config 为基准（**推荐**） | 路径 2：以 radxa-pkg 官方打包为基准 |
| --- | --- | --- |
| 起点 | `cp /boot/config-5.15.147-99btf1-a733 .config` | 上游 defconfig + 三个 patch + fragment |
| BTF 块 | **原样继承，无需改动** | patch 先设 `DEBUG_INFO_NONE=y`，必须**重新构造**整个 BTF 块 |
| 风险 | 无（不碰 BTF） | 重构过程中任何一行写错都静默失效 |
| 结论 | **用这条** | 仅在必须复现官方 deb 打包时考虑 |

路径 1 的做法——**只做加法，绝不动 DEBUG_INFO 块**：

```bash
cp /boot/config-5.15.147-99btf1-a733  .config.orig
cp .config.orig .config

# 最小集：只开 msgbox（路线 B / E902 见证者只需要这个）
scripts/config -e MAILBOX -e AW_MSGBOX

# 若要完整 rpmsg 栈，再追加（注意 RPMSG_VIRTIO 会额外拉入 VIRTIO）
# scripts/config -e REMOTEPROC -e AW_REMOTEPROC \
#                -e RPMSG -e RPMSG_VIRTIO -e RPMSG_CHAR \
#                -e AW_RPBUF_CONTROLLER_SUNXI -e AW_RPBUF_SERVICE_RPMSG

make olddefconfig
```

**★ 守卫必须做块级比对，不是只查两个符号。** 只 grep `DEBUG_INFO_BTF` 与
`DEBUG_INFO_NONE` 挡不住\"DWARF 档位被改\"这类漂移：

```bash
# 把 DEBUG_INFO 整块提出来逐字节比对——任何一行变了就停
blk() { grep -E '^(# )?CONFIG_DEBUG_INFO' "$1"; }
if ! diff <(blk .config.orig) <(blk .config) > /tmp/btf.diff; then
    echo "BTF 块被改动，停："; cat /tmp/btf.diff; exit 1
fi
echo "BTF 块逐字节未变 ✔"

# 还要确认 BTF 仍在（防止块没变但被依赖链挤掉）
grep -q '^CONFIG_DEBUG_INFO_BTF=y'         .config || { echo "BTF 丢了，停"; exit 1; }
grep -q '^CONFIG_DEBUG_INFO_BTF_MODULES=y' .config || { echo "模块 BTF 丢了，停"; exit 1; }

# 再确认新开的符号真的进来了
for s in MAILBOX AW_MSGBOX; do
    grep -q "^CONFIG_$s=y" .config || { echo "$s 没开成，停"; exit 1; }
done

# 全量 diff 只允许预期增行（依赖自动拉入的也算"预期"）
diff <(sort .config.orig) <(sort .config) | grep '^>'
```

**必须预检**：BTF 既然继承为 `=y`，构建时**一定**要跑 `pahole` 生成 BTF。
板端 `pahole` 未安装，但 config 记着 `CONFIG_PAHOLE_VERSION=120` ⇒ 上次构建的机器
上有 pahole。构建机没有它时，BTF 生成会失败或静默降级，这是第二个失败点，
编之前先 `which pahole && pahole --version`。bullseye-backports 有 1.22。

**编完后的产物验证**（比 config 断言更硬）：

```bash
ls -l vmlinux | awk '{print $5}'          # 应远大于无 BTF 版本
readelf -S vmlinux | grep -c BTF          # .BTF / .BTF.ext 必须在
pahole --btf_encode_detached=/tmp/t.btf vmlinux && echo "BTF 可解析 ✔"
```

#### 4.3.4 双内核与版本号

vermagic 来自版本字符串（`CONFIG_LOCALVERSION=""`，`LOCALVERSION_AUTO` 未开）。
所以只有两条路：

| 方案 | 版本号 | dpkg | 外部模块 | 回滚 |
| --- | --- | --- | --- | --- |
| **A（推荐）** | 改，如 `5.15.147-100btf1amp` | **并存** | `bsp/` 随包自带；**DKMS 5 个要重建** | 干净 |
| B | 不变 `99btf1` | **覆盖** | 全不用重建 | 手工备份，很脏 |

**推荐 A**，且"B 的好处其实不存在"：`kernel/bsp/drivers/vin/vin_v4l2.ko.xz`
是内核编译产物，换内核必然随之更新；真正独立的只有
`/lib/modules/<ver>/updates/dkms/` 那五个：

```
aic_btusb_usb / aic_load_fw_usb / aic8800_fdrv_usb / radxa-overlays / pvrsrvkm
```

#### 4.3.5 ⚠️ 致命依赖：网络挂在这五个 DKMS 模块里

`aic8800_fdrv` 是 WiFi 驱动，而板子是通过 **`127.0.0.1:22222` 反向隧道**访问的。
新内核开机后 DKMS 未重建 ⇒ **没有 WiFi ⇒ 板子失联，只能上串口**。

顺序必须是：

```bash
dpkg -i linux-image-<new>-a733.deb linux-headers-<new>-a733.deb
cat /boot/extlinux/extlinux.conf | grep -E 'default|menu label'   # 默认仍是旧内核？
dkms autoinstall -k <new>                                          # 不成功别重启
ls /lib/modules/<new>/updates/dkms/
```

另两点：

- `extlinux.conf` 头部写着「Do not edit this file manually, use: u-boot-update」——别手改。
- 板有硬件看门狗：新内核起来但 `weaknet-server` 没起，**16 秒整机复位**，会成复位循环。

#### 4.3.6 构建环境：Windows WSL2（已就位，直接复用）

> **本节是给在 Windows 上执行构建的人/AI 看的完整移交说明。**
>
> **重要结论：不需要从零搭建。** 上一次编译 `5.15.147-99btf1` 的整套环境与产物
> 都还在 Windows 的 WSL2 里，本次只做**增量编译**。

##### 为什么不在开发服务器上编

原开发服务器只有 **3.6 GB 内存 / 12 GB 可用磁盘**，而带 BTF 的 ARM64 内核
构建需要 **20–25 GB 磁盘 + 8 GB 以上内存**。结论不是"勉强能跑"，是**不可能完成**。

##### 为什么是 WSL2，不是 Docker Desktop

**结论：走 WSL2。Docker Desktop 在本任务上不是等价选项，是倒退。**

**决定性理由：既有产物就躺在 WSL 的 ext4 里。**

上一次编 `5.15.147-99btf1` 的整套资产（源码树、已编译的 `.o`、300 MB 的
`vmlinux`、`Image`、deb、完整构建日志）全在 `~/linux-a733` 这个 WSL 发行版内。
换 Docker Desktop 意味着这些要么跨层挂载进去、要么重编几小时——
**没有收益，只有新增风险**。

**Docker Desktop 的三处实际劣势：**

| 劣势 | 后果 |
| --- | --- |
| 文件系统多一层（Windows FS → WSL → Docker utility VM） | 内核构建是几十万次小文件操作，逐次放大开销 |
| `.wslconfig` 对它不生效 | 内存上限在它自己的 Settings → Resources，是另一套；混用会得到"以为给了 12 GB、实际只给 4 GB"的假象，在 `-j8` 链接 vmlinux 时被 OOM killer 杀 |
| uid / 权限翻译 | WSL 里 `tqf` 是 uid 1000，绑进容器常变成 root 或 nobody，`make` 产出属主错乱 |

> **⚠️ 一个必须澄清的历史事实**：文档里说"上次是在 devcontainer 里编的"
> （`/workspaces/linux-a733` + devenv/Nix），**这不等于 Docker Desktop**。
> VS Code 的 devcontainer 跑在 WSL 里，`/workspaces` 通常就是 WSL 文件的 bind mount。
> 但由此引出一个**尚未验证**的点：`devenv` / Nix 装在 WSL 主机上，
> 还是只装在 devcontainer 镜像里？若只在镜像里，WSL 主机直接跑 `make`
> 会找不到 `pahole`。

**所以 WSL 侧第一条该执行的命令是环境探测，不是编译：**

```bash
which devenv nix pahole aarch64-linux-gnu-gcc
```

| 探测结果 | 走法 |
| --- | --- |
| 全都有 | **直接在 WSL 里编**，最省事（主路径） |
| 缺 `pahole` / `nix` | 回到 devcontainer 里编（仍在 WSL 文件系统上，可接受） |
| 只缺 `pahole` | `sudo apt install -y dwarves` |

**总原则**：WSL2 原生是主路径；devcontainer 仅作 Nix 环境的兜底；
Docker Desktop 不在选项里。

##### 当前 Windows 真实环境（2026-11-17 实测）

| 项 | 值 |
| --- | --- |
| 发行版 | **Ubuntu-22.04**（默认，用户名 `tqf`） |
| 虚拟磁盘 | 已迁移到 **`E:\WSL\Ubuntu-22.04`**（`wsl --manage ... --move`，注册表 BasePath 已更新） |
| WSL 版本 | 2.7.11.0，内核 `6.18.33.2-microsoft-standard-WSL2` |
| WSL 内核二进制 | `C:\Program Files\WSL\tools\kernel`（**微软官方，未自定义**） |
| Docker Desktop | 数据盘经 **NTFS 目录联接**迁到 `E:\WSL\Docker\disk`（`mklink /J`） |
| C 盘剩余 | **55 GB**（迁移前 38.2 GB） |
| E 盘剩余 | **712 GB** |
| `.wslconfig` | **不存在**（见下方"必须先建"） |

> ⚠️ **概念澄清**：`5.15.147-99btf1` **不是 WSL 内核**，而是**交叉编译出来的
> Radxa A733 arm64 内核**。WSL 自身用的是微软官方内核，两者无关。
> `/usr/src` 为空、`/lib/modules` 只有微软模块，都是**正常现象**。

##### 既有资产清单（本次直接复用，不要重建）

| 资产 | 路径 | 说明 |
| --- | --- | --- |
| 内核源码树 | `~/linux-a733`（即 `\\wsl$\Ubuntu-22.04\home\tqf\linux-a733`） | 来自 `radxa-pkg/linux-a733`，已含三个 submodule |
| 构建产物 | `~/linux-a733/src/arch/arm64/boot/{Image,Image.gz}` | `Image` 约 26 MB |
| **带 BTF 的 vmlinux** | `~/linux-a733/src/vmlinux` | **约 300 MB** ⇒ BTF 确实编进去了（无 BTF 通常仅 30–80 MB） |
| deb 安装包 | `~/linux-a733/src/*.deb` | 上次 `bindeb-pkg` 产出，版本 `5.15.147-99btf1` |
| 产物副本 | `~/a733-btf-release/` | 备份，**空间紧张时可删** |
| **BTF 改动凭据** | `~/linux-a733-btf-config.patch` | 上次开 BTF 的原始 patch，**权威基线** |
| 构建日志 | `~/linux-a733/build-final.log` | 上次 `make bindeb-pkg` 完整日志，lintian 通过 |

##### 上一次是怎么编的（历史事实）

- **时间**：2026-07-28 ~ 07-29
- **方式**：WSL 内 devcontainer（`/workspaces/linux-a733`）+ **devenv / Nix** 提供工具链
- **命令**：`make bindeb-pkg`
- **BTF 改动**：`~/linux-a733-btf-config.patch` 把 `CONFIG_DEBUG_INFO_NONE=y`
  换成 `CONFIG_DEBUG_INFO=y` + `CONFIG_DEBUG_INFO_DWARF4=y` + `CONFIG_DEBUG_INFO_BTF=y`
- **交叉编译**：曾用 `qemu-aarch64-static`（历史记录），但**内核本身是纯交叉编译**，
  QEMU 只在早期用户态环节用到

> **⇒ 本次不需要装 pahole、不需要装交叉工具链。** 上次能编成功，说明这套环境里
> 该有的都有（Nix 提供）。**先验证，不要盲目 apt install 破坏 Nix 环境。**

##### ⚠️ 必做前置：创建 `.wslconfig`（当前不存在）

`memory` 不设限时 WSL2 默认只用物理机内存的一半，编内核并行编译会 OOM。
在 **Windows PowerShell**（不是 WSL 里）执行：

```powershell
notepad "$env:USERPROFILE\.wslconfig"
```

写入（按物理机实际内存调整，`memory` 建议为物理内存的 60–70%）：

```ini
[wsl2]
memory=12GB
processors=8
swap=8GB
```

保存后 `wsl --shutdown` 重开生效。验证：

```bash
free -h          # Mem total 应接近 12Gi
nproc            # 应等于设置的核心数
```

##### ⚠️ 两个必须遵守的硬约束

**约束一：绝对不要在 `/mnt/c`、`/mnt/e` 等 Windows 盘上构建。**

WSL2 访问 Windows 磁盘走 DrvFs，小文件 I/O 比 ext4 慢一个数量级。内核构建涉及
几十万次小文件操作，放 `/mnt/*` 会让 1 小时的构建变成数小时。
**源码与构建目录必须在 WSL 的 ext4 里**——当前 `~/linux-a733` 就在 ext4 上，**正确，别移动**。

**约束二：vmlinux（300 MB）很吃空间，删旧产物前先确认。**

vhdx 已迁到 E 盘（712 GB 可用），空间充裕。但**vhdx 只增不减**，
删文件不会自动返还空间。真需要回收时用 PowerShell：

```powershell
wsl --shutdown
Optimize-VHD -Path "E:\WSL\Ubuntu-22.04\ext4.vhdx" -Mode Full   # 需 Hyper-V 模块
```

或用 `diskpart` 的 `compact vdisk`。

#### 4.3.7 增量构建流程（WSL2 内逐条执行，复用既有树）

> 全部命令在 **WSL2 的 Ubuntu-22.04** 里执行，**不在 PowerShell 里执行**。

##### 步骤 0：确认既有资产完好 + 工具链可用（只读，1 分钟）

```bash
cd ~/linux-a733
ls -l src/arch/arm64/boot/Image src/vmlinux 2>/dev/null
ls -l ~/linux-a733-btf-config.patch
git -C src log --oneline -1 2>/dev/null || echo "(src 非 git 仓库或无提交)"
df -h ~ | tail -1

# 工具链探测：决定原地编还是回 devcontainer
which devenv nix pahole aarch64-linux-gnu-gcc || true
```

**判据一（资产）**：`Image` 约 26 MB、`vmlinux` 约 300 MB。任一缺失说明树被清过，
转本节末尾「附注：完整重建」。

**判据二（工具链）**：按 4.3.6 末尾的探测表走——全都有就原地编，
缺 `pahole`/`nix` 回 devcontainer，只缺 `pahole` 装 `dwarves`。
**工具链没验清就往下走，会在步骤 6 链接阶段才炸，白等 20 分钟。**

##### 步骤 1：读 BTF patch，确认改动落在哪一层

```bash
cat ~/linux-a733-btf-config.patch
```

**这一步决定增量加在哪里**，三种可能：

| patch 改的是 | 增量应加在 | 备注 |
| --- | --- | --- |
| `src/.config` | 直接 `scripts/config` 改 `.config` | 最简单，但 `bindeb-pkg` 会重新 defconfig |
| `src/arch/arm64/configs/radxa.config` | 改同一文件 | 打包路径会用它 |
| 新增 `radxa_custom.config` fragment | 加到同一 fragment | `Makefile.local` 里它**最后合并、优先级最高** |

> **务必把 patch 原文发回规划方确认**，不要凭猜。

##### 步骤 2：确认当前 `.config` 与板端一致

```bash
cd ~/linux-a733/src
grep -E '^(# )?CONFIG_DEBUG_INFO' .config
grep -E '^CONFIG_LOCALVERSION' .config
grep -E '^CONFIG_(MAILBOX|AW_MSGBOX)' .config    # 应均为 "is not set"
```

**判据**（必须与板端 `/boot/config-5.15.147-99btf1-a733` 逐行一致）：

```
CONFIG_DEBUG_INFO=y
# CONFIG_DEBUG_INFO_REDUCED is not set
# CONFIG_DEBUG_INFO_COMPRESSED is not set
# CONFIG_DEBUG_INFO_SPLIT is not set
# CONFIG_DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT is not set
CONFIG_DEBUG_INFO_DWARF4=y
CONFIG_DEBUG_INFO_BTF=y
CONFIG_DEBUG_INFO_BTF_MODULES=y
```

**且绝对没有 `CONFIG_DEBUG_INFO_NONE`。**

##### 步骤 3：基线落盘 + 只做加法

```bash
cd ~/linux-a733/src
cp .config .config.orig          # ★ 基线，后面所有断言都拿它比

# 最小集：只开 msgbox（路线 B / E902 见证者只需要这个）
scripts/config -e MAILBOX -e AW_MSGBOX

# 若要完整 rpmsg 栈，再追加（RPMSG_VIRTIO 会额外拉入 VIRTIO）
# scripts/config -e REMOTEPROC -e AW_REMOTEPROC \
#                -e RPMSG -e RPMSG_VIRTIO -e RPMSG_CHAR \
#                -e AW_RPBUF_CONTROLLER_SUNXI -e AW_RPBUF_SERVICE_RPMSG

make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
```

> 若步骤 1 发现 patch 走的是 `radxa.config` / `radxa_custom.config` 路线，
> 则**改文件而不是改 `.config`**，再跑 defconfig 让其生效。

##### 步骤 4：核心守卫 —— BTF 块必须逐字节未变

**任何一行变了就停，不要编译。**

```bash
cd ~/linux-a733/src

# 4.1 DEBUG_INFO 整块比对（不是只查两个符号！）
blk() { grep -E '^(# )?CONFIG_DEBUG_INFO' "$1"; }
if ! diff <(blk .config.orig) <(blk .config) > /tmp/btf.diff; then
    echo "❌ BTF 块被改动，停："; cat /tmp/btf.diff; exit 1
fi
echo "✅ BTF 块逐字节未变"

# 4.2 确认 BTF 仍启用（防止块没变但被依赖链挤掉）
grep -q '^CONFIG_DEBUG_INFO_BTF=y'         .config || { echo "❌ BTF 丢了"; exit 1; }
grep -q '^CONFIG_DEBUG_INFO_BTF_MODULES=y' .config || { echo "❌ 模块 BTF 丢了"; exit 1; }
echo "✅ BTF / BTF_MODULES 均在"

# 4.3 确认新符号真的进来（防止依赖不满足被静默丢弃）
for s in MAILBOX AW_MSGBOX; do
    grep -q "^CONFIG_$s=y" .config || { echo "❌ $s 没开成"; exit 1; }
done
echo "✅ MAILBOX / AW_MSGBOX 已启用"

# 4.4 全量 diff 只允许预期增行
diff <(sort .config.orig) <(sort .config) | grep '^>'
```

> **为什么必须块级比对**：只 grep `DEBUG_INFO_BTF` 与 `DEBUG_INFO_NONE`
> 挡不住「DWARF 档位被改」这类漂移。例如 `DWARF4` 变成
> `DWARF_TOOLCHAIN_DEFAULT`，BTF 仍在但产物布局已变。

> **已知的正常差异**：`CONFIG_PAHOLE_VERSION` 可能因构建机 pahole 版本不同而变
> （板端是 `120`）。它不在 `CONFIG_DEBUG_INFO` 前缀下，上面的块比对抓不到，
> 属**无害差异**，不必处理。

##### 步骤 5：改版本号（决定双内核能否共存）

**当前板子跑 `5.15.147-99btf1-a733`。新内核必须换版本号**，否则 dpkg 覆盖旧包，
失去回退路径。

`Makefile.extra` 里：

```make
LOCALVERSION=-$(shell dpkg-parsechangelog -S Version | cut -d "-" -f 2)-$(KERNEL_FORK)
```

⇒ 版本来自 `debian/changelog`。改它：

```bash
cd ~/linux-a733
head -3 debian/changelog        # 确认当前是 5.15.147-99btf1

cat > /tmp/newentry <<'EOF'
linux-a733 (5.15.147-100btf1amp) unstable; urgency=medium

  * 增量开启 CONFIG_MAILBOX + CONFIG_AW_MSGBOX（AMP msgbox 通道）
  * BTF 配置与 99btf1 逐字节一致

 -- tqf <tqf@localhost>  Mon, 17 Nov 2026 00:00:00 +0000

EOF
cat /tmp/newentry debian/changelog > /tmp/cl.new && mv /tmp/cl.new debian/changelog
head -8 debian/changelog
```

> ⚠️ **换版本号的必然代价**：`LOCALVERSION` 变 ⇒ `utsrelease.h` 变
> ⇒ **所有模块 vermagic 改变、全部要重编**。目录级 `.o` 可复用，
> 但模块层重编仍需 10–20 分钟。不可避免，除非接受覆盖安装（不推荐）。

##### 步骤 6：增量编译（先只编 Image + dtbs 做最小验证）

```bash
cd ~/linux-a733/src
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) Image dtbs
```

约 **10–25 分钟**（增量，大部分 `.o` 已存在）。

成功**立即验证 BTF 段**，别急着打包：

```bash
readelf -S vmlinux | grep -E '\.BTF|\.BTF\.ext'    # 至少两行
pahole --btf_encode_detached=/tmp/t.btf vmlinux && echo "✅ BTF 可解析"
ls -l arch/arm64/boot/Image                        # 应与 26 MB 同量级
```

**`.BTF` 段不存在 ⇒ 立即停。** config 对了不等于产物对了。

##### 步骤 7：打包成 deb

```bash
cd ~/linux-a733/src
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) bindeb-pkg
```

约 **20–40 分钟**（重新链接 + 压缩模块）。

**产出**在 `~/linux-a733/`（src 的上一级）：

```
linux-image-5.15.147-100btf1amp-a733_*.deb
linux-headers-5.15.147-100btf1amp-a733_*.deb
```

**验证包内确实含 BTF 与 BSP 驱动**：

```bash
cd ~/linux-a733
dpkg -c linux-image-*-100btf1amp-*.deb | head -20
dpkg -x linux-image-*-100btf1amp-*.deb /tmp/chk
ls -l /tmp/chk/boot/vmlinuz-*
ls /tmp/chk/lib/modules/*/kernel/bsp/drivers/     # BSP 驱动应存在
```

##### 步骤 8：工具链与 pahole 的真实情况

**不要盲目 `apt install dwarves`。** 上次构建由 **Nix/devenv** 提供 `pahole`，
系统 apt 未装。若步骤 6 报 `pahole: command not found`：

```bash
# 优先恢复 Nix 环境（上次成功路径，版本一致）
cd ~/linux-a733 && devenv shell
pahole --version

# 或（会污染系统环境，二选一）
sudo apt install -y dwarves
```

##### 步骤 9：把产物带出 WSL

产物在 WSL 里，板子只能通过开发服务器的 `ssh board` 访问。

```bash
ls -lh ~/linux-a733/*.deb

# 方式 A：WSL → 开发服务器 → 板子
scp ~/linux-a733/linux-image-5.15.147-100btf1amp-a733_*.deb \
    ~/linux-a733/linux-headers-5.15.147-100btf1amp-a733_*.deb \
    <开发服务器用户>@<开发服务器地址>:~/kbuild/

# 方式 B：Windows 侧从 \\wsl$ 直接拷（不走网络）
#   \\wsl$\Ubuntu-22.04\home\tqf\linux-a733\*.deb
```

> **image 与 headers 两个 deb 必须一起带走**——headers 是板端重建 DKMS 的前提，
> 缺了就用不了 WiFi。

##### 附注：若发现源码树已被清空（步骤 0 失败）

才需要完整重建：

```bash
mkdir -p ~/kbuild && cd ~/kbuild
git clone --depth 1 https://ghproxy.net/https://github.com/radxa-pkg/linux-a733.git
cd linux-a733
git submodule update --init --depth 1 bsp src device-a733

# BSP 挂载（.github/local/Makefile.local 的 pre_build，最易漏）
ln -sf ../bsp src/bsp
ln -sf ../../../../device-a733/configs/default/linux-5.15/bsp_defconfig \
       src/arch/arm64/configs/bsp.config
ln -sf ../../../../../../device-a733/configs/cubie_a7a/linux-5.15/board.dts \
       src/arch/arm64/boot/dts/allwinner/sun60i-a733-cubie-a7a.dts
cp -aR bsp/configs/linux-5.15/*.dtsi src/arch/arm64/boot/dts/allwinner/
cp -aR bsp/include/dt-bindings/. src/include/dt-bindings/

# 基线 config 从板子取 /boot/config-5.15.147-99btf1-a733，再走步骤 1 起流程
```

> GitHub 直连不通时，把 `https://github.com/` 换成
> `https://ghproxy.net/https://github.com/` 或 `https://gh-proxy.org/https://github.com/`。

**image 与 headers 两个 deb 必须一起带走**——headers 是板端重建 DKMS 的前提，
缺了就用不了 WiFi。

##### 步骤 10：上板（含失联风险，顺序不可调换）

> ⚠️ **做错这一步板子会失联，只能接串口抢救。**

```bash
# 1. 先建维护标记（板子有硬件看门狗，16 秒超时）
ssh board 'sudo touch /var/lib/weaknet/health-guard-maintenance'

# 2. 传包
scp linux-image-5.15.147-100btf1amp-a733_*.deb \
    linux-headers-5.15.147-100btf1amp-a733_*.deb board:~/

# 3. 装包（两个都要）
ssh board 'sudo dpkg -i ~/linux-image-5.15.147-100btf1amp-a733_*.deb \
                        ~/linux-headers-5.15.147-100btf1amp-a733_*.deb'

# 4. 确认默认启动项仍是旧内核（别手改 extlinux.conf）
ssh board 'grep -E "default|menu label" /boot/extlinux/extlinux.conf'

# 5. ★ 重建 DKMS —— 不做这步别重启
ssh board 'sudo dkms autoinstall -k 5.15.147-100btf1amp-a733'
ssh board 'ls /lib/modules/5.15.147-100btf1amp-a733/updates/dkms/'
# 期望五个：aic_btusb_usb / aic_load_fw_usb / aic8800_fdrv_usb
#           radxa-overlays / pvrsrvkm

# 6. 重启
ssh board 'sudo reboot'
```

**为什么第 5 步致命**：`aic8800_fdrv` 是 WiFi 驱动，而板子经反向隧道访问。
DKMS 未重建 ⇒ 无 WiFi ⇒ 板子失联。

**重启后验证**：

```bash
ssh board 'uname -r'                                  # 应为 5.15.147-100btf1amp-a733
ssh board 'ls -l /sys/kernel/btf/vmlinux'             # BTF 必须存在
ssh board 'ls /sys/bus/mailbox/devices/ 2>/dev/null'  # msgbox 是否 probe 成功
ssh board 'sudo dmesg | grep -i msgbox'
```

**回滚**：`extlinux.conf` 里选旧 `-99btf1` 条目即可，两个内核并存互不影响。

#### 4.3.7.1 开发板 SSH 访问方式（Windows 侧必读）

##### 核心事实：板子没有公网地址，只经反向隧道可达

板子（Radxa Cubie A7A）挂在无固定 IP 的局域网上，**Windows 电脑无法直连**。
它自己维持一条到开发服务器的 **反向 SSH 隧道**，开发服务器上 `127.0.0.1:22222`
即板子的 22 端口。

| 项 | 值 |
| --- | --- |
| 开发板主机名 | `radxa-cubie-a7a.local`（mDNS/Avahi，**仅同局域网可解析**） |
| SSH 用户名 | `radxa` |
| 板端端口 | `22` |
| 从开发服务器访问 | `ssh board`（即 `127.0.0.1:22222`） |
| 板端路径 | `/home/radxa/weaknet/`（服务）、`/boot/`（内核） |

##### 三种可用链路（按推荐顺序）

**链路 A（推荐）：WSL → 开发服务器 → 板子**

Windows 的 WSL 能 SSH 到开发服务器，再用服务器的跳板访问板子：

```bash
# 一次性配置 ~/.ssh/config（在 WSL 里）
cat >> ~/.ssh/config <<'EOF'

Host devserver
    HostName <开发服务器IP>
    User <开发服务器用户>
    # 密钥或密码按实际情况

Host board
    HostName 127.0.0.1
    Port 22222
    User radxa
    ProxyJump devserver
EOF
chmod 600 ~/.ssh/config

# 之后直接用
ssh board
scp file.deb board:~/
```

> **为什么用 `ProxyJump`**：它让 `127.0.0.1:22222` 在**开发服务器的上下文**里解析，
> 而不是在你本机。这是唯一不需要在 Windows 上开端口转发的方式。

**链路 B：Windows 直连板子（仅同一局域网时可用）**

若 Windows 与板子在同一网段（同一路由器 / 同一热点）：

```powershell
ssh radxa@radxa-cubie-a7a.local
```

`.local` 是 mDNS 链路本地组播，**不能跨网段/路由器**。跨网段必定解析失败，
这是拓扑限制，不是配置错误。

**链路 C：开发服务器代为执行（最省事）**

把命令交代给开发服务器，由它执行：

```bash
ssh devserver 'ssh board "uname -r"'
```

##### ⚠️ 两个必须知道的坑

**坑一：`ssh board` 别名必须在开发服务器的 `radxa` 用户下、不带 sudo。**

用 `sudo ssh board` 会切到 root 的 `known_hosts`，别名立刻失效并报
`Host key verification failed`。同理，在远端执行的提权操作要写成
`ssh board 'sudo ...'`（sudo 在板子那一侧执行），而不是在外层 sudo。

**坑二：板子有硬件看门狗，16 秒整机复位。**

停止或重启 `weaknet-server` 之前必须建维护标记，否则判据失败 → 守卫停止喂狗 →
`sunxi_wdt` 超时 → **整机复位**：

```bash
ssh board 'sudo touch /var/lib/weaknet/health-guard-maintenance'
# ... 操作 ...
ssh board 'sudo rm /var/lib/weaknet/health-guard-maintenance'
```

##### 如果板子已经失联，怎么救

1. 确认隧道是否还活着：`ssh devserver 'ss -lntp | grep 22222'`
2. 若隧道断了但板子在同一局域网：走链路 B
3. 若都不可达：**接串口**（UART），或插显示器+键盘本地登录，
   在 `extlinux.conf` 里选回旧内核

---

#### 4.3.8 上板命令（板端本机视角）

> 本节与 **步骤 10** 内容对应，区别只是执行位置：
> 步骤 10 是从 WSL **远程**通过 `ssh board` 执行，本节是**已登录板子后**就地执行。
> 两条路选其一即可，效果相同。

> ⚠️ **做错会让板子失联，只能上串口。**

```bash
# 1. 装包（两个都要，headers 是 DKMS 重建的前提）
sudo dpkg -i linux-image-<newver>-a733.deb linux-headers-<newver>-a733.deb

# 2. 确认默认启动项仍是旧内核（extlinux 别手改，用 u-boot-update）
grep -E 'default|menu label' /boot/extlinux/extlinux.conf

# 3. ★ 重建 WiFi/BT/GPU 的 DKMS 模块 —— 不做这步别重启
sudo dkms autoinstall -k <newver>
ls /lib/modules/<newver>/updates/dkms/
# 期望五个：aic_btusb_usb / aic_load_fw_usb / aic8800_fdrv_usb
#           radxa-overlays / pvrsrvkm

# 4. 再重启，从菜单选新内核
sudo reboot
```

**为什么第 3 步是致命的**：`aic8800_fdrv` 是 WiFi 驱动，而板子经
**`127.0.0.1:22222` 反向隧道**访问。新内核开机若 DKMS 未重建 ⇒ 无 WiFi
⇒ 板子失联 ⇒ 只能接串口抢救。

**另一个坑**：板子有硬件看门狗（`sunxi_wdt`，超时 16 秒）。
新内核起来但 `weaknet-server` 未启动 ⇒ 判据失败 ⇒ **16 秒整机复位** ⇒ 复位循环。
做任何停/重启服务的操作前先建维护标记：

```bash
sudo touch /var/lib/weaknet/health-guard-maintenance
# ... 操作 ...
sudo rm /var/lib/weaknet/health-guard-maintenance
```

### 4.4 阶段 3：ARISC 日志（决策门）

打开 `arisc_config` 的 `s_uart_config`（PL2/PL3），抓 ARISC 侧日志。

**这一步决定路线 B 还是 A**：

- 若 ARISC 日志显示它跑的是全志 rpmsg/rpbuf 协议 ⇒ 路线 B 有希望直接挂客户端；
- 若是闭源自定义 ⇒ 只能在 header 层做自描述，且很可能只能单向。

### 4.5 阶段 4：实现对端协议

按阶段 3 结果决定。见第五节业务安排。

---

## 五、业务安排：重划分现有负载，而不是给 MCU 加新功能

> 本节的口径是**把 A 核现有业务切一部分出去**，不是给 E902 增加新业务。
> 判据是「这件事在 A 核上是不是结构性做不好」，而不是「E902 能不能做」。

### 5.1 先按物理形态分类，再谈能不能搬

现有业务不是一堆同质的线程，而是三类完全不同的形态：

| 形态 | 执行位置 | 现状 | 能否搬到 E902 |
| --- | --- | --- | --- |
| **A. eBPF 采集** | **内核态** | 9 个 BPF 程序（`server/src/*.bpf.c`，共 3227 行），挂在 `kprobe`/`tracepoint` 上抓协议栈事件 | **搬不了**。E902 没有 Linux 协议栈，也没有这些 hook 点 |
| **B. 硬件已卸载** | cedar VE / VIN / ISP | 视频 H.264 走 `CedarVideoEncoder`（`video_encoder.cpp:537`），devicetree 打不开时回退桩实现；采集走 V4L2 | **没必要搬**。已经不在 CPU 上 |
| **C. 用户态逻辑** | A 核 CPU | D-Bus 2482 行、server 2393 行、BT 2090 行、SQLite 2073 行、遥测 1446 行 | **只有这类理论上可搬** |

**关键事实：占开销大头的 A 和 B 本来就不在 A 核 CPU 上。** 真正吃 CPU 的是 C，
而 C 几乎全部长在 Linux 专有基础设施上：

| 模块 | 依赖 | 迁移可行性 |
| --- | --- | --- |
| `dbus_service.cpp`（452 处 D-Bus 引用） | libdbus / 系统总线 | ❌ E902 上没有 D-Bus |
| `database_manager.cpp` | SQLite3 + WAL | ❌ 需要文件系统与页缓存 |
| `bt_monitor.cpp`（209 处） | BlueZ / MGMT socket | ❌ 依赖内核蓝牙栈 |
| `edge_telemetry_exporter.cpp`（86 处） | libcurl / HTTPS | ❌ 依赖 TLS 与 TCP 栈 |
| `v4l2_frame_source.cpp` | V4L2 ioctl | ❌ 依赖 V4L2 子系统 |
| `thermal_monitor.cpp` | sysfs（`/sys/class/thermal`、hwmon） | ❌ 需要访问 Linux sysfs |
| `host_pressure_monitor.cpp` | mmap `/dev/weaknet_cpu_stat`、`/dev/weaknet_softirq` | ❌ 依赖自建内核模块 |

⇒ **现有用户态业务没有一项能在搬走后保持功能等价**——不是"不划算"，是被底座锁死。

### 5.2 可切的只有「不依赖 Linux 底座、且 A 核结构性做不好」的部分

按这个筛子过一遍，只剩三件事。它们的共同点是：**都是对现有 L1 观测 /
健康判据职责的切分，不是新业务**。

| 优先级 | 切什么出去 | 为什么 A 核结构性做不好 | 依赖的通信能力 |
| --- | --- | --- | --- |
| **P1** | **喂狗权（见 5.3）** | 守卫进程自身挂掉时无法自证；`WDIOC_GETBOOTSTATUS` 恒 0，复位原因事后不可判 | 单向心跳 |
| **P2** | **采集旁路兜底** | A 核高负载时 eBPF 会掉证据，`observer_attribution.cpp` 已把退化链归因到 `cpu_busy` / `softirq_net_rx_share` | 双向小消息 |
| **P3** | **单调时戳基准** | `journalctl -b N` 在时钟跳变后 boot 归属不可信 | 双向小消息 |

### 5.3 看门狗协同：可以搬，但搬的是「所有权」不是「判据」

这是 P1 的展开，也是最值得先做的一项。

#### 为什么它特别适合搬

喂狗是一条**硬件通道**：`sunxi_wdt` 平台驱动 + `/dev/watchdog0`。
`tools/weaknet-health-guard.cpp:22` 明确写着「本程序不碰任何寄存器，
只通过字符设备的标准 write/ioctl 语义驱动它」。谁 `open()` 谁持有喂狗权
（`drivers/watchdog/watchdog_dev.c` 的 `watchdog_start()` 会置 `WDOG_ACTIVE`
并让内核彻底停止代喂）。这与 D-Bus / SQLite 那类软件依赖不同——
硬件同源，换一个核持有不需要重建任何底座。

#### 四条判据一条都搬不走

| 判据 | 实现 | 数据源 | E902 能否获取 |
| --- | --- | --- | --- |
| 落库推进 | `CheckWalMtime()` :587 | `stat()` `history.db-wal` 的 mtime | ❌ 无文件系统 |
| 采集推进 | `CheckSampleNs()` :615 | `busctl` 调 D-Bus `GetEbpfMonitorHealth` | ❌ 无 D-Bus |
| 进程活性 | `CheckCpuDelta()` :821 | `/proc/<pid>/stat` 的 `utime+stime` | ❌ 无 procfs |
| D 状态 | `CheckDState()` :926 | `/proc/<pid>/task/<tid>/stat` 第 3 字段 | ❌ 无 procfs |

⇒ **判据必须留在 A 核。** 搬到 E902 等于让它猜 A 核的健康状况，退化成凭感觉判断。

#### 方案一：E902 持有喂狗权，A 核申请喂狗（高价值 / 高前提）

```
A 核 health-guard 四条判据全通过
    ↓ msgbox 发 4 字节「健康」心跳
E902 收到 → E902 执行喂狗
E902 超 N 秒未收到心跳 → 停止喂狗 → 硬件超时复位
```

- **收益**：判据一行不改（它们本来就在 A 核）；
  守卫进程自身挂掉 ⇒ 心跳停 ⇒ 仍会复位，补上现有实现里
  「自己崩溃只能当成期望行为、无法区分」的盲区；
  跨复位边界的故障记录成为可能（E902 在复位前把最后心跳时刻写进共享内存保留区）。
- **前提**：`sunxi_wdt` 寄存器基址必须落在 E902 的可见地址范围内——**未验证**。
- **新问题**：E902 自己崩了就没人喂狗 ⇒ 需要双向守护，复杂度上升。

#### 方案二：E902 只当外部见证者（低风险 / 先做这个）

E902 不喂狗，只记录「A 核还活着吗」的时间戳到共享内存。复位后 A 核读出来，
就能区分「守卫主动停喂」与「守卫自己死了」。

- **收益**：零风险，不碰任何现有关键路径；只需要单向心跳，
  对 ARISC 固件的要求最低（收到 4 字节就记时间戳，甚至不需要认识 rpmsg 协议）。
- **代价**：不防护 A 核卡死，只是个记录仪。

#### 结论：先方案二，再评估方案一

方案二能回答一个当前无法回答的问题：**过去那些复位，到底有几次是守卫自己挂的？**
在拿到这个答案之前上方案一，等于没做体检就做手术。

### 5.4 明确不做

| 不做 | 原因 |
| --- | --- |
| sensor 上电时序搬到 E902 | 已证伪：零帧根因是 CSI 排线接触不良；Linux 侧 `sensor_power()` 本就完整，搬走只会造出观测不到的黑盒 |
| E902 直接产生诊断/预测结论 | 违反五层契约：诊断只由 L2 产生 |
| 用 E902 做设备控制 | 违反产品定位的「不做设备管理」 |
| 视频编码卸载 | E902 200 MHz，无 VE，且 cedar VE 已经承担 |
| E902 直接写 `device_events` | 双源冲突，违反 single-source capture 原则 |

### 5.5 数据回流纪律（若确有数据回 A 核）

- E902 产出必须带 `source=rproc` 标记，**禁止**与 eBPF 结果混在同一字段；
- 入库前必须经 A 核校验：时间基准归一、范围检查、能对上才写；
- **禁止**替代任何现有 eBPF 探针。

### 5.6 验收口径

不能只验「能发能收」。必须回答：

1. 切出去的那部分，**有没有让某个已有指标变得更可信**？
2. 拆掉 E902 后，是否出现**可观测的退化**？（若无退化，说明它是装饰）
3. 故障注入：让 A 核故意卡死，E902 那侧是否真的独立工作？

---

## 六、边界与风险

### 不需要再折腾的

- ❌ 不要重刷 `rsdk-r6` 镜像：官方 `-21` 内核**没有 BTF**，一刷 eBPF 全废。
- ❌ 不要"从 Tina 抽 E902 rproc"：BSP 里没有，Tina 里要拿到 SDK 全套。
- ❌ 不要把 `/sys/kernel/debug/mpp/mipi` 当探针（`openspec/pitfalls/mpp-mipi-debugfs-panics.md`）。

### 仍在的风险

| 风险 | 说明 | 缓解 |
| --- | --- | --- |
| ARISC 占用 msgbox 通道 1/2/3 | hwdata 只声明了 coef 0 | 阶段 0 只读探测 |
| E902 地址视图未知 | 决定共享内存能否被访问 | 查 SoC 手册 |
| 无 reserved-memory 可用区 | 板端只有 bl31 | 自行划区，避开 BL31 与 CMA |
| 接管后 PM 失效 | 路线 A 的必然代价 | 不走路线 A |

### 硬件安全

任何 DT 改动前必须：

```bash
ssh board 'sudo touch /var/lib/weaknet/health-guard-maintenance'
# ... 操作 ...
ssh board 'sudo rm /var/lib/weaknet/health-guard-maintenance'
```

（`./tools/ci.sh` 已内置，手工操作需自己做。）

---

## 七、待决问题

1. **E902 到底跑什么固件？** —— 需阶段 3 的串口日志。
2. **E902 的地址视图是什么？** —— 需 sun60iw2 手册。
3. **msgbox 通道 1/2/3 谁在用？** —— 需阶段 0/1 实测。
4. **`a55_rproc@0` 是给谁预留的？** —— compatible `allwinner,arm64-rproc`
   两版 BSP 都无驱动，可能是给未来的 A55-AMP 或自研固件。
5. **是否值得为 P1（健康判据）单独立项？** —— 这是唯一不依赖 E902 固件的业务。

---

## 八、优先级 / 状态

### 已完成（2026-11-18）

- [x] **阶段 0：msgbox 归属探测** —— DT 节点 `soc@3000000/msgbox@3004000`
      由 overlay 打开，`status=okay`，**无任何 `mboxes` 消费者**
- [x] **阶段 1：`sunxi-msgbox` probe 验证** —— 四项硬证据齐备
      （ioremap 两段 / IRQ 432·433 / 时钟 26 MHz enabled / 驱动已绑定）
- [x] **阶段 2：内核增量编译** —— `5.15.147-100btf1amp-a733` 已运行，
      **BTF 零损失（51 个 BPF 程序在跑）**，配置差异仅 19 行，
      旧内核保留为 `extlinux.conf` 的 `l1`
- [x] **阶段 2.5：通路实测（新增，原计划无此步）** —— 用内核模块写 FIFO，
      确认 **A→E902 写入通路在硬件层可用**，但**两侧 `RD_IRQ_EN=0`、
      A 核本地队列结构性全 0**，即 **E902 不在监听、且从未发过消息**（详见 2.10）
- [x] **阶段 2.6：p=1/2/3 通道覆盖（2026-11-18 补测）** —— **p=3 未实现**
      （六次写入计数纹丝不动），p=1 对照正常递增。此前 `RD_IRQ_ST=0x40`
      被当作「p=3 有历史痕迹」系误读，实为复位默认值

### 未完成

- [ ] **阶段 3：ARISC 串口日志（决策门）** —— **已确认走不通**：
      `arisc_config/s_uart_config` 实测为空值，串口未配置，**无日志可抓**。
      要开必须改 DT 写 pinmux/baud，风险高于此前所有操作。
- [ ] **阶段 4：实现对端协议** —— 前置条件缺失（见下）

### 当前的真实卡点

**阶段 3 的决策门已失效**，原设计「抓 ARISC 日志判断它跑什么协议」这条路断了。
现在的信息缺口变成：

| 缺口 | 影响 | 可能的解法 |
| --- | --- | --- |
| E902 是否真的在执行指令 | 决定这事还有没有讨论前提 | **无法验证**：需其 CFG/version 寄存器基址，sun60iw2 未知；盲扫 R 域会把板子扫挂（已发生一次） |
| E902 跑什么固件 | 决定 coexisting 是否可能 | 无（除非能烧固件或找到原厂文档） |
| E902 的 DDR 地址视图 | 决定共享内存能否被访问 | sun60iw2 手册 |
| ~~p=1/2/3 通道状态~~ | ~~提示 p=3 有历史~~ | ✅ **已解决**：p=3 未实现，p=1 正常；`0x40` 是复位默认值 |

### 三条路线的现状重估

| 路线 | 原评估 | **实测后重估** |
| --- | --- | --- |
| A. 完全接管 E902 | 不建议 | 不建议（结论未变） |
| B. coexisting | 首选 | ❌ **当前不可行**：对端既不监听、也从未发过消息；且 BSP 缺 rproc/rpmsg/保留内存三件套 |
| C. 只验 msgbox 驱动 | 第一步 | ✅ **已完成，成功** |

⇒ **路线 B 当前不可行**。「不碰 E902 固件」与「需要 E902 侧实现协议」
互相矛盾，而且比原先估计更糟：**不只是固件不配合，连承载 rpmsg 的内核
构件（`CONFIG_AW_REMOTEPROC` / `CONFIG_RPMSG_VIRTIO` / E902 boot 驱动 /
riscv SRAM 保留）在 sun60iw2 的 BSP 里被整段裁掉了**。

### 结论

**当前建议是不启动。** 理由已经从「等业务需求」变成一条硬约束：

> **这块板子的 AMP 通路，在硬件上只有一个「能写进去的邮箱」，
> 没有任何活体在另一端收信。** 继续投入必须同时解决两件事：
> ① 拿到 E902 的固件控制权或接口文档；② 自己补上 remoteproc + rpmsg
> 的整套内核构件（BSP 里没有 E902 适配）。

这两件事**都超出本仓库能力范围**。

### 若将来要重启，第一步是什么

不是写协议，也不是划共享内存，而是按顺序回答三个问题：

1. **E902 到底在不在跑？** —— 需要 SoC 手册给出它的 CFG/version 寄存器地址；
   在此之前连「有没有对端」都无法确认。
2. **能不能烧固件？** —— 决定走路线 A（接管）还是路线 B（共存）。
3. **它能看到哪段 DDR？** —— 决定共享内存怎么划。

在这三个问题有答案之前，任何协议层设计都是空中楼阁。

### 结论

**当前建议仍然是不启动。** 但理由已经变了：

- 原理由：「需等出现真正需要实时隔离的业务」；
- **新理由**：「硬件通路已验证可用，但**对端没有配合的固件**，
  继续投入必须先解决 E902 固件问题——这超出本仓库能力范围」。

**这一阶段的净收益**：把一个模糊的「也许能行」变成了明确的
「通路没问题，缺的是对端」。这本身是有价值的结论——
它避免了未来在错误的方向上投入。

### 若将来要重启，第一步是什么

不是写协议，也不是划共享内存，而是：**想办法拿到 E902 的固件控制权或接口文档**。
在此之前，任何协议层设计都是空中楼阁。

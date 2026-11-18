# A733 跨核 rpmsg（AMP）可行性与路线勘察

> 日期：2026-11-17
> 分类：探索（只读勘察 + 文档产出）
> 结果：部分完成（事实已核清，路线未落地；阶段 0 探测待板子在线）

## 构思

**要解决的问题**：能否在 A733 的 A 核（Linux）与片上 E902 RISC-V 之间建立
rpmsg 跨核通道，以及为此需要哪些前置改动。

**原始诉求**来自"方案2"：保留 Radxa Debian 主系统不换 Tina，在现有内核上
加 `remoteproc + msgbox + rpmsg`，把 IMX219 的 sensor 上电/复位时序搬到
协处理器，以此解决零帧问题。

**为什么不做进 OpenSpec change**：

1. 全部工作是**只读勘察**（读板端 `/boot` `/proc/device-tree`、读 GitHub 上的
   BSP 源码），没有产品代码改动，没有可验证的行为变更，套用 change 流程
   会产生零 footprint；
2. 结论是"当前不建议启动"，还不具备规划成 change 的前提；
3. 按仓库约定，未动工的设想进 `docs/ideas/`，勘察过程进本目录。

**备选方案与取舍**：

| 方案 | 取舍 |
| --- | --- |
| 从 Tina SDK 抽 E902 rproc 驱动 + 自烧 OpenAMP 固件 | 需要整套 Tina SDK；废掉原厂电源管理栈，整机稳定性风险高 |
| 复用现有 msgbox，与 ARISC 共存（不换固件） | 不碰 PM，但要先确认 msgbox 通道归属与 E902 地址视图 |
| 只验证 `sunxi-msgbox` 驱动能否 probe | 成本最低、信息量最大，作为第一步 |

## 实现方式

**没有修改任何仓库代码**。产出是一份文档：

`docs/ideas/A733跨核rpmsg通信-可行性与路线.md`

**验证手段（全部只读）**：

- 板端：`/boot/config-*`、`/boot/System.map-*`、`/boot/extlinux/extlinux.conf`、
  `/proc/device-tree/**`、反向解压 `cubie-a7a-radxa-camera-8m-219.dtbo`、
  `ls /lib/modules/<ver>/updates/dkms/`、`/usr/src/linux-a733-5.15`；
- GitHub：`radxa-pkg/linux-a733`（`.gitmodules`、`.github/local/Makefile.local`、
  `debian/patches/linux/0001-*.patch`）、`radxa/allwinner-bsp@cubie-aiot-v1.4.6`
  与 `@v1.5.0`（`drivers/msgbox`、`drivers/remoteproc`、`drivers/rpmsg`、
  `drivers/rpbuf`）、`radxa/allwinner-device@device-a733-v1.4.6`
  （`configs/cubie_a7a/linux-5.15/board.dts`）、`radxa/kernel@allwinner-aiot-linux-5.15`
  （`configs/linux-5.15/sun60iw2p1.dtsi`）。

**关键决策**：

1. **不在本次会话编译内核**。BTF 是现有 eBPF 能力的唯一来源，构建机缺 `pahole`
   时 BTF 会静默降级，必须先预检。
2. **明确记录"BTF 的静默失效点"**：上游 patch 设 `CONFIG_DEBUG_INFO_NONE=y`，
   而 5.15 的 `DEBUG_INFO_BTF depends on DEBUG_INFO depends on !DEBUG_INFO_NONE`，
   直接加 `CONFIG_DEBUG_INFO_BTF=y` 会被 `olddefconfig` 静默吞掉、不报错。
   这是本次勘察最有价值的可复用结论。
3. **把网上流传的 DT 示例判为不可用**：`rproc_fw@48000000` 与
   `reserved-memory/bl31`（`0x48000000`，16 MiB）完全重叠；
   `0x02900000` / `SPI 132` / `#address-cells=<1>` 三项均与 BSP 原文不符。

## 结果

**已定性的事实**（证据见 `docs/ideas/A733跨核rpmsg通信-可行性与路线.md` 第二节）：

- 当前内核 `5.15.147-99btf1-a733` 的 `MAILBOX` / `REMOTEPROC` / `RPMSG` /
  `AW_MSGBOX` / `AW_REMOTEPROC` / `AW_RPBUF_*` **全部未开**；
  相对官方 `-21` 内核，差异只有 9 行（BTF 那几行 + `PAHOLE_VERSION`）。
- BSP 里有完整的 `msgbox + remoteproc + rpmsg + rpbuf` 驱动栈，**但没有 E902
  的 rproc 驱动**（v1.4.6 只有 hifi4/e906/e907；v1.5.0 增 c906/arm-rtos/
  arm-baremetal，仍无 e902，也无 `arm64-rproc`）。
- 板级 `board.dts:710` 与上游 `sun60iw2p1.dtsi` **两层都把 `&msgbox` 设为 disabled**。
- `sun55iw5_hwdata.processors_max = 2`（sun60iw2 复用）⇒ 这颗 SoC 的 msgbox
  只能连两个处理器，一侧是 A 核，另一侧只能是 E902/ARISC；
  **不存在 A 核 ↔ A55 的 msgbox 通路**，DT 里 `a55_rproc@0` 无可用硬件。
- msgbox 一条消息 4 字节、FIFO 深度 8 ⇒ 只能做 doorbell，payload 必须走共享内存。

**遗留问题**：

1. E902 实际运行的固件未知——需打开 `arisc_config/s_uart_config`（PL2/PL3）抓日志。
2. E902 的 DDR 地址视图未知——需 sun60iw2 手册，决定共享内存能否被访问。
3. msgbox 通道 1/2/3 是否被 ARISC 占用——需板子在线做阶段 0 只读探测。
4. `a55_rproc@0`（compatible `allwinner,arm64-rproc`）两版 BSP 都无驱动认领，
   用途不明。

**本次未能完成的部分**：阶段 0 的板端探测（`/proc/interrupts` 的 SPI 0/211
计数、`/proc/iomem`、`/proc/device-tree/arisc_config/`）因反向隧道断开未能执行。

**是否值得升格为正式 OpenSpec change**：**暂不**。当前建议是不启动；
等出现真正需要实时隔离且 Linux 做不到的业务，或"E902 独立健康判据"
被证明是当前系统的实际短板时，再考虑立项。

**同时修正的一条旧判断**：本次勘察的原始动机（把 sensor 时序搬到 E902）
已于 2026-11-16 被证伪——零帧根因是 CSI 排线接触不良，Linux 侧
`modules/sensor/imx219.c::sensor_power()` 本就实现了完整时序。

---

## 追加一：内核增量编译移交（同日，未执行）

勘察结论出来后，用户决定**先不动板子**，改在**自己的 Windows 电脑**上做
内核增量编译（开发服务器只有 3.6 GB 内存 / 12 GB 可用磁盘，而带 BTF 的
ARM64 内核构建需要 20–25 GB 磁盘 + 8 GB 以上内存，客观不可能完成）。

**因此本次会话的产出从"勘察报告"扩展为"可移交的施工说明"**，
写入同一份 `docs/ideas/A733跨核rpmsg通信-可行性与路线.md`：

- 新增「零、执行入口」——给执行方 Agent 的定位表 + 开工前必须补的三项信息；
- 新增 `4.3.6` 构建环境（Windows WSL2 真实实测数据、既有资产清单、
  `.wslconfig` 必建、两条硬约束）；
- 新增 `4.3.7` 增量构建流程（步骤 0–10，可逐条复制执行）；
- 新增 `4.3.7.1` 开发板 SSH 访问方式（板子无公网地址，只经开发服务器
  `127.0.0.1:22222` 反向隧道可达；三种链路 + 两个坑 + 失联抢救）；
- 新增 `4.3.8` 上板命令（板端本机视角，与步骤 10 二选一）。

**为什么没有直接编译**：

1. 本机（开发服务器）磁盘与内存都不够，强行构建会把工作区拖到不可用；
2. Windows 侧上一次编译 `5.15.147-99btf1` 的环境与产物**完好可复用**
   （`~/linux-a733`，`vmlinux` 300 MB 含 BTF），本次是增量而非重建；
3. 但**配置改动落在哪一层尚未确定**——`~/linux-a733-btf-config.patch`
   改的是 `src/.config`、`radxa.config` 还是新增 fragment，决定后续所有改法。
   走错层会被 `bindeb-pkg` 的 defconfig 覆盖，BTF 静默丢失且不报错。
   必须先读到该 patch 原文才能继续。

**当前阻塞点**（需 Windows 侧提供）：

- `~/linux-a733-btf-config.patch` 全文；
- `~/linux-a733/src/.config` 里 `CONFIG_DEBUG_INFO*` 与 `CONFIG_LOCALVERSION*`；
- WSL 内 `df -h ~`（确认 ≥ 20 GB 可用）。

另：开发板反向隧道在本次会话末尾已断开（`127.0.0.1:22222` Connection refused），
阶段 0 的板端探测仍未执行。

**选定的构建环境：WSL2 原生，排除 Docker Desktop。**

三条理由（已写入文档 4.3.6）：

1. **既有产物就在 WSL 的 ext4 里**——源码树、已编译 `.o`、300 MB 的 `vmlinux`、
   `Image`、deb、构建日志全在 `~/linux-a733`。换容器要么跨层挂载、要么重编几小时，
   没有收益；
2. **`.wslconfig` 对 Docker Desktop 不生效**——它的内存上限在自身
   Settings → Resources，是另一套。混用会得到"以为给了 12 GB、实际只给 4 GB"的
   假象，在 `-j8` 链接 vmlinux 时被 OOM killer 杀；
3. **uid/权限翻译**——WSL 里 `tqf` 是 uid 1000，绑进容器常变 root 或 nobody，
   `make` 产出属主错乱。

**顺带澄清一个容易混淆的历史事实**：日志里"上次在 devcontainer 里编的"
（`/workspaces/linux-a733` + devenv/Nix）**不等于 Docker Desktop**。
VS Code 的 devcontainer 跑在 WSL 里，`/workspaces` 通常就是 WSL 文件的 bind mount。
但它引出一个尚未验证的点：`devenv`/Nix 装在 WSL 主机上还是只在镜像里？
若只在镜像里，WSL 主机直接 `make` 会找不到 `pahole`。

因此把**工具链探测**提升为动手前的第 ④ 项必查（`which devenv nix pahole
aarch64-linux-gnu-gcc`），并写进文档步骤 0：

| 探测结果 | 走法 |
| --- | --- |
| 全都有 | 直接在 WSL 里编（主路径） |
| 缺 `pahole` / `nix` | 回 devcontainer 编（仍在 WSL 文件系统上，可接受） |
| 只缺 `pahole` | `sudo apt install -y dwarves` |

**没验工具链就往下走，会在步骤 6 链接阶段才炸，白等 20 分钟。**

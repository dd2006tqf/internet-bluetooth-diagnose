# A733 msgbox 通路实测：A 核侧全静默，对端无活体

> 日期：2026-11-18（2026-11-18 追加修正，见文末「修正记录」）
> 分类：探索（真机实测）
> 结果：已完成（结论明确，但方向因此关闭）

> **一句话结论**：这块板子的 AMP 通路，在硬件上只有一个「能写进去的邮箱」，
> **没有任何活体在另一端收信**，且承载 rpmsg 的内核构件在 BSP 里被整段裁掉。

> ⚠️ **本文早期版本的标题与结论「E902 存活」已被推翻，不可引用。**
> 它建立在一次硬件下溢假象之上，详见文末「修正记录」与
> `openspec/pitfalls/msgbox-fifo-read-underflow.md`。

## 构思

**要解决什么问题**：`docs/ideas/A733跨核rpmsg通信-可行性与路线.md` 的方案卡在
「E902 到底在不在、msgbox 能不能通」这个岔路口。文档里 E902 的固件与行为
全部是**推断**，没有任何实测。

**为什么不做进 OpenSpec change**：

1. 全程只读 + 一次性探针，**没有产品代码改动**，套 change 流程会产生零 footprint；
2. 结论是「方向关闭」，不是「行为变更」，没有可验收的产品表面；
3. 按仓库约定，孵化期探索走 `docs/ideas/` + 本目录。

**备选方案与取舍**：

| 方案 | 取舍 |
| --- | --- |
| 用户态 `/dev/mem` 直接读寄存器 | ❌ **实测不可行**：设备区已被 ioremap，`read` 报 `Bad address` |
| 改板级 DTS 开 msgbox | ❌ 不必：已有 `/boot/dtbo/msgbox-enable.dtbo` overlay |
| 写内核模块，只轮询状态寄存器 | ✅ **采用**：不使能中断，无 IRQ 风暴/空指针风险 |
| 写内核模块，正规走 `mbox_request_channel` | ❌ 需构造 consumer 节点，侵入大；且 `startup()` 会改寄存器状态 |
| 打开 ARISC 串口抓日志（原阶段 3） | ❌ `s_uart_config` 为空值，需改 DT 写 pinmux，风险高 |

## 实现方式

### 内核侧（Windows WSL2 完成，已在上一任务记录）

`5.15.147-100btf1amp-a733`：只加 `CONFIG_MAILBOX` + `CONFIG_AW_MSGBOX`，
BTF 零损失。另配 `/boot/dtbo/msgbox-enable.dtbo`（282 字节）翻转
`soc@3000000/msgbox@3004000` 的 `status` 为 `okay`。

### 探针模块（本次新增）

源码存档于 `openspec/tasks/artifacts/2026-11-18-a733-msgbox-probe/`：

- `msgbox_probe.c`（4.8 KB）
- `Makefile`

**设计要点**：

1. **只轮询，不开中断**。驱动只在 `sunxi_msgbox_startup()` 里置 `RD_IRQ_EN`，
   而它由 `mbox_request_channel()` 触发——当前**无任何 DT 消费者**，`startup()`
   从未执行。若探针自行置位中断使能，驱动的 IRQ handler 会遍历
   `controller.chans[]` 调 `sunxi_msgbox_read_handler()`，那里 `chan->cl` 为 NULL
   ⇒ **内核 oops**。因此探针绕开中断路径，直接写 FIFO + 读状态寄存器。
2. `mode=0` 纯只读 dump（不写任何寄存器）。
3. `mode=1` 额外写 4 字节 magic `0xA733C0DE` 到远端 FIFO，每 50 ms 轮询 60 次。
4. 板端原生编译（headers 完整、aarch64-linux-gnu gcc 可用、103 GB 空间）。

### 验证手段（真机命令）

```bash
# 编译
make -C /lib/modules/$(uname -r)/build M=$PWD modules

# 只读快照
sudo insmod msgbox_probe.ko mode=0
# 写入实验
sudo insmod msgbox_probe.ko mode=1 send_chan=0
```

## 结果

### 驱动 probe 成功（四项硬证据）

| 证据 | 实测值 |
| --- | --- |
| ioremap | `/proc/iomem`：`03004000-03004fff` + `07094000-07094fff` |
| 中断 | IRQ **432 / 433**（devname `3004000.msgbox`） |
| 时钟 | `msgbox0` 26 MHz，enabled |
| 绑定 | `/sys/bus/platform/drivers/sunxi-msgbox/3004000.msgbox` |

### 写入实验（决定性）

```
before send, remote FIFO queued = 0        ← 写入前是空的，排除误判
wrote 0xA733C0DE to remote FIFO p=0
remote FIFO changed at t=2250ms (queued=1) ← 消息确实进了远端 FIFO
t=2300..3000ms 持续 queued=1               ← 3 秒内无人取走
RESULT got=1  remote_queued=1  local_queued=0
RD_IRQ_ST[0] = 0x40 → 0x41                 ← 硬件为新消息置起 pending
```

### 结论

| 问题 | 答案 |
| --- | --- |
| A→E902 写入通路（硬件层） | ✅ **可用**（写入后计数递增、`RD_IRQ_ST` 置 pending 位） |
| E902 是否在监听 msgbox | ❌ **没有**（两侧 `RD_IRQ_EN=0`，消息 10s 无人取） |
| E902 是否向 A 核发过消息 | ❌ **从未**（A 核本地 8 个 chan/proc 组合 `MSG_ST` 全 0，静置 10s 不自增） |
| E902 是否在运行 | ⚠️ **无法证明，也无任何正面证据** |
| E902 跑什么固件 | ❌ **完全未知**（BSP 无 E902 驱动、DT 无节点、无保留内存） |

⇒ **原推断「E902 跑 ARISC PM 固件并会用 msgbox」被彻底推翻。**

**一个必须分清的因果**：写进 FIFO 成功 **只证明 msgbox 控制器正常工作**，
不证明对端有活体。`RD_IRQ_ST` 的 pending 位是**控制器**置的，与 E902 是否取数无关。
早期版本把这两件事当成一件事，直接得出「E902 存活」——这是本次最大的错误。

### 对方案的影响

**路线 B（coexisting，不碰 E902 固件）当前不可行**——「不碰 E902 固件」与
「需要 E902 侧实现协议」互相矛盾。要往下走只剩路线 A（接管 E902）或
找到原厂固件接口，两者都超出本仓库能力范围。

**原阶段 3（ARISC 串口日志决策门）已失效**：`s_uart_config` 为空值，
串口未配置，无日志可抓。

### 遗留问题

1. **E902 是否真的在执行指令** —— **无法验证**。需要它的 CFG/version 寄存器
   基址，而 sun60iw2 的该基址未知；**盲扫 R 域地址已把板子扫挂一次**，不再尝试。
2. **E902 到底跑什么固件** —— 仍未知，这是继续投入的唯一前置。
3. **E902 的 DDR 地址视图** —— 需 sun60iw2 手册。
4. ~~p=1/2/3 通道未测~~ —— ✅ **已测**：**p=3 通道未实现**（六次写
   `0x0709407C`，`MSG_ST` 纹丝不动；p=1 对照正常从 4 递增到 9）。
   此前 `RD_IRQ_ST=0x40` 被当作「p=3 有历史痕迹」系误读，它是**每次冷启动
   必现的复位默认值**。
5. E902→A 回传方向——**不是「未验证」，是「已确认从未发生」**：
   A 核本地 8 个 chan/proc 组合 `MSG_ST` 结构性全 0，静置 10 秒不自增。

### 是否值得升格为正式 OpenSpec change

**不值得。** 结论是「硬件通路在 A 核侧没问题，但对端无活体、且 BSP 缺整套
remoteproc/rpmsg 构件」，这不是本仓库能解决的问题。

### 架构层补充：rpmsg 在这块板子上结构性不可达

| 检查项 | 实测 |
| --- | --- |
| `CONFIG_AW_REMOTEPROC` / `CONFIG_REMOTEPROC` | **not set** |
| `CONFIG_RPMSG_VIRTIO` | **not set** |
| 全部 `CONFIG_AW_RPBUF_*` | **not set** |
| BSP 是否有 E902 boot 驱动 | ❌ 没有。只有 hifi4 / **e906** / e907；`sunxi_rproc_match[]` 里**全库搜不到 e902** |
| sun60iw2 dtsi 有无 E902 节点 | ❌ 没有。只有 `a55_rproc`（`allwinner,arm64-rproc`），且 `status="disabled"` |
| sun60iw2 有无 riscv SRAM 保留 | ❌ 没有。`reserved-memory` 里**只有 `bl31`** |
| 对比 sun60iw1p1（有协处理器的平台） | `e906_rproc@7130000` + `memory-region=<&riscvsram0_reserved>`；两块 0x80000 riscv SRAM；msgbox 四个寄存器 bank |

⇒ **不是「配置没开」，是驱动 / DT 节点 / 保留内存三样全缺。**

## 修正记录（2026-11-18 追加）

本次追加推翻了本文原始版本的标题与核心结论。原因是一处硬件假象：

**`MSG_ST[3:0]` 是 4 位环形计数器，读空 FIFO 会下溢回绕（0 − 1 = 15）。**

任何「先读 `MSG_ST`、再读 `MSG_FIFO`」的轮询都会自己制造出「有消息」的假象，
且**永远不报错**。铁证是一次三段记录：

```
UNDERFLOW loc p=0 before=0 read=0x00000000 after=15
UNDERFLOW loc p=1 before=0 read=0x00000000 after=15
UNDERFLOW loc p=2 before=0 read=0x00000000 after=15
```

原始版本据此判定的「E902 存活（能接收 FIFO 写入、硬件置 pending）」与
「p=3 有历史痕迹」**全部作废**。改正方式：重写严格只读探针（`msgbox_probe3.c`，
**不写任何寄存器、不读任何数据 FIFO**），在干净启动后连做两次快照（间隔 10s），
得到两侧 `RD_IRQ_EN=0`、A 核本地队列全 0、10 秒逐字段不变的结果。

完整踩坑记录见 `openspec/pitfalls/msgbox-fifo-read-underflow.md`。

## 产物清理

板端 `~/msgbox_probe/` 与本机 `/tmp/msgbox_probe/`、`/tmp/bspmsgbox/` 均为
一次性的运行时材料，**任务完成即删**。源码已存档到本目录 `artifacts/` 下。

**2026-11-18 已执行清理**：板端 `rmmod` 全部探针模块并 `rm -rf ~/msgbox_probe`
（确认无残留、`lsmod` 无 msgbox_probe、msgbox/mailbox 驱动均未加载）；
本机 `rm -rf /tmp/msgbox_probe`。`/tmp/bsp_*` 为 BSP 源码拷贝，非运行时材料，
保留备查。

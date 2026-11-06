# LRADC 按键驱动（L1→L4 全链路）

> 日期：2026-11-06
> 分类：工作流外实施 / 驱动技术栈练习
> 结果：已完成（L1/L2/L3/L4 代码全部落地，L3 已在真机端到端验证；ADC 转换在本板无法启动，已定位到 BSP 层面）

## 构思

### 要解决什么

`内核模块技术栈引入讨论.md` 提出三个方向，路径 B1 的内核模块实验（`weaknet_cpu_stat` +
`weaknet_softirq`）已于 2026-11-05 完成。但那条路线只覆盖了「内核态数据采集」，
没有覆盖**设备驱动**这一整层技术栈：platform_driver 与 OF 匹配、MMIO、时钟、复位、
中断、input 子系统、debugfs。

目标是给这个项目补上驱动层的纵深，让仓库能覆盖 Linux 驱动开发岗会问的技术面。

### 为什么选 LRADC

- 板端 DT 里确实有一个 `lradc@2524000` 节点，`status = "okay"`，声明了 5 个按键，
  但内核里**没有任何驱动绑定它**（`CONFIG_AW_LRADC` 未开）。
- 它是一个完整的、有真实硬件语义的外设：6 bit ADC + 电压阈值中断 + 按键去抖。
- mainline 有 `sun4i-lradc-keys.c` 可作规格参照，但**不能直接复用**（见下）。

### 为什么不能复用 mainline 驱动

| # | 障碍 | 证据 |
| --- | --- | --- |
| 1 | compatible 不匹配 | mainline 只认 `allwinner,sun4i-a10-lradc-keys` 与 `allwinner,sun8i-a83t-r-lradc`；本板是 BSP 私有串 `allwinner,keyboard_1350mv` |
| 2 | DT 键表格式不同 | mainline 用 `channel { linux,code; ... }` 子节点；本板用 `key_cnt = <5>` + `key0..key4 = <voltage_mv keycode>` 成对属性，无子节点 |
| 3 | 无 `vref-supply` | mainline 靠 regulator 取参考电压；本板把电压编码在 compatible 后缀（`1350mv`）里 |
| 4 | 时钟/复位未管 | mainline 完全不碰 clk 与 reset；本板不显式 `clk_prepare_enable` + `reset_control_deassert` 就拿不到可用寄存器 |

因此选择**重写而非移植**：寄存器与位定义参照 mainline（GPL-2.0-or-later,
Copyright (C) 2014 Hans de Goede），其余为本项目重写。

### 为什么不做进 OpenSpec change

按 CLAUDE.md 的孤儿判定：一个没有真实消费者、没有可观测命令的产品表面不成立。
这个驱动对用户态的唯一出口是 `/dev/input/eventN`，而项目的 `MetricId` 枚举里
16 个指标全是网络类，`assurance/` 下 26 个头文件无一看物理按键。

即使驱动完全正常，产出的「用户按了 KEY_VOLUMEUP」在业务语义里也没有位置可放。
强行套 change 流程会因 footprint 判定与 fingerprint 反复变化而报废。故走工作流外记录。

## 实现方式

### 交付物

| 文件 | 行数 | 作用 |
| --- | --- | --- |
| `kmod/weaknet_lradc_keys.c` | 1362 | 完整驱动，四层结构 |
| `kmod/Makefile` | +1 行 | `obj-m += weaknet_lradc_keys.o` |
| `kmod/README.md` | +156 / -4 行 | 第八节，硬件事实 / 四层实现 / 实测边界 / 最终判定 |
| 本文件 | — | 三节记录 |

### 硬件事实（全部实测）

| 项 | 值 | 来源 |
| --- | --- | --- |
| 寄存器基址 | `0x02524000`，长度 `0x100` | DT `reg` 属性 |
| 中断 | IRQ 428（SPI 37，电平触发） | DT `interrupts = <0 0x25 4>` |
| 时钟 | `lradc`，26 MHz，phandle `0x12`/`0xc1` | DT `clocks` + `clk_summary` |
| 复位 | phandle `0x12`/`0x47` | DT `resets` |
| 上电初值 | `CTRL=0x01000168`（ENABLE=0），`DATA0=DATA1=0x3f` | 驱动 probe 打印 |
| 参考电压 | 1350 mV | compatible 后缀 `1350mv` |
| 按键表 | key0 210mV/115、key1 410/114、key2 590/139、key3 750/28、key4 880/172 | DT `key0..key4` |

寄存器布局：`CTRL 0x00` / `INTC 0x04` / `INTS 0x08` / `DATA0 0x0c` / `DATA1 0x10`；
`INTS` 写 1 清零（W1C）；采样值 `DATA0 & 0x3f`（6 bit，满量程 63）。

### 四层实现

**L1 骨架**：`platform_driver` + `of_match_table`（`allwinner,keyboard_1350mv`）+
`devm_platform_ioremap_resource` + `devm_clk_get`/`clk_prepare_enable` +
`devm_reset_control_get_optional_exclusive` + deassert。全部走 `devm_*`，错误路径自动回收。

**L2 中断**：`devm_request_irq` + `INTS` 判源 + `DATA0` 读值 + 电压换算 + W1C 清中断。
顺序上**先 request_irq 再打 INTC 使能位**，避免电平中断在无 handler 窗口期被记 spurious。
中断上下文用 `spinlock_t`（不能睡眠）。电压匹配用**绝对差最小**而非区间判定，
DT 标称电压有偏差时不会整键失效。

**L3 input 子系统**（本轮新增）：
- `devm_input_allocate_device`（挂 devres，解绑自动注销，不漏 error path）
- `set_bit(EV_KEY)` + `set_bit(EV_SYN)` + 逐键 `set_bit(keycode, keybit)`
  （漏声明时内核会直接丢弃该键码，用户态什么都收不到）
- `input_report_key` + `input_sync` 在**锁外**调用（input 子系统自带锁，嵌套会死锁）
- `input->open` → `wn_lradc_enable`，`input->close` → `wn_lradc_disable`：
  probe 后 ADC 不转换，只有真有进程 open evdev 才开始。这是键盘类驱动的标准省电语义

**L4 debugfs**：五个节点

| 节点 | 权限 | 作用 |
| --- | --- | --- |
| `regs` | 0444 | 寄存器 + 键表 + 计数器 |
| `sample` | 0644 | 实时直读 DATA0 |
| `selftest` | 0644 | 临时开 `CHAN0_DATA_IRQ` 等 100ms，验证 ISR 通路 |
| `raw` | 0644 | 任意偏移十六进制寄存器读写（扫 0x00–0xfc 全窗口） |
| `inject` | 0644 | 喂 raw 值走完整上报链路，`release` 注入抬起 |

`raw` 与 `inject` 是本轮定位硬件行为、以及在硬件不工作时验证 L3 的关键工具。

### 关键决策

1. **上报逻辑抽成函数**：`wn_lradc_report_raw()` / `wn_lradc_release()`，
   让 IRQ 路径与 debugfs 注入路径共用同一套「换算 → 匹配 → 去重 → 上报」逻辑。
   不抽的话，注入路径验证的是一份平行实现，证明不了 IRQ 路径正确。
2. **注入前自动 release**：若 `cur_keycode` 非 0，先抬起再按新键，
   模拟「手指从 A 键滑到 B 键」。不做这步第二次注入会被去重逻辑吞掉。
3. **selftest 自己开关 ADC**：L3 之后 probe 不再使能 ADC，
   但自检必须让 ADC 真的跑才能验证中断通路，所以临时自己开、测完恢复原状态
   （用 `was_open` 记录，不替用户态关掉）。

## 结果

### 验证通过的部分（真机）

环境：`radxa-cubie-a7a`（Allwinner A733 / sun60iw2p1），
`Linux 5.15.147-99btf1-a733` aarch64，8 核。

| 验证项 | 结果 |
| --- | --- |
| 板端编译 | 通过，`vermagic` 与 `uname -r` 逐字匹配 |
| probe 全链路 | 解析 5 键、映射寄存器、26 MHz 时钟、复位 deassert、`irq 428 registered` |
| `/proc/bus/input/devices` | `weaknet-lradc-keys`，`Handlers=kbd event11`，`EV=3`（EV_SYN+EV_KEY），KEY 位图精确对应 5 键码 |
| `/dev/input/event11` | 已生成 |
| `input->open` 回调 | 生效（probe 后的 `CTRL=0x02000141` 的 ENABLE=1 只可能由 open 回调写入，probe 代码不调 `wn_lradc_enable()`） |
| 寄存器读写 | CTRL 精确回读；窗口 0x00–0xfc 无 fault |
| CTRL 位布局 | 写全 1 回读 `0xffcf7fff`，实现位与 mainline sun4i **逐位相同** |
| INTC | 写 `0xffffffff` 回读 `0x00001f1f`（5 位 CHAN0 + 5 位 CHAN1） |
| 时钟 | `clk_summary` 中 `lradc` enable_count 0→1，26 MHz |
| 复位 | deassert 成功 |

**L3 端到端注入验证**（这是本轮最有力的证据）：

```text
echo 19   > inject      →  EV_KEY code=114 value=1 + EV_SYN   (KEY_VOLUMEDOWN, 410mV)
echo 41   > inject      →  EV_KEY code=114 value=0 + EV_SYN    (自动先抬起)
                           EV_KEY code=172 value=1 + EV_SYN   (KEY_HOMEPAGE, 880mV)
echo release > inject   →  EV_KEY code=172 value=0 + EV_SYN
echo 0    > inject      →  EV_KEY code=115 value=1 + EV_SYN   (最近邻匹配到 210mV)
echo 200  > inject      →  拒绝，out of range 0-63，rc=1
```

对应 dmesg：

```text
key press code=114 (raw=19 mv=407 key#1)
key release code=114
key press code=172 (raw=41 mv=878 key#4)
key release code=172
key press code=115 (raw=0 mv=0 key#0)
inject: raw 200 out of range 0-63
```

注入 5 次，evdev 收到 9 个事件，**无多余噪声**，能力位与事件序列全部正确。

### 未能验证的部分：ADC 转换从未启动

`INTS` 恒 `0x00000000`、`DATA0` 恒 `0x0000003f`、IRQ 计数恒 0。

已穷举的扫描（全部零响应）：

| 维度 | 覆盖范围 |
| --- | --- |
| `CHAN_SELECT` | 0 / 1 / 2 / 3 |
| `SAMPLE_RATE` | 0 / 1 / 2 / 3 |
| `KEY_MODE_SEL × LEVELB_VOL` | 全 16 组合 |
| `CONTINUE_TIME_SEL` | 0 / 1 / 2 / 4 / 8 / 15 |
| `FIRST_CONVERT_DLY` | 0 / 1 / 2 / 4 / 15 / 255 |
| `LEVELA_B_CNT` | 0 / 1 / 2 / 4 / 8 / 15 |
| `HOLD_EN` | 0（连续模式）/ 1 |
| 纯 ENABLE | `CTRL=0x00000001` |
| vendor 复位值 | `CTRL=0x01000168` + `INTC=0x00001f1f` |
| gpadc 争用 | unbind `sunxi-gpadc` → 无变化；rebind → 仍无变化 |

`selftest` 会打出 `selftest FAILED: no interrupt observed, ISR path is dead`——
这是**正确结论**，不是 bug。它证明「中断通路确实没被硬件触发」，而非「我的 handler 写错了」。

### 根因定位（本轮最重要的发现）

三条证据指向同一个结论：**A733 上 LRADC 的按键功能已被 gpadc 取代**。

1. **内核配置**：

   ```text
   # CONFIG_AW_LRADC is not set
   # CONFIG_KEYBOARD_SUN4I_LRADC is not set
   ```

   BSP 内核根本没有编译任何 LRADC 驱动。

2. **gpadc 的 dmesg 明确说按键在它身上**：

   ```text
   sunxi-gpadc 2521000.gpadc: sunxi_gpadc_dts_parse: warn: keyadc not select
   sunxi-gpadc 2521000.gpadc: warn: sample rate not set
   ```

   这条 warn 说明 gpadc 驱动里**有一条 keyadc 分支**，只是本板 DT 没启用。
   而 gpadc 已注册 8 个 input 设备（`sunxi-gpadc0/channel0..7/input0`）。

3. **DT 属性对比**：gpadc 节点带一整套模拟前端配置——
   `channel_select = 0xff`、`channel_data_select = 0xff`、`channel_compare_select = 0`、
   `gpadc_mode_select = 2`、`channel_num = 8`；
   而 lradc 节点**一个都没有**，只有 `clocks` / `resets` / 5 个按键。

   LRADC 的 CTRL 里也有 `CHAN_SELECT` 位，但在这颗 SoC 上，通道选择与比较器门限
   大概率已经迁到 gpadc 的模拟前端。lradc 寄存器仍在、位布局仍与 sun4i 兼容，
   但它的模拟输入通路没被 BSP 配置。

**结论**：`DATA0 = 0x3f` 不是「转换失败」，更可能是「模拟输入未连接，通道浮到 VREF 满量程」。
在这条路径上继续调寄存器不会有结果——缺的是 BSP 层面的模拟前端配置，不是驱动代码。

### 诚实边界

- **本驱动在 A733 上不能作为可用的按键输入设备。** 它的 L1/L2/L4 代码路径都能执行，
  但 L2 的触发源（ADC 转换完成/按键阈值中断）永远不会到来。
- 只有 L3 被端到端证明——而这是靠 debugfs 注入而非物理按键证明的。
  不要把「注入链路通」说成「按键能用」。
- `input->close` 未被观察到：设备被标成 `Handlers=kbd`，X server（PID 546）
  常驻持有 `event11` 的 fd，open 计数不归零，内核自然不调 close。
  这是真实桌面环境的既定行为，不是驱动缺陷。
- 未在其它 SoC 上验证。若要验证 mainline 兼容性，需要一块 `sun4i-a10` /
  `sun8i-a83t` 的真板，且要改 compatible 与 DT 键表格式。

### 后续是否值得升格为正式 OpenSpec change

**不。** 理由与 B1 实验相同：没有真实业务消费者，产出的按键事件在
`MetricId` 里没有位置。它是驱动技术栈练习，价值在于技术纵深而非产品功能。

若未来要做「板端模拟量采集」，**正确的起点是 gpadc 而不是 lradc**：
gpadc 是 12 位（lradc 只有 6 位）、已由内核驱动接管、读数稳定
（`in_voltage0..3_raw` = 1796 / 608 / 1567 / 1311）、有 IIO 标准接口。
但仍需先回答「采什么物理量、它解释哪种网络劣化」，否则就是硬凑数据。

### 与 CPU 归因工作的关系

同日另一个工作流外任务
`openspec/tasks/2026-11-06-host-pressure-observer-attribution.md`
把 `weaknet_cpu_stat` 接进了业务链路。两者都是 kmod 目录的产物，但性质不同：

| | `weaknet_cpu_stat` | `weaknet_lradc_keys` |
| --- | --- | --- |
| 性质 | 数据采集 | 驱动技术栈练习 |
| 业务消费者 | 有（`observer_attribution`） | 无 |
| 可观测命令 | `weaknet-cli get host_pressure` | 无（仅 debugfs） |
| 进主构建 | 头文件被 include | 否 |
| 硬件可用性 | 已验证 | ADC 不转换（BSP 层面） |

### 最终判定与处置（2026-11-06 补充）

编写 `驱动面能力引入与业务融合方案.md` 时，为确认「哪些驱动面能零硬件落地」
又回读了一遍板端状态，拿到两条互补证据，足以闭合「ADC 为什么不转换」这个悬案：

| 证据 | 观测 |
| --- | --- |
| LRADC（`2524000.lradc`） | 驱动侧一切正常（probe 通过、寄存器可精确回读、26 MHz 时钟、复位 deassert、IRQ 428 已注册），硬件侧 `INTS` 恒 0、`DATA0` 恒 `0x3f`、IRQ 计数恒 0 |
| GPADC（`2521000.gpadc`） | `iio:device0` 正常工作，`in_voltage0..3_raw` 有稳定读数（1797 / 608 / 1567 / 1311） |

同一块 A733 上同属 ADC 类的两个 IP，一个驱动全对而硬件不响应，另一个驱动与
硬件都正常。这排除了「驱动写错」的可能，与前面「根因定位」一节的三条证据一致：
**A733 上 LRADC 的按键功能已被 gpadc 取代**，缺的是 BSP 层的模拟前端配置。

**处置：保留源文件、不接生产链路、不改写成 GPADC 驱动、不删除。**

不改写 GPADC 的理由：通道已被 `sunxi-gpadc` 占满（`channel_select = 0xff`，
8 通道全开）、要接管就得与在树驱动仲裁（**实测板上无进程持有 `/dev/iio:device0`，
仓库内也无 iio 消费者，故不存在「破坏下游」问题**）、且按键输入对本项目无
业务价值（产品定位「不控制、不管理，只做观测与诊断」，部署形态为无人值守
机柜设备，远程操作走 `weaknet-cli` → D-Bus）。

本模块的价值因此重新定位为：驱动技术栈纵深（四层链路可复用骨架）+ 一套硬件
诊断方法学（`raw` / `inject` / `selftest`）+ 一个诚实的负面结论。完整论述见
`驱动面能力引入与业务融合方案.md` 第 13 节。

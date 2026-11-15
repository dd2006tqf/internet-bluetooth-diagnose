# 外设驱动全链路：irrx 平台驱动

> 状态：孵化中（2026-11-12 记录）
> 定位：**技术练习路线**的可行性草案。不是产品功能，不纳入当前阶段。
> 关联：`kmod/README.md` 第八节（LRADC 复盘）、`驱动面能力引入与业务融合方案.md` 第 13 节

## 一句话摘要

在板载 `irrx@7040000` 这个硬件活着、DT 声明齐全、却没有任何驱动认领的外设上，
从零写一个 `platform_driver`，把「OF 匹配 → MMIO → 时钟复位 → 真实中断 →
脉宽时序解码」这条完整链路走通。

## 动机

仓库现有两个内核模块 `weaknet_cpu_stat` / `weaknet_softirq` 属于
「字符设备 + mmap + 定时器采样」形态，它们证明的是内核模块的**加载与数据交付**能力，
但整片外设驱动的主干道没有触及：

- 没有 `probe()`，`module_init` 即执行，不关心机器上有什么硬件
- 没有 `of_match_table`，不与设备树发生关系
- 不碰 MMIO，数据来源是 `kcpustat` / `kstat_softirqs_cpu` 这类内核已有结构
- 不取时钟、不复位、不申请中断
- 资源回收靠手写 `goto` 逆序链，而非 `devm_*`

这不是缺陷——它们的任务不需要这些。但若目标是补齐外设驱动能力，
板载需要一个「有真实硬件、有真实中断、且可安全实验」的靶子。
`irrx@7040000` 是目前唯一满足前两条的候选。

## 硬件事实（板端实测）

设备树节点：

```dts
irrx@7040000 {
    compatible = "allwinner,irrx";
    reg = <0x00 0x7040000 0x00 0x400>;      /* 1 KB MMIO 窗口 */
    interrupts = <0x00 0xce 0x04>;           /* SPI 206，高电平触发 */
    clocks = <0x27 0x17 0x12 0x00 0x27 0x16>;
    clock-names = "bus", "pclk", "mclk";     /* 三路时钟 */
    resets = <0x27 0x09>;
    status = "okay";
    pinctrl-names = "default", "sleep";
    pinctrl-0 = <0x95>;                      /* PL4, function = s_ir_rx */
    pinctrl-1 = <0x96>;                      /* PL4, function = gpio_in */
};
```

平台设备侧状态：

| 项 | 观测结果 |
| --- | --- |
| `/sys/bus/platform/devices/7040000.irrx/` | 存在 |
| `waiting_for_supplier` | `0`（依赖已就绪，不缺 supplier） |
| supplier `7010000.r_ccu` | 有驱动，已绑定 |
| supplier `7025000.pinctrl` | 有驱动，已绑定 |
| 驱动认领 | **无**（`driver_override` 未曾设置，无 driver 软链） |
| 内核配置 | `CONFIG_RC_CORE=y`，`# CONFIG_AW_IR_RX is not set` |
| `ir_*.ko` | 模块目录里一个都没有 |

**一个必须先说清的混淆点**：`/sys/class/rc/rc0` 是存在的，但它的
`OF_FULLNAME` 指向 `/soc@3000000/hdmi0@5520000`、`DRIVER=allwinner,sunxi-hdmi`、
`protocols=[cec]`——那是 HDMI CEC，不是红外接收。
**不能把它当成「红外已经在工作」的证据。**

## 技术方案分层

按 LRADC 当年已经被验证过的四层结构组织，避免重新摸索：

### L1 骨架：platform_driver + OF 匹配

```c
static const struct of_device_id irrx_of_match[] = {
    { .compatible = "allwinner,irrx" },
    { }
};
MODULE_DEVICE_TABLE(of, irrx_of_match);

static int irrx_probe(struct platform_device *pdev);
static int irrx_remove(struct platform_device *pdev);
```

`module_init` 里只有一句 `platform_driver_register()`。真正的执行时机由内核决定：
解析 DT 发现 `compatible` 匹配、创建 platform_device 之后才回调 `probe()`。
这是与现有两个模块**最本质的分野**——不是代码风格差异，是「谁决定驱动何时执行」。

### L2 硬件资源四件套

全部用 `devm_*`，probe 失败或 remove 时内核按注册逆序自动释放，不写回收代码：

| 资源 | 获取方式 | 注意 |
| --- | --- | --- |
| MMIO | `devm_platform_ioremap_resource(pdev, 0)` | 1 KB 窗口，直接得到 `void __iomem *` |
| 时钟 | `devm_clk_get()` × 3 + `clk_prepare_enable()` | 三路都要开，少一路寄存器访问可能挂死 |
| 复位 | `devm_reset_control_get()` + `reset_control_deassert()` | 顺序在时钟之后 |
| 中断 | `devm_request_irq()`，IRQ 206 | SPI 高电平触发，无共享 |

`devm_` 与手写 goto 链的差别值得单独体会：现有模块在 `wn_init` 里每加一行初始化，
就得同时维护 `err_*` 标签链和 `wn_exit` 的反序清理两处；
`devm_` 把这件事变成编译期之后就无需再想的事。

### L3 中断：从寄存器到脉宽

红外接收的本质是**边沿时间戳序列**，不是「按键值」。核心工作：

1. 中断里读状态寄存器（判源）与数据寄存器（取值）
2. 写 1 清中断（W1C），漏掉这一步会立刻中断风暴
3. 用 `ktime_get()` 记录每次边沿间隔，累积成脉宽数组
4. 达到帧长或超时后，把整帧交给解码器

关键设计约束：

- **中断上下文里绝不解码**。解码涉及数组遍历与状态机，工作量大；
  正确做法是把原始脉宽拷进环形缓冲，唤醒一块 `workqueue` 或 `kthread` 处理。
- 时序敏感度是这条路线的主要难点。NEC 的引导码是 9ms 低 + 4.5ms 高，
  比特 0/1 靠 560µs 与 1690µs 的脉宽区分，容差通常在 ±25% 内。
  内核态的 `ktime_get()` 精度足够，但要考虑中断延迟抖动。
- 备用方案：若不敢自己写解码，L3 只负责采集脉宽，把 `rc-core`
  （`CONFIG_RC_CORE=y` 已在）当解码后端。这会把 L4 变成框架调用，
  少写代码但少证明能力——取决于目标。

### L4 出口：input 还是 rc-core

两条路，产物不同：

| 出口 | 做法 | 结果 |
| --- | --- | --- |
| rc-core | 实现 `struct rc_dev` 的 `driver_*` 回调，注册到 `rc-core` | `/sys/class/rc/rc1` 自动出现，解码器可插拔 |
| input | `devm_input_allocate_device()` + `input_report_key()` + `input_sync()` | `/dev/input/eventN` 出现 |

与现有模块的对照：现有模块自造 `/dev/weaknet_*` 并靠 `mmap` 交付共享内存，
等于**自己设计并长期维护一套内核 ABI**——`hdr.abi_version`、`hdr.seq` 顺序锁、
结构体布局都是兼容性义务。这条路交给内核的是二十年没变过的 `struct input_event`，
少设计一套 ABI，但自由度被框架收走。

顺带一个反向观察：现有模块在 `wn_mmap` 里用 `try_module_get()` + `vma->vm_ops`
的 close 回调，防止用户态还映射着内存时模块被 `rmmod`。那是自造字符设备的**附带义务**，
走 input/rc-core 则完全没有这个问题。

## 验证方法（不需要红外遥控器也能做）

| 手段 | 能证明什么 |
| --- | --- |
| `unbind` / `bind` 热插拔 | probe/remove 的对称性、资源是否正确释放（`devm_*` 的主要考点） |
| `cat /proc/interrupts` | IRQ 206 是否真的挂上、计数是否随信号增长 |
| debugfs 导出原始脉宽 | 中断链路是否活着，不必等解码正确 |
| 对照 `cat /sys/class/rc/rc1/protocols` | rc-core 路径是否注册成功 |

### 前置验证的一个意外障碍

原计划用板端现成的 `gpioget` 观察 PL4 在按遥控器时的电平变化。实测**行不通**：

```
$ gpioget gpiochip1 4          # PL4 = gpiochip1 基址 352 + 行 4 = 全局 356
gpioget: error reading GPIO values: Invalid argument

$ gpioinfo gpiochip1 | sed -n '6p'
line   4:      unnamed       kernel   input  active-high [used]
```

PL4 已经被 pinctrl 占用（`[used]`，consumer 显示为 `kernel`），mux 到了
`s_ir_rx` 功能上，因此不能按普通 GPIO 读。**具体是谁在何时把这个 default state
应用上去的尚未确认**——理论上无驱动时 `pinctrl_bind_pins()` 不该执行。

好在这条路并非死路：DT 里 `pinctrl-1` 就是 `gpio_in`（sleep 状态），
运行时切到该状态即可把 PL4 拉回 GPIO 功能，读电平观察信号。
这恰好也是驱动 `suspend()` 该做的事，可顺带验证 pinctrl 状态切换。

## 边界与风险

**无业务消费者。** `docs/产品定位-工业无线诊断网关.md` 第二节把「设备管理」与
「被观测对象控制」都排除了。红外遥控器既不在 Wi-Fi/Bluetooth 观测范围内，
也不产生任何诊断事实。按铁律，没有 requirement + consumer + observable command
的生产接口是 orphan surface。

**不能进发布链。** `tools/kmod_cross_build.sh:187` 的 vermagic 校验是 all-or-nothing，
任一个 `.ko` 不匹配即 `exit 1`，随后 `tools/ci.sh:180-187` 会 `rm -rf dist-arm64/kmod`，
**连生产观测模块一起删掉**。LRADC 当年就是这样拖累生产模块的
（`kmod/Makefile:28-30` 有记录）。

若保留代码，只能走 `kmod/hid_multitouch/` 的模式：独立构建单元、自己的 Makefile、
失败只告警（`tools/kmod_cross_build.sh:155-179`），或干脆不接入 `tools/ci.sh`。

**与 LRADC 的处境高度相似。** 那套驱动四层全跑通、真机验证过、debugfs 能注入模拟按键，
最后因为焊盘上没按键、业务上没消费者被删除（`openspec/tasks/2026-11-08-remove-lradc-keys-driver.md`）。
本路线的唯一优势变量是硬件确实活着，劣势变量是缺一个红外遥控器。

## 待决问题

1. **PL4 到底有没有信号**：这是整条路线的地基。需先把 pinctrl 切到 `gpio_in`
   再用 `gpioget` 观察，或用 L1+L2 的探测模块读状态寄存器。测不出则全部投入作废。
2. **解码是自己写还是交给 rc-core**：影响 L3/L4 的代码量与能力证明强度。
3. **代码做完怎么处置**：按 LRADC 的方式（保留方法论、删除源码），
   还是像 `hid_multitouch/` 那样作为独立构建单元长期留着。

## 优先级/状态

- [ ] 待评估（**明确不纳入当前阶段**）
- 前置依赖：PL4 硬件信号验证结论

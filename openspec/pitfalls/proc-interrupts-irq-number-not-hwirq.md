# `/proc/interrupts` 里的 IRQ 号不是 DTS 里的 `GIC_SPI` 编号

> 发现于：2026-11-18
> 位置：`/proc/interrupts`、`irq-gic-v3.c`、`sunxi-msgbox` DTS 节点
> 影响：按 DTS 里的 `GIC_SPI` 编号 grep 中断，得到**假阴性**，误判驱动 probe 失败

## 现象

DTS 里写的是：

```dts
interrupts = <GIC_SPI 0   IRQ_TYPE_LEVEL_HIGH>,
             <GIC_SPI 211 IRQ_TYPE_LEVEL_HIGH>;
```

于是查中断时自然这么写：

```bash
grep -E '^\s*(0|211):' /proc/interrupts
```

**结果是空的。** 得出错误结论：「中断没注册，probe 失败了」。

## 成因

**`GIC_SPI n` 里的 `n` 是 GIC 的 SPI 序号，不是 Linux 的 IRQ 号。**

在 AArch64 Linux 上，映射关系是：

```
Linux IRQ 号 = GIC_SPI 序号 + 32
                    ↑
              32 = SGI(16) + PPI(16) 占据的前 32 号
```

所以：

| DTS 写的 | 实际 Linux IRQ 号 |
| --- | --- |
| `GIC_SPI 0` | **32** |
| `GIC_SPI 211` | **243** |

而且这个偏移**不是固定的**——它取决于：
- GIC 驱动版本（`irq-gic-v3.c`）
- 是否启用 `CONFIG_ARM_GIC_V3_ITS`（ITS/LPI 会改变范围）
- 是否有 `irqchip` 层级转换

所以**不能靠算术去猜**，也不该靠猜。

## 修法

**按设备名 grep，不要按编号 grep。**

```bash
grep -i msgbox /proc/interrupts
```

实测输出（A733，`5.15.147-100btf1amp-a733`）：

```
432:  0 0 0 0 0 0 0 0  wakeupgen   0 Level  3004000.msgbox
433:  0 0 0 0 0 0 0 0  wakeupgen 211 Level  3004000.msgbox
```

注意三点：

1. **真实 IRQ 是 432 / 433，不是 32 / 243**——比简单加 32 又高出一截，
   说明中间还有 `wakeupgen` 这一层 irqchip 做了二次映射；
2. 最后一列 `3004000.msgbox` 是 `request_irq()` 时传入的 devname，
   **这是唯一可靠的匹配依据**；
3. 中间列 `0` 和 `211` 是原始 hwirq，**恰好**与 DTS 一致，
   但这只是 print 出来给你看的，不是可 grep 的索引。

## 更可靠的替代判据

如果连设备名也不确定，用不依赖 IRQ 编号的证据：

```bash
# 寄存器已 ioremap —— 最强
sudo grep -i msgbox /proc/iomem

# 驱动已绑定
ls /sys/bus/platform/drivers/sunxi-msgbox/

# 中断总数（确认有新增）
wc -l < /proc/interrupts
```

## 教训

**在 `/proc/interrupts` 里，编号列是内核内部 IRQ 号，不是硬件手册/DTS 里的编号。**
两者之间至少隔着：GIC 的 SGI/PPI 偏移、可能的 ITS 转换、可能的 irqchip 层级。

永远按 **devname 列**（最后一列）grep，不要按编号。

相关：[[mailbox-has-no-sysfs-class]]

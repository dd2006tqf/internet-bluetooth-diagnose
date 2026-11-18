# Linux mailbox 框架没有 `/sys/class/mailbox/`——按它判断 probe 会得到假阴性

> 发现于：2026-11-18
> 位置：`drivers/mailbox/mailbox.c`（5.15 主线），A733 sun60iw2
> 影响：按不存在的 sysfs 路径判断 probe，得到**假阴性**，把成功判成失败

## 现象

内核已编入 `CONFIG_MAILBOX=y` + `CONFIG_AW_MSGBOX=y`，dmesg 明确打出：

```
[    0.706162] sunxi-msgbox 3004000.msgbox: sunxi msgbox start probe
[    0.706274] sunxi-msgbox 3004000.msgbox: sunxi msgbox probe success
```

但用下面两条命令检查，**两个目录都不存在**：

```bash
ls /sys/class/mailbox/          # No such file or directory
ls /sys/bus/mailbox/devices/    # No such file or directory
```

于是第一反应是「probe 在撒谎，驱动其实没成功」。

## 成因

**Linux 5.x 的 mailbox 框架本来就不创建这两个 sysfs 目录。**

`drivers/mailbox/mailbox.c` 只维护一个 `mbox_cons`/`mbox_controllers` 链表，
通过 `mbox_controller_register()` 把控制器挂进内部链表。
它**没有** `subsystem_initialize()`、**没有** `class_create()`、**没有** bus 类型。

对比其他子系统：`/sys/class/remoteproc/` 存在（remoteproc 建了 class），
但 mailbox 从来没有。**这两个子系统的 sysfs 可见性完全不同**，
不能拿 remoteproc 的经验套 mailbox。

`/sys/class/mailbox/` 这个路径只在极少数**下游 vendor 补丁**里出现，
上游 5.15 主线没有。

## 修法

按证据强度排序：

| 判据 | 命令 | 说明 |
| --- | --- | --- |
| **最强**：寄存器已 ioremap | `sudo grep -i msgbox /proc/iomem` | 出现地址段即证明 `devm_ioremap_resource()` 走通了 |
| **强**：中断已注册 | `grep -i msgbox /proc/interrupts` | 出现即证明 `platform_get_irq` + `request_irq` 成功 |
| **强**：驱动已绑定 | `ls /sys/bus/platform/drivers/sunxi-msgbox/` | 出现设备符号链接即证明 probe 返回 0 |
| **中**：时钟已使能 | `sudo grep msgbox /sys/kernel/debug/clk/clk_summary` | `enabled=Y` 且计数 >0 |
| **弱**：dmesg 日志 | `sudo dmesg \| grep -i msgbox` | **可以被伪造/误导**，只作辅助 |

**不要用 `/sys/class/mailbox/` 或 `/sys/bus/mailbox/devices/` 作判据**——
它们不存在是正常的，不存在的结论永远是「假阴性」。

## 发现过程

先看到 dmesg 报 success，再查 sysfs 发现两个目录都没有，得出「自相矛盾」的结论。
实际是**两个证据里有一个是错的**：sysfs 路径那条根本不适用。

后来改用 `/proc/iomem` 检查，立刻看到：

```
03004000-03004fff : 3004000.msgbox msgbox@3004000
07094000-07094fff : 3004000.msgbox msgbox@3004000
```

两块寄存器区都已被驱动独占 ioremap，probe 确实成功。

## 教训

**「目录不存在」和「设备不存在」是两件事。**
在拿 sysfs 路径作判据前，先确认该子系统在目标内核版本里**是否真的建了这个 class**。
Linux 各子系统的 sysfs 暴露程度差异极大：有的建 class、有的建 bus、有的一无所有。

判断方式：查该子系统源码里有没有 `class_create` / `subsystem_initialize` /
`*_register()` 里带 sysfs 初始化。或者干脆改用 `/proc/iomem` 这类
**不依赖子系统实现细节**的证据。

相关：[[kprobe-zero-count-false-negative]]

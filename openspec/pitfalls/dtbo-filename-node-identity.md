# 从 dtbo 文件名推断设备树节点，会把「找错节点」当成「overlay 没生效」

> 发现于：2026-11-10（推论）／2026-11-11 板端实测确认
> 位置：`openspec/tasks/2026-11-08-stage2-overlay-drill-playbook.md` Step 1 / Step 3 / Step 4
> 影响：判据命令找不到节点，执行者会得到一个「看起来是失败」的正常输出

## 现象

演练手册要启用一份 overlay，文件名是：

```text
sun60iw2p1-uart2.dtbo
```

名字里带 `uart2`，于是判据写成「检查 `uart@2501*` 的 `status` 是否翻转」——
`soc` 下面 uart 控制器的地址是 `2500000`、`2501000`、`2502000` 这样排的，
`uart2` 对上第二个，也就是 `uart@2501000`。看起来很自然。

板端实测（2026-11-11）这条命令的返回是**空**：

```text
=== 手册原判据写法（uart@2501*）===
(以上为空 = 判据失效)
```

而同一时刻 overlay 明明已经生效：`/dev/ttyAS2` 存在，
dmesg 里写着 `uart-ng2: ttyAS2 at MMIO 0x2502000`。

空的输出没有错误码。执行者面对「命令跑成功了，什么都没找到」，
最自然的解读就是「overlay 没生效」——于是回滚，
一次本可以验证机制的演练被自己的判据判成失败。

## 成因

### 第一层：三套编号互不相等

| 编号体系 | 来源 | 本次的值 |
| --- | --- | --- |
| overlay 文件名 | 厂商给 dtbo 起的名字 | `uart2` |
| 设备树 unit-address | SoC 里控制器的物理基址 | `2502000` |
| alias 序号 | `aliases` 节点的赋值 | `serial2` |

`uart2` 是**厂商的命名习惯**。它这次恰好等于 `serial2`，也恰好等于
`uart@2502000`——但这个「恰好」不是契约，是巧合。

手册当初的推理错在：把 `uart2` 的 `2` 读成了「2500000 之后的第 2 个」。
而实际上 `uart2` 的 `2` 是**控制器序号**，与基地址的数值排列没有关系。

板端 `aliases` 里直接能看到这个规律并不成立：

```text
serial0 = /soc@3000000/uart@2500000
serial1 = /soc@3000000/uart@2501000
serial2 = /soc@3000000/uart@2502000
serial3 = /soc@3000000/uart@2503000
serial4 = /soc@3000000/uart@2504000
serial5 = /soc@3000000/uart@2505000
serial6 = /soc@3000000/uart@2506000
serial7 = /soc@3000000/uart@7080000     ← 地址跳了
serial8 = /soc@3000000/uart@7081000
```

`serial6` 到 `serial7` 之间，基址从 `2506000` 跳到 `7080000`。
**按地址间隔心算序号，从第 7 个开始就全错。** 地址排布是芯片设计决定的，
不是算术规律。

### 第二层：文件名的 `uart2` 到底指什么

反编译 dtbo 才能看到它真正瞄准谁：

```bash
dtc -I dtb -O dts /boot/dtbo/sun60iw2p1-uart2.dtbo
```

```dts
metadata {
    title = "Enable UART2";
    exclusive = "PB0\0PB1\0uart2";
};

__fixups__ {
    pio   = "/fragment@0:target:0";
    uart2 = "/fragment@1:target:0";     ← target 是 label "uart2"
};
```

**文件名里的 `uart2` 是 base DTB 里某个节点的 label**，不是地址、不是序号。
dtc 在应用 overlay 时按 label 解析目标，而那个 label 落在
`uart@2502000` 上。所以「文件名对得上」完全不能推出「地址是哪个」。

### 第三层（见另一篇）：`find /proc/device-tree` 恒为空

即使把 `uart@2501*` 改成正确的 `uart@2502*`，这条命令**依然返回空**。
原因是 `/proc/device-tree` 是指向 `/sys/firmware/devicetree/base` 的符号链接，
而 `find` 默认不跟随符号链接。这一层单独写在
`proc-device-tree-find-symlink.md`——它是更危险的一层，因为它让**任何**
基于 `find /proc/device-tree` 的判据都永久失效，与节点名对不对无关。

三层叠加的效果是：手册那条判据在任何情况下都不会有输出，
**看起来永远像「overlay 没生效」**。

## 修法

**不要从文件名或地址推导节点，从板子上读。**

第一步，确认这份 dtbo 到底改哪些节点：

```bash
dtc -I dtb -O dts /boot/dtbo/sun60iw2p1-uart2.dtbo 2>/dev/null \
  | grep -E "title|exclusive|target|status"
```

第二步，用两个独立来源交叉确认节点身份：

```bash
# aliases：编号映射（注意 aliases 在设备树里是**目录**，不是文件）
for a in $(ls /sys/firmware/devicetree/base/aliases); do
  echo "$a = $(tr -d '\0' < /sys/firmware/devicetree/base/aliases/$a)"
done | grep uart

# uevent：内核实际认到的 OF_FULLNAME
cat /sys/class/tty/ttyAS*/device/uevent | grep OF_FULLNAME
```

本次两者一致：

```text
serial2 = /soc@3000000/uart@2502000
OF_FULLNAME=/soc@3000000/uart@2502000
```

第三步，判据用**全量变化事实**代替预设身份：

```bash
for n in $(find -L /proc/device-tree -name "uart@*" 2>/dev/null); do
  printf '%s = %s\n' "$(basename "$n")" "$(tr -d '\0' < "$n/status")"
done | sort
```

前后各跑一次、diff，就能看见「哪一个是那个从 `disabled` 变 `okay` 的」。
本次实测结果：

```text
uart@2500000 = okay      ← console（uart0）
uart@2502000 = okay      ← 被 overlay 启用（不是 2501000）
其余 7 个 = disabled
```

这样即便最初猜错了节点，diff 也能把真相指出来。

## 边界

`aliases` **不是文件而是目录**。网上常见的
`cat /sys/firmware/devicetree/base/aliases | tr -d '\0'` 在这块板子上会报
`Is a directory`——设备树里每个 alias 是一个子节点，要逐个读子节点的内容。
两种形态（单值属性的文件、带子节点的目录）都存在，先 `ls` 一下再决定怎么读。

另外：`aliases` 只给出「编号 → 路径」，不给出「label」。要确认 dtbo 的
`target` label 落在哪个节点，只能靠反编译 dtbo + 对照 base DTB 的 label，
或者用 `uevent` 反向验证（overlay 生效后多了哪个 ttyAS）。

## 教训

**名字、地址、序号是三套互不相等的编号，任何一套都不能用来推断另一套。**

凡是从名字推出结构信息的判断，都要问一句：这个名字是谁起的、受什么约束？
厂商给 dtbo 起名时没有义务遵守内核的地址排布规律，而算法只需要
「看起来差不多」就能骗过读代码的人。

顺带一条：**判据命令本身必须先在已知正确的状态下跑出非空输出，再拿去判失败。**
本次那条 `find` 命令从来就没有工作过，它在「overlay 未启用」和
「overlay 已启用」两种状态下输出都是空的——这样的判据不具备区分能力，
却看起来完全正常。

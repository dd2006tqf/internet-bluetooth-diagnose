# 「dmesg 有报错」不等于「功能坏了」——pinctrl 报错与 console 存活可以同时成立

> 发现于：2026-11-11（板端实测）
> 位置：`openspec/tasks/2026-11-08-stage2-overlay-drill-playbook.md` 诚实边界 1
> 影响：一条三年都在的 dmesg 报错，被当成了「引脚争用」的证据，导致整个阶段 2 被挂起

## 现象

板端 dmesg 里从开机第一秒就有这几行：

```text
[    0.230603] uart-ng 2500000.uart: there is not valid maps for state default
[    0.230661] uart-ng 2500000.uart: there is not valid maps for state sleep
[    0.230907] uart-ng uart-ng0: supply uart not found, using dummy regulator
[    0.230980] sunxi:sunxi_uart_ng-uart-ng0:[INFO]: cannot get the TX DMA channel!
```

`not valid maps` 加 `cannot get the TX DMA channel`，看起来就是「串口配置是坏的」。

于是推论链建起来了：uart2 的 dtbo 声明 `exclusive = "PB0\0PB1\0uart2"`，
uart0（console）也在用 PB0/PB1 → 两者争用 → 启用 uart2 会抢走 console →
救砖通道被毁 → **先反编译 dtbo 查清再动**。整条链在文本上完全自洽。

2026-11-11 板端实测：uart2 overlay 已启用，camera overlay 也已启用，
`/dev/ttyAS0` 和 `/dev/ttyAS2` 同时在，console 会话正常：

```text
radxa    ttyAS0       2026-11-11 01:42      ← who 的真实输出
```

**报错一直在，功能一直是好的。** 那条报错从买板子那天就在，跟 overlay 无关。

## 成因

### 第一层：报错说的是「引脚表没定义」，不是「引脚配错了」

把 pinctrl 里的引脚组读出来就清楚了：

```bash
# uart0 的引脚组
$ xxd -p .../pinctrl@2000000/uart0_pins@0/pins
0000                          ← 只有一个 NUL，空字符串，size = 2

$ tr '\0' ' ' < .../pinctrl@2000000/uart0_pins@0/function
uart0

# uart2 的引脚组
$ xxd -p .../pinctrl@2000000/uart2_pins@0/pins
5042300050423100              ← "PB0\0PB1\0"，size = 8
```

`uart0_pins@0` 引用了 `uart0_pins@0` 这个 group，但那个 group 的 **`pins`
属性是空的**。pinctrl 拿到一个空引脚表，自然报 `not valid maps`——
它说的正是「这份 map 里没有有效条目」，字面意思完全正确。

**不是引脚冲突，是压根没写引脚。** uart0 的引脚在 U-Boot 阶段就被配置好了，
内核这边不需要（也没法）再配一次。留一个空的 pinctrl group 在那里，
是 Allwinner BSP 的一个遗留写法，驱动容错后照常工作。

### 第二层：`exclusive` 是给 rsetup 看的，不是内核的仲裁

dtbo 的 metadata 里：

```dts
metadata {
    title = "Enable UART2";
    exclusive = "PB0\0PB1\0uart2";
};
```

这个 `exclusive` **在内核里没有任何作用**。它是 Radxa 的 `rsetup` 工具
用来防止用户在 TUI 里同时启用两组抢同一引脚的 overlay 的**元数据**。
内核不认识这个字段，`u-boot-update` 也不读它来做冲突检测。

所以「dtbo 声明了 exclusive」，等于「这份 overlay 声称自己独占 PB0/PB1」，
不代表「内核会发现别的驱动也在用 PB0/PB1 并拒绝加载」。

### 第三层：真正的事实是「谁在用 PB0/PB1」

| 节点 | pinctrl group | pins |
| --- | --- | --- |
| `uart@2500000`（console） | `uart0_pins@0` | **空** |
| `uart@2502000`（uart2） | `uart2_pins@0` | `PB0`, `PB1` |

**只有 uart2 声明了 PB0/PB1。** console 用的 uart0 从来没有把这些引脚
写进 pinctrl——它靠 U-Boot 已经把引脚 mux 好了。

这也解释了为什么两者能共存：uart2 申请 PB0/PB1 时，内核没有找到
别的 pinctrl 消费者占着这两个引脚，于是照常分配；而 uart0 因为没有
pinctrl 声明，根本不会去申请，也就不会冲突。若 uart0 真的也声明了
PB0/PB1，内核 pinctrl 会直接拒绝第二次申请，`probe` 就会失败——那时
才会真的出现「console 坏掉」。

> 但要看清这个巧合的成分：**uart0 不声明引脚是 BSP 的遗留写法，不是设计。**
> 若哪天有人在 uart0 上补全 pinctrl（这看起来是「修复」），冲突会真的出现。
> 当前的共存依赖「一边不申请」，而不是依赖「两边都正确」。

## 修法

**把 dmesg 报错当线索，不当结论。** 验证「功能是否可用」只能用功能判据：

```bash
# 三个层次，缺一不可
ls /dev/ttyAS*                    # 设备节点在
who                               # console 上有真实登录会话 ← 最有说服力
sudo dmesg | grep "ttyAS"         # 内核确实注册了它
```

`who` 这一条特别关键：它证明的是「有人在通过这个 tty 交互」，
比「设备节点存在」强得多。设备节点可以是残留的，登录会话不能。

判据的层次要写清楚，否则「设备在」会被误读成「功能好」。
本次实测：

```text
uart@2500000 = okay       ← console
uart@2502000 = okay       ← uart2（overlay 启用后）
其余 7 个     = disabled
/dev/ttyAS0  /dev/ttyAS2
who:          radxa ttyAS0
```

三条判据全满足，且**报错依然在 dmesg 里**——这不矛盾。

## 边界

这条修法不能推广成「dmesg 报错都可以忽略」。区分标准是：

- 报错说的对象与你要验证的功能**是同一件事**吗？`not valid maps for state default`
  说的是 pinctrl group 的 map 内容，而 console 能不能用取决于 U-Boot 配好的
  引脚和驱动能否注册 tty。两者在链路上隔着好几层。
- 报错之后**有没有降级路径**？这里驱动 `using dummy regulator` 之后照常工作，
  是「容错并继续」。若换成 `probe failed`，那就是真的坏了。

对 `pinctrl` 这类报错，还要分清 `-EPROBE_DEFER`（时序问题，稍后会重试成功）
与真实失败。dmesg 里两者长得差不多。

## 教训

**「有报错」和「功能坏了」是两个独立命题，必须分别取证。**

这次的实际代价不小：一条无关的 dmesg 报错，配上一份写得像冲突声明的 dtbo
metadata，让整个阶段 2 挂起了好几天，还催生了一整套「先反编译取证」的流程。
而真相是运行一次、看一眼 `who` 就能回答——**成本几十秒，推演成本几天**。

推论链越自洽，越要问一句：链条的**第一环**是实测的还是阅读得来的？
阅读得来的环，本身可能是错的，而后面每一环都会忠实继承这个错误。

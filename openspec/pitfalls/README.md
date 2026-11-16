# 开发问题记录（openspec/pitfalls/）

> 本目录记录**开发过程中真实踩到的坑**：一个现象、一次误判、一处只有踩过才知道的边界。
> 目的不是留档，而是**下次不要再踩**，以及面试/交接时能说清「这里为什么这么写」。

## 与其它目录的分工

| 目录 | 定位 |
| --- | --- |
| `openspec/changes/` | 走完整工作流的变更（Planner → Generator → Evaluator → 归档） |
| `openspec/tasks/` | 不走工作流的**任务**记录（一次任务一个文件，答"做了什么"） |
| `openspec/pitfalls/`（本目录） | **问题**记录（一个问题一个文件，答"踩了什么、为什么、怎么绕开"） |
| `docs/ideas/` | 尚未动工的设想（孵化区） |

关键区别：`tasks/` 是按**时间线**记一件事做完了什么；本目录是按**知识点**记一个坑。
同一个任务可能产出三份问题记录，一个问题也可能横跨多个任务。

## 规则

1. **一个文件只记一个问题**。不要把三个不相干的坑塞进一篇。
2. **命名**：`<kebab-name>.md`，不带日期——问题是知识点，不是事件。
   若同一问题有多次复发，在原文件追加「复发记录」节，不新建文件。
3. **必须能回答"为什么"**。只写现象和修法是笔记，写清成因才是知识。
   写不出成因的，标注为「未查清」，并写明下一步怎么查。
4. **必须有代码锚点**。指向真实文件与实际行为，便于日后核对是否仍然成立。
   代码变了而本文未更新时，以代码为准。
5. **不写正确的废话**。"要注意内存管理"这类没有信息量的话不要写；
   本目录只记那些**看起来应该是对的、实际却不是**的地方。
6. **不受 harness 约束**，可自由编辑，但**不具备验收效力**。

## 模板

```markdown
# <问题一句话概括>

> 发现于：YYYY-MM-DD
> 位置：<文件:行 或 子系统>
> 影响：<会导致什么错误结论或行为>

## 现象

观察到什么。要具体到可复现的程度（哪条命令、什么输出）。

## 成因

为什么会这样。这一节是本文的核心，宁可长。

## 修法

怎么改的。附代码锚点。

## 边界

这个修法覆盖不到什么，或者什么条件下会重新出问题。

## 教训

一句话，能直接迁移到别的场景的那种。
```

节可以增减，但「现象 / 成因」不可缺。

## 索引

### 观测与采集

| 文件 | 一句话 |
| --- | --- |
| [cross-probe-join-dns.md](cross-probe-join-dns.md) | 跨探针拼一次观测，query 与 response 会数成两个世界（`tracker_q=45` vs `tracker_r=324`） |
| [oracle-exit-code-as-truth.md](oracle-exit-code-as-truth.md) | 拿工具进程的退出码当 ground truth，会把健康解析器判成故障 |
| [proc-stat-field-split-vs-comm.md](proc-stat-field-split-vs-comm.md) | `/proc/*/stat` 按空白取第 3 字段，comm 含空格时错位 → D 线程数不出来、判据恒通过 |
| [dmesg-not-history-and-nonmonotonic-counters.md](dmesg-not-history-and-nonmonotonic-counters.md) | 用 `dmesg` 与单次 `/proc/interrupts` 采样断言「从未发生」，把可自愈故障判成硬件损坏（149 次错误被读成 0 次） |
| [journal-log-date-attribution.md](journal-log-date-attribution.md) | 统计日志只给总数不给时间，把 5 天前（11-11）的 55771 条报错当成本次重启的现场，据此虚构出「两种失败形态」 |
| [lsof-v4l2-kills-board.md](lsof-v4l2-kills-board.md) | `lsof /dev/video0` 看起来完全无害，实际两次导致整板失联，只能物理重启 |
| [register-value-semantics-not-verified.md](register-value-semantics-not-verified.md) | 不知道寄存器/时钟值的语义就当判据，一天内错 7 次且全部静默通过 |
| [two-similarly-named-reset-functions.md](two-similarly-named-reset-functions.md) | `grep` 给的是行号不是作用域——拿 A 行的 `if` 去解释 B 行的打印，把 `work_mode` 判反，方向整个反了 |

### 帧统计与 V4L2

| 文件 | 一句话 |
| --- | --- |
| [window-span-vs-elapsed-time.md](window-span-vs-elapsed-time.md) | 窗口跨度用实际耗时算，会把「睡眠」误算成「健康」 |
| [cross-window-seq-gap.md](cross-window-seq-gap.md) | 跨窗口比较帧序号，会稳定产生一次假丢帧 |
| [float-truncation-frame-count.md](float-truncation-frame-count.md) | 用浮点算「本该收到多少帧」，截断会少算一帧 |
| [requeue-buffer-reuse.md](requeue-buffer-reuse.md) | 复用 DQBUF 写过的 buffer 做 requeue，依赖了内核未承诺的行为 |
| [media-enum-next-flag.md](media-enum-next-flag.md) | 迭代式 ioctl 用 0..N 循环枚举，会重复拿到同一批实体（29 条而非 11 个） |
| [sensor-mode-table-fps-lie.md](sensor-mode-table-fps-lie.md) | 模式表的 `pclk=278MHz` 与 `fps_fixed=30` 互相配平，实际 PLL 只有 172.8MHz → 恒定 38% 假丢帧 |
| [subdev-parm-to-devnode-parm.md](subdev-parm-to-devnode-parm.md) | 子设备 `s_parm` 只存不用，父节点照样返回 `rc=0`，`S_PARM` 对帧率是空操作 |
| [fixed-cadence-rules-out-drop.md](fixed-cadence-rules-out-drop.md) | 帧间隔单箱 ±0.4% 说明下游一帧没丢，低帧率是「产生得慢」不是「丢得多」 |
| [ctrl-readback-vs-register.md](ctrl-readback-vs-register.md) | 控制框架回读 `exposure=4036`，实际快门寄存器是 `1000`，跨层对照才戳得穿 |
| [gst-v4l2src-fps-artifact.md](gst-v4l2src-fps-artifact.md) | `gst v4l2src` 把同一台设备量成 6.86fps（实为 18.56），测帧率不能用它 |
| [black-frame-is-lens-film.md](black-frame-is-lens-film.md) | 全黑帧的 `max=60` vs `max=252` 是镜头保护膜的判据，不是 sensor 故障 |
| [v4l2-sizeimage-selfconsistent-wrong.md](v4l2-sizeimage-selfconsistent-wrong.md) | `S_FMT` 后读到的 `sizeimage` 是个**自洽的错值**（3030720 而非 3045504），能通过为它写的每一道 guard |
| [tdm-silent-skip-on-zero-pixel.md](tdm-silent-skip-on-zero-pixel.md) | TDM 有一处 `if (!speed_dn_en) return 0;`：`STREAMON` 成功、无日志、中断零次，整条链看不出任何异常 |

### 归因与数据可达性

| 文件 | 一句话 |
| --- | --- |
| [resolver-whitelist-attribution-gap.md](resolver-whitelist-attribution-gap.md) | 诊断规则的字段解析器只认 SLE evidence，没有字段等于永远无法归因 |

### 线程接线与生命周期

| 文件 | 一句话 |
| --- | --- |
| [capture-before-creation-null-consumer.md](capture-before-creation-null-consumer.md) | 线程启动时捕获下游对象的 `get()`，必然是空指针且永不刷新，失败被 `&&` 短路吞成一行含糊日志 |

### 设备树与硬件实测

| 文件 | 一句话 |
| --- | --- |
| [proc-device-tree-find-symlink.md](proc-device-tree-find-symlink.md) | `find /proc/device-tree` 永远是空的——它是个符号链接，而 find 默认不跟随 |
| [dtbo-filename-node-identity.md](dtbo-filename-node-identity.md) | 从 dtbo 文件名推断设备树节点，会把「找错节点」当成「overlay 没生效」 |
| [dmesg-error-vs-function.md](dmesg-error-vs-function.md) | 「dmesg 有报错」不等于「功能坏了」——pinctrl 报错与 console 存活可以同时成立 |

### 驱动调试接口

| 文件 | 一句话 |
| --- | --- |
| [vin-log-compiled-out.md](vin-log-compiled-out.md) | `CONFIG_VIN_LOG` 依赖 `DYNAMIC_DEBUG`，本板没开 → 整个 VIN 驱动的 `vin_log()` 是 `do{}while(0)`，调 `vin_log_mask` 永远空转 |
| [mpp-mipi-debugfs-panics.md](mpp-mipi-debugfs-panics.md) | `/sys/kernel/debug/mpp/mipi` 是 0444，但 `open` 里直接 `readl` 已断电模块的寄存器 → panic 且 `PANIC_TIMEOUT=0` 永不重启，`timeout` 也救不了 |

### 构建与工具链

| 文件 | 一句话 |
| --- | --- |
| [execute-process-foreign-binary-sh-fallback.md](execute-process-foreign-binary-sh-fallback.md) | `execute_process` 探测异构架构二进制，内核回退 `/bin/sh` 解析 ELF，在 cwd 里造出名字是乱码字节的垃圾文件 |
| [install-no-parent-dir-with-set-e.md](install-no-parent-dir-with-set-e.md) | `install` 不建父目录，`set -e` 脚本里新增一行就让整个打包静默中断 |

## 一条元教训（跨越多篇）

这几篇里有四篇是同一类错误的变体：**判据/测量本身没被验证**。

- `proc-device-tree-find-symlink`：判据在成功和失败时输出相同，不具备区分能力；
- `oracle-exit-code-as-truth`：oracle 测的不是要测的那个量；
- `dmesg-error-vs-function`：线索被当成了结论；
- `dtbo-filename-node-identity`：用名字推结构，而那正是该被验证的东西。

共同特征：**它们都不会报错**。工具正常运行、命令成功返回、日志刷得好好的，
只是结论是错的。所以验收流程里每加一条命令，都要先问：
「它在已知正确的状态下输出什么？在已知错误的状态下又输出什么？
如果两者一样，它就不是判据。」

### 2026-11-11 补充：这条元教训在 V4L2 排查里复发了一整族

同一天新增的六篇里，**有五篇**是「判据没被验证」的不同变体：

| 文件 | 判据声称的 | 实际是 |
| --- | --- | --- |
| `sensor-mode-table-fps-lie` | 模式表写着 30fps / 278MHz | PLL 只到 172.8MHz，真实 18.56fps |
| `subdev-parm-to-devnode-parm` | `S_PARM` 返回 0 = 生效 | 只是存进一个没人读的结构体 |
| `ctrl-readback-vs-register` | `G_CTRL` 报 `exposure=4036` | 寄存器是 `1000`，两条路径不相干 |
| `gst-v4l2src-fps-artifact` | gst 报 6.86fps | 同设备另外三条路径都是 18.5 |
| `fixed-cadence-rules-out-drop` | 帧率低 = 在丢帧 | 单箱分布说明一帧没丢，只是产生得慢 |

（第六篇 `black-frame-is-lens-film` 不是判据问题，是「跳过物理层直接查软件」。）

**这一族的共同触发条件是「跨层」**：声明跨层（表 vs 寄存器）、
调用跨层（userspace vs 驱动状态）、读取跨层（控制框架 vs 硬件）、
工具跨层（gst vs 裸 ioctl）、概念跨层（丢帧 vs 低帧率）。
每跨一层就多一次失真机会，而**失真不会报错**。

所以往后凡是做跨层判断，先在最下层取一次真值，再和上层声明的对照。
本次靠的就是一条 20 行的裸 I2C 读寄存器程序——它是整轮排查里
唯一没有经过任何抽象层的数据来源，也是唯一戳穿全部假象的那一条。

### 2026-11-14 补充：「校验通过」也可能毫无意义

`v4l2-sizeimage-selfconsistent-wrong` 是同一族里更隐蔽的一档：

前面的坑是「判据在成功和失败时输出相同」，这一篇是「**判据两边取自同一个
错误来源，于是互相印证**」。`sizeimage` 与 `bytesperline` 都来自同一次不完整的
`G_FMT`，guard 拿它俩互验，在数学上恒成立（而且恰好卡在等号上），
于是「没校验」和「校验通过」完全不可区分。

由此再加一问：**这条判据的两个操作数，是不是来自同一个我不完全信任的来源？**
若是，它证明的只是来源内部自洽，不是与事实相符。

这次戳穿它的仍然是最下层的事实——**帧内容的零字节游程**：
真实数据的空洞位置不撒谎，而任何 `ioctl` 回报的字段都可能。

### 2026-11-15 补充：有些坑只能靠「换一个编译器」和「真跑一遍打包」发现

这一轮的两篇（`proc-stat-field-split-vs-comm`、`install-no-parent-dir-with-set-e`）
有一个共同的、值得记住的特征：**它们在被发现时，所有既有关卡都是绿的**。

| 关卡 | 当时状态 | 为什么没拦住 |
| --- | --- | --- |
| x86 `g++ -Werror` | 通过 | g++ 不报未使用的 `constexpr`（clang 报） |
| 78 项单元测试 | 全通过 | 测试只覆盖「数出来之后怎么判」，没覆盖「能不能数出来」 |
| `bash -n` 语法检查 | 通过 | 语法完全合法，是路径不存在 |
| 代码审查 | 会漏 | diff 只有一行 `install`，看起来完全正常 |

两条的发现方式分别是：**换 ARM64 clang 编译**时由一条看似无关的
`-Wunused-const-variable` 告警牵出来；以及**逐行核对打包段路径**时，
用 `install` 亲手实测一次「父目录不存在会怎样」确认。

由此再多一问：**我这次的验证，覆盖的是「我写的逻辑对不对」，还是「我以为存在的
前置条件是否真的成立」？** 单元测试天然只能验证前者。后者需要跑真实的
构建/打包/部署链路 —— 而在没有板子的时候，至少可以跑「打包到 dist 为止」。

另外一条更具体的教训：**多编译器交叉验证的成本极低、收益不对称**。
g++ 与 clang 对「未使用的定义」告警策略不同，这一条差异直接指向了一处
从未被认真执行过的代码路径。

### 2026-11-16 补充：观测通道本身要先被验证一次

这一轮的两篇（`vin-log-compiled-out`、`mpp-mipi-debugfs-panics`）
不是「判据错了」，而是**根本拿不到判据**：

| 通道 | 我以为它是 | 实际是 |
| --- | --- | --- |
| `dmesg` 里的 VIN 调试日志 | 驱动在打，只是等级被 mask | 宏被编译成 `do{}while(0)`，一行都不存在 |
| `/sys/kernel/debug/mpp/mipi` | 0444 只读，可以小心地读一次 | `open` 里 `readl` 已断电的 PHY，一读即 panic |

两者的共同点是**通道的失效是静默的**：写 `vin_log_mask` 成功返回、
`cat` 一个 0444 文件在权限上完全合法。没有任何一步会告诉你
「你正在看的是一个空的观测面」。

所以在这一族里，「取一次真值」的做法本身也不够 —— 你取的那个真值
可能来自一条已经断掉的线。前置动作应该是：**先确认这条线是通的**。

- 对日志：做一次**阳性对照**，找一条已知必然发生的行，确认它可见。
  （本案的对照是 `vin_err` 的 `tdm0 rx0 frame error`：它可见，
  所以 `vin_log` 的缺席是宏的问题，不是通道的问题。）
- 对 debugfs / sysfs / ioctl：先读源码里那个 `open` / `show` / handler
  到底做了什么，再决定碰不碰。**权限位描述的是语义，不是行为。**

一句话：**在真机上，最贵的错误不是算错，而是对着一个坏掉的仪表盘
得出「一切正常」。**

### 2026-11-16 追加：「读对了源码」也可能推出反的结论

同一天晚些时候又来一篇（`two-similarly-named-reset-functions`），
它和上面那一族不同：**仪表盘是好的，源码也读对了，错在把两行归给了同一个函数。**

`vin_tdm.c` 里 `:1479` 的 `if (work_mode == TDM_OFFLINE) return;` 属于
`__sunxi_tdm_wdr_reset`（:1465），而 `:1814` 那条 `reset!!!` 打印属于
`__sunxi_tdm_reset_v2_handle`（:1746）—— 两个函数，相隔 335 行，
中间还夹着一个 `__sunxi_tdm_reset_v2`（:1607）。名字共享
`sunxi_tdm_reset` 主干，`grep` 一次就能同时命中，于是被我当成了同一个作用域。

后果是「`reset!!!` 只可能出现在 ONLINE」这个结论恰好反了 ——
真正的门是 `tdm_rx_reset[]` 数组，而它**只在 `tdm_rx_set_reset()` 里置 1**，
那个函数开头就是 `if (work_mode == TDM_ONLINE) return;`。
两跳之后，`reset!!!` 变成 OFFLINE-only。

由此多一问：**我引用的这一行，它的作用域到哪里结束？**
`grep` 输出的是行号，不是函数边界。长文件里凡是靠「A 行命中符号 X、
B 行也命中符号 X」建立的因果，都要先把函数起始行也 `grep` 出来做区间归属。
C 语言的缩进和空行不会阻止你读错，只有函数边界会。


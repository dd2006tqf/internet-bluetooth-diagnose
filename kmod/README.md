# kmod —— 实验性内核模块技术栈（B1 路径）

本目录是 `内核模块技术栈引入讨论.md` 中**方向二 / 路径 B1** 的落地实现。
目的是把项目的观测能力从用户态/eBPF 延伸到内核态，验证这条技术路线是否值得
继续投入。

目录里有三个模块，分两类：`weaknet_cpu_stat` 与 `weaknet_softirq` 是**数据采集**，
`weaknet_lradc_keys` 是**驱动技术栈练习**（见第八节）。

> **定位：实验代码，不进生产链路。**
> 不进 `tools/ci.sh`、不进 OTA manifest、不注册 `IMonitorPlugin`、
> 不新增 D-Bus 方法、不进 `config.yaml`。全部使用方式就是板端手工
> `make` → `insmod` → 跑 `tools/wn_kmod_read` → `rmmod`。
>
> 换句话说：**当前项目的主构建、部署流程、发布产物都不会因为本目录存在而改变。**
> 如果有人发现某个脚本引用了本目录，那是个 bug。
>
> **例外（2026-11-06）**：`weaknet_cpu_stat` 的共享 ABI 头 `weaknet_kmod_abi.h`
> 已被服务端 `server/src/host_pressure_monitor.cpp` include，用来把 CPU 压力接进
> 观测者降级归因（`CPU_BUSY_RATIO` → `observer_attribution`）。这是本目录第一次
> 进入主构建，但**只依赖头文件**：不加载 .ko、不打包 .ko、不要求设备节点存在。
> 设备节点缺失时该消费者恒报 `UNAVAILABLE`，不伪造 0，业务链路行为不变。
> 其余边界（不进 CI、不进 OTA manifest、不新增 D-Bus 方法）仍然成立。

---

## 一、三个模块

| 模块 | 源文件 | 作用 | 用户态出口 |
| --- | --- | --- | --- |
| `weaknet_cpu_stat` | `weaknet_cpu_stat.c` | per-CPU 累计时间（user/nice/system/idle/iowait/irq/softirq/steal/guest/guest_nice） | `/dev/weaknet_cpu_stat` |
| `weaknet_softirq` | `weaknet_softirq.c` | per-CPU 十类软中断累计触发次数 | `/dev/weaknet_softirq` |
| `weaknet_lradc_keys` | `weaknet_lradc_keys.c` | Allwinner A733 LRADC 按键驱动（L1 骨架 / L2 中断 / L3 input / L4 debugfs） | `/dev/input/eventN` |

前两个同构，是**数据采集**类模块；第三个性质不同，是**驱动技术栈练习**，见第八节。
三者都不进生产链路。

前两个模块同构：`hrtimer` 1 Hz 采样 → 写入内核私有缓冲区 → seqlock 保护下
`memcpy` 到 `vmalloc_user` 共享页 → 用户态 `mmap` 只读访问。

共享的 ABI 结构定义在 `weaknet_kmod_abi.h`，内核模块与用户态工具**包含同一份头文件**，
避免两边结构体各写一遍导致布局漂移。

## 二、构建与运行

```bash
# 1. 板端就地编译（需要 linux-headers）
cd kmod && make

# 2. 加载
sudo insmod weaknet_cpu_stat.ko
sudo insmod weaknet_softirq.ko

# 3. 编译并运行用户态读取工具
gcc -Wall -Wextra -O2 -o /tmp/wn_kmod_read tools/wn_kmod_read.c
sudo /tmp/wn_kmod_read cpu                      # 单次累计值
sudo /tmp/wn_kmod_read cpu --count 5 --interval 1   # 区间使用率
sudo /tmp/wn_kmod_read softirq                  # 软中断累计计数

# 4. 卸载（映射存活时会被内核拒绝，这是预期行为）
sudo rmmod weaknet_cpu_stat weaknet_softirq
```

`Makefile` 支持三种用法，注释里写了：板端就地编译（默认）、宿主机对拍编译
（`KDIR=...`）、交叉编译（`ARCH=arm64 CROSS_COMPILE=... KDIR=...`）。

## 三、相对参考实现的改造点

参考实现是 `monitor_system` 的 `worker/src/kmod/cpu_stat_collector.c`
（MIT，`cpp-agan-team/monitor_system`）。本目录**按目标结构重写，不复制源码**；
源项目仅作规格参照，其 MIT 条款与署名在下方「来源与许可」一节保留。

重写时逐项处理的问题（编号沿用讨论稿第五节）：

| 编号 | 问题 | 本实现的处理 |
| --- | --- | --- |
| D1 | 跨字段撕裂读：源实现无锁直接写 mmap 页，用户态可能读到半新半旧的数据 | 私有缓冲区 + seqlock 代次号，见下节 |
| D2 | `pgprot_noncached` 用错：把普通 RAM 设成非缓存，拖慢每次读 | `vmalloc_user` + `remap_vmalloc_range`，走普通 cached 映射 |
| D3 | ~~guest 重复计入~~ **不成立**，源实现没把 guest 算进 busy | 保留 `guest`/`guest_nice` 原样导出，由用户态按 `/proc/stat` 口径排除 |
| D3′ | `idle`/`iowait` 被 `usecs_to_jiffies()` 二次截断，丢掉 tickless 的微秒精度 | 直接输出微秒值 ×1000 转纳秒，不做二次截断 |
| D4 | 内核 `MAX_CPUS 256` 与用户态硬编码 `128` 不一致，潜在越界 | 头部携带 `nr_cpus`，用户态按此遍历 |
| D5 | 无法区分离线 CPU | 每条记录带 `online` 标记 |
| D6 | 无权限校验 | `open()` 校验 `capable(CAP_SYS_ADMIN)` |
| D7 | 构建链不可移植 | `KDIR ?=` 可覆盖；板端开箱即用 |

补充两处源实现没有、但这里必须加的：

**1. 映射存活期间禁止卸载。** `mmap` 里 `try_module_get(THIS_MODULE)`，
`vm_ops->close` 里 `module_put()`。否则用户态还持有映射时 `rmmod` 成功，
`vm_ops` 回调会跳进已释放的代码段。

**2. 只读映射强制。** `mmap` 里检查 `vma->vm_flags & VM_WRITE` 并返回 `-EACCES`。

这一条容易漏，因为 `remap_vmalloc_range` **不**替你拒绝 `PROT_WRITE`。
曾经实测过漏掉的后果：root 用 `PROT_READ|PROT_WRITE` 映射成功后，
直接把 `hdr.sample_ns` 从 `1374934036214` 改成 `18446744073709551615`，写入生效。

更麻烦的是**seqlock 挡不住这种篡改**——它的前提是「一个写者、多个读者」，
而用户态一旦可写就变成了第二个写者：改完 `seq` 后读者照常校验通过并采信，
一致性保证被彻底架空。所以这不是「防君子」的加固，是协议成立的必要条件。

## 四、seqlock 快照协议

内核侧（`wn_publish`）：

```c
s = READ_ONCE(dst->hdr.seq);
WRITE_ONCE(dst->hdr.seq, s + 1);   /* 奇数：写入中 */
smp_wmb();
/* ... 写 sample_ns / nr_cpus / memcpy(cpu[]) ... */
smp_wmb();
WRITE_ONCE(dst->hdr.seq, s + 2);   /* 偶数：一致 */
```

用户态（`read_snapshot`）：

```c
do {
    s0 = seq;
    if (s0 & 1) continue;      /* 内核正在写，重试 */
    memcpy(dst, map, size);    /* 先拷贝 */
    fence();
    s1 = seq;                  /* 再校验 */
} while (s0 != s1);
```

**顺序不能反**：如果先校验 seq 再交给调用方拷贝，两次操作之间内核可能又写一轮，
拿到的仍是撕裂数据。必须「拷贝 → 校验 → 不一致重试」。

一致性靠内存屏障，不靠 cache 属性——这也是 D2 的结论。

## 五、板端实测结果（2026-11-05）

环境：`radxa-cubie-a7a`，`Linux 5.15.147-99btf1-a733` aarch64，8 核，
`CONFIG_HZ=250`、`CONFIG_NO_HZ_IDLE=y`、`CONFIG_TICK_CPU_ACCOUNTING=y`、
`CONFIG_VIRT_CPU_ACCOUNTING_GEN` 未设置。

| 验证项 | 结果 |
| --- | --- |
| 板端 `make` 编译 | 通过；`vermagic=5.15.147-99btf1-a733 SMP preempt mod_unload aarch64`，与 `uname -r` 逐字匹配 |
| `insmod` 两个模块 | 均 rc=0；设备节点正常创建；`dmesg` 打印 8 个 possible CPU、map 24576 字节 |
| 与 `/proc/stat` 对拍 | CPU0 `user` 模块 23.13 s，`/proc/stat` 2313 tick @USER_HZ=100，逐项一致 |
| 与 `/proc/softirqs` 对拍 | 十类计数逐 CPU 全部一致 |
| 区间使用率 | `Δtotal ≈ 1.0e9 ns`，量纲正确 |
| 权限门禁 | 同一 root 身份，`capsh --drop=cap_sys_admin` 后打开设备返回 `EPERM(1)`，加回则成功 |
| 只读映射 | `PROT_READ\|PROT_WRITE` 映射被拒绝（`EACCES`），仅 `PROT_READ` 成功 |
| 卸载安全性 | 映射存活时 `rmmod` 被拒绝（`Module weaknet_cpu_stat is in use`，refcount=3）；进程退出后 rc=0 |
| 撕裂检测 | 30 s 内 666,726 次读取（约 22k 次/秒），覆盖约 60 个写入窗口，**0 次撕裂、0 次重试耗尽** |
| CPU 热插拔 | 离线 CPU 的 `idle` 完全冻结（12 s 内三次读数一致），不再跟随 wall clock；`valid` 在离线↔上线切换时置 0，用户态显式跳过该轮 |
| 长稳（30 分钟） | 58,512,235 次读取（32,507 次/秒），**0 失败 / 0 撕裂 / 0 单调性违规 / 0 seq 倒退**；进程 RSS 恒定 10,156 kB（Δ+0）；内核采样节拍实测 1.000 Hz（`seq` 262→3862） |

### 一个必须记住的精度边界

`CONFIG_HZ=250` 意味着 `kcpustat` 的 busy 类字段（user/nice/system/irq/softirq/steal）
本身就按 `TICK_NSEC = 1e9/250 = 4 ms` 累加——这是数据源的固有粒度，
**不是本实现引入的**，也无法通过「输出纳秒而不是 jiffies」消除。
实测确认：8 个 CPU × 8 个 busy 类字段全部是 4,000,000 ns 的整数倍，无一例外。

真正的精度收益只在 `idle`/`iowait`：它们走 `get_cpu_idle_time_us()` 的微秒路径，
本实现保留微秒，源实现用 `usecs_to_jiffies()` 又压回 4 ms。

因此，若要 1 秒粒度的使用率，4 ms 量化带来的误差上限约 0.4%，可接受；
若要更细的采样间隔，这个下限会成为瓶颈，需要换
`CONFIG_VIRT_CPU_ACCOUNTING_GEN` 或 `CONFIG_IRQ_TIME_ACCOUNTING`（本板均未开启）。

### 长稳与泄漏测试

`tools/wn_kmod_soak.py` 是专用长稳压测工具，与 `wn_kmod_read.c` 用同一套 seqlock
读者逻辑，但独立用 Python 实现——避免 C 工具自身的问题掩盖模块问题。

```bash
# 30 分钟长稳（后台）
sudo nohup python3 tools/wn_kmod_soak.py --duration 1800 > /tmp/soak.log 2>&1 &

# 短测（脚本自检）
sudo python3 tools/wn_kmod_soak.py --duration 60
```

它每个迭代同时读两个模块，检查：

| 判据 | 说明 |
| --- | --- |
| 读取失败率 | seqlock 重试耗尽次数 |
| 粒度违规 | busy 类字段必须是 `TICK_NSEC` 整数倍 |
| 单调性违规 | 累计值不得倒退（仅 `valid=1` 时检查） |
| softirq 单调违规 | 十类软中断计数不得倒退 |
| seq 倒退 | 内核发布代次必须单调递增 |
| ABI 错误 | `abi_version` 必须等于工具期望值 |
| 进程 RSS | 泄漏检测，必须零增长 |
| 采样节拍 | `sample_ns` 推进速率应≈1.0 Hz |

**2026-11-05 实测结果（30 分钟，板端生产服务同时在跑）**：

```text
时长            : 1800.0 s
迭代次数        : 58512235 (32507 次/秒)
cpu 读取        : 成功 58512235 / 失败 0
softirq 读取    : 成功 58512235 / 失败 0
粒度违规        : 0
单调性违规      : 0
seq 倒退        : 0
ABI 错误        : 0
内核采样推进    : seq 262 -> 3862, 时间 1800.0 s
采样节拍        : 期望 1.0 Hz, 实测 1.000 Hz
进程 RSS        : 10156 -> 10156 kB (Δ+0)
结论: PASS
```

`Slab` 同期上涨约 1.5 MB，但同机有 `weaknet-dbus-server` 等生产服务在跑，
不能归因到本模块；进程 RSS 严格零增长是更有力的无泄漏证据。

## 六、CPU 热插拔：一个必须知道的陷阱

这是实测中踩到的坑，比预想的深，值得单独记一节。

### 陷阱：`get_cpu_idle_time_us()` 不判 CPU 在线

`kernel/time/tick-sched.c` 里这个函数的开头只有一句守卫：

```c
if (!tick_nohz_active)
    return -1;
```

**它不检查 `cpu_online()`。** 它读的是 `per_cpu(tick_cpu_sched, cpu)` 的
`idle_sleeptime` / `idle_entrytime` 两个字段。而 CPU 下线时
`tick_cancel_sched_timer()` 会执行：

```c
memset(ts, 0, sizeof(*ts));
```

把整块结构清零。之后 `idle_active=0`、`idle_entrytime=0`，函数走 else 分支：

```c
idle = ktime_add(ts->idle_sleeptime, delta);   // 0 + (ktime_get() - 0)
```

于是它**返回自开机以来的绝对时间**，且每次采样都在增长。实测（CPU6/CPU7 离线）：
值从 1533 s 掉到 1.58 s，之后每 2 秒精确 +2.00 s——它跟的是 wall clock，
不是 CPU 空闲时间。

**危害**：用户态用 `(idle_a - idle_b)` 算区间增量会得到负数或远超 100% 的值。
而且这个错误值**不会返回 -1**，所以「失败就回落 kcpustat」的分支根本不触发。

### 修法：离线 CPU 绕开 tick 路径

```c
static u64 wn_idle_ns(int cpu)
{
    u64 us;

    if (!cpu_online(cpu))
        return kcpustat_cpu(cpu).cpustat[CPUTIME_IDLE];   /* 冻结的累计值 */

    us = get_cpu_idle_time_us(cpu, NULL);
    ...
}
```

已实测确认：CPU 离线期间 `kcpustat` 的纯累计字段（user/system/softirq 等）
**完全冻结**，5 秒内一个字节都不变。所以对离线 CPU 直接读 kcpustat 既安全
又语义正确——报的是该 CPU 下线前的最终累计值。

`idle` 的修复效果：离线后 12 秒内三次读数完全一致（1785.72 s），不再漂移。

### 第二个坑：`valid` 标记的必要性

即使 `idle` 修好了，还有一个残留问题：`kcpustat` 与 `tick_cpu_sched` 的
生命周期不一致。前者跨热插拔保留累计，后者被 `memset` 清零。这意味着
「离线→上线」这一对快照的**不同字段可能处于不同基准**，直接相减不可靠。

因此 ABI v2 给每条记录加了 `valid` 字段：

```c
WN_U32 online;   /* 当前这一刻是否在线 */
WN_U32 valid;    /* 本记录能否与上一份快照做差值 */
```

内核侧记住上一轮各 CPU 的 online 状态，**只要与上一轮不同就置 `valid=0`**。
用户态必须显式跳过，不能靠 `tot_a <= tot_b` 之类的判断去猜——那只是巧合，
不是契约。

实测确认：跨过恢复点的那一轮输出 `<累计值已重置，跳过本轮>`，
下一轮自动恢复正常。

### 仍然存在的语义限制

CPU 下线时 `tick_cpu_sched` 被清零，因此该 CPU **自开机以来经
`get_cpu_idle_time_us` 累计的那部分历史（若在线期间有 tickless 空闲）
在重新上线后不可恢复**。`kcpustat` 的部分会保留，两者不再严格可比。

如果业务要求「跨热插拔的绝对累计值」，需要在内核侧自行维护一份
per-CPU 的累计快照与基线，这是当前实现没有做的。

## 七、没有做的事（边界）

- **没有**接入生产链路（`IMonitorPlugin` / D-Bus / `config.yaml` / SQLite）。
- **没有**做热插拔 CPU 的**绝对累计值**跨周期保持（见第五节末尾的语义限制）。
  当前实现只保证「热插拔期间不产出错误数据」，不保证「累计值连续」。
- **没有**在超过 8 核的机器上验证（`WEAKNET_KMOD_MAX_CPUS` 为 256，但只实测了 8 核）。
- **没有**在 `CONFIG_VIRT_CPU_ACCOUNTING_GEN=y` 的内核上验证
  （本板未开，`kcpustat_cpu_fetch` 走的是内联 `*dst = kcpustat_cpu(cpu)` 分支）。
- **没有**做长期稳定性测试（>1 小时）与内存泄漏检测。
- **没有**做跨内核版本兼容性矩阵，只验证了 5.15.147 与编译期的 6.8。
- **没有**在 `weaknet_lradc_keys` 上验证到真实 ADC 转换（本板硬件行为，见第八节）。
  它的 L3 链路是靠 debugfs 注入证明的，不是靠物理按键。
- **没有**让 `weaknet_lradc_keys` 的 `input->close` 被观察到（X server 常驻占用
  evdev 节点，open 计数不归零，见第八节）。

## 八、`weaknet_lradc_keys`：LRADC 按键驱动

前两个模块是数据采集，这第三个性质不同：它是**驱动技术栈的完整练习**，覆盖
`platform_driver` / OF 匹配 / MMIO / 时钟 / 复位 / 中断 / input 子系统 / debugfs
七件事。目标板 `radxa cubie a7a`（Allwinner A733 / sun60iw2p1）。

### 为什么不用 mainline 驱动

Linux 主线有 `drivers/input/keyboard/sun4i-lradc-keys.c`，但在这个板上用不了：

1. **compatible 不匹配**——主线只认 `allwinner,sun4i-a10-lradc-keys` 和
   `allwinner,sun8i-a83t-r-lradc`，本板 DT 用的是 BSP 私有串
   `allwinner,keyboard_1350mv`。
2. **DT 键表格式不同**——主线用 `channel { linux,code; ... }` 子节点；本板用
   `key_cnt = <5>` + `key0..key4 = <voltage_mv keycode>` 成对属性，没有子节点。
3. **没有 `vref-supply`**——主线靠 regulator 取参考电压，本板把电压编码在
   compatible 字符串后缀里（`1350mv`），只能从字符串解析。
4. **时钟/复位没被主线驱动管**——主线完全不碰 clk 与 reset，本板不显式
   `clk_prepare_enable` + `reset_control_deassert` 就拿不到可用寄存器。

因此这里选择重写而非移植，寄存器与位定义参照主线（GPL-2.0-or-later,
Copyright (C) 2014 Hans de Goede），其余部分为本项目重写。

### 硬件事实（实测）

| 项 | 值 |
| --- | --- |
| 寄存器基址 | `0x02524000`，长度 `0x100` |
| 中断 | IRQ 428（SPI 37，电平触发） |
| 时钟 | `lradc`，26 MHz |
| 复位 | DT resets phandle `0x12` / `0x47` |
| 上电初值 | `CTRL=0x01000168`（ENABLE=0），`DATA0=DATA1=0x3f` |
| 参考电压 | 1350 mV（来自 compatible 后缀） |
| 按键表 | key0 210mV→KEY_VOLUMEUP(115) / key1 410→VOLUMEDOWN(114) / key2 590→MENU(139) / key3 750→ENTER(28) / key4 880→HOMEPAGE(172) |

寄存器布局集中在源文件 1.2 节的 `#define` 块：`CTRL 0x00` / `INTC 0x04` /
`INTS 0x08` / `DATA0 0x0c` / `DATA1 0x10`，`INTS` 为写 1 清零（W1C），
采样值取 `DATA0 & 0x3f`（6 bit，满量程 63）。

### 四层实现

| 层 | 内容 | 关键点 |
| --- | --- | --- |
| L1 骨架 | `platform_driver` + `of_match_table` + DT 解析 + `devm_platform_ioremap_resource` + clk + reset | 资源全部走 `devm_*`，错误路径自动回收 |
| L2 中断 | `devm_request_irq` + `INTS` 判源 + `DATA0` 读值 + 电压换算 + W1C 清中断 | 先 `request_irq` 再打 `INTC` 使能位，避免电平中断无人应答被记 spurious |
| L3 input | `devm_input_allocate_device` + `set_bit(EV_KEY/EV_SYN)` + 逐键 `keybit` + `input_report_key` + `input_sync` | 上报在**锁外**做（input 子系统自带锁，嵌套会死锁） |
| L4 debugfs | `regs` / `sample` / `selftest` / `raw` / `inject` 五个节点 | `raw` 可读写任意偏移，是定位「硬件为什么不转换」的关键工具 |

**省电语义**是 L3 的核心：probe 之后 ADC **不转换**，只有用户态 `open`
`/dev/input/eventN` 才在 `input->open` 回调里 `wn_lradc_enable()`，最后一个 fd
关闭时在 `input->close` 里 `wn_lradc_disable()`。这是键盘类驱动的标准做法。

### 本板实测：L3 通过，ADC 不转换

**已验证通过的部分：**

- probe 全链路：解析出 5 个键、映射寄存器、取得 26 MHz 时钟、复位 deassert、
  `irq 428 registered`
- `/proc/bus/input/devices` 出现 `weaknet-lradc-keys`，`Handlers=kbd event11`，
  `EV=3`（EV_SYN+EV_KEY），KEY 位图精确对应 5 个键码
- `/dev/input/event11` 节点生成
- **端到端注入验证**：往 debugfs `inject` 写 raw 值，evdev 收到正确事件序列

```text
echo 19   > inject   →  EV_KEY code=114 value=1  + EV_SYN   (KEY_VOLUMEDOWN)
echo 41   > inject   →  EV_KEY code=114 value=0  + EV_SYN
                        EV_KEY code=172 value=1  + EV_SYN   (KEY_HOMEPAGE)
echo release > inject →  EV_KEY code=172 value=0  + EV_SYN
echo 0    > inject   →  EV_KEY code=115 value=1  + EV_SYN   (最近邻匹配到 210mV)
echo 200  > inject   →  拒绝，out of range 0-63
```

- `input->open` 回调生效：probe 后 `CTRL=0x02000141` 的 `ENABLE=1` 只可能由
  open 回调写入（probe 代码本身不调 `wn_lradc_enable()`）

**未能验证的部分，以及原因：**

本板 LRADC 的**模拟转换从未启动**。`CTRL` 可精确回读、位布局与 mainline sun4i
逐位相同（写全 1 回读 `0xffcf7fff`）、`INTC` 回读 `0x00001f1f`、时钟 26 MHz、
复位已 deassert——但 `INTS` 恒 `0`、`DATA0` 恒 `0x3f`、IRQ 计数恒 `0`。
横扫 `CHAN_SELECT×4`、`SAMPLE_RATE×4`、`KEY_MODE_SEL×LEVELB_VOL` 全 16 组合、
`CONTINUE_TIME_SEL`、`FIRST_CONVERT_DLY`、`LEVELA_B_CNT`、纯 `ENABLE=1`、
vendor 复位值 `0x01000168` + `INTC=0x1f1f`，全部零响应；已排除 gpadc 争用
（unbind `sunxi-gpadc` 后无变化）。

因此 `selftest` 会打出 `selftest FAILED: ISR path is dead`——这是**正确结论**，
不是 bug。`inject` 节点的存在就是为了在这种情况下仍能端到端证明 L3 链路正确。

`input->close` 也未能观察到：本设备被标成 `Handlers=kbd`，X server 会常驻
持有 `event11`（实测 PID 546 持有 fd 34），open 计数永不为零，内核自然不调
close。这是真实桌面环境的既定行为，不是驱动缺陷。

**诚实结论：本驱动在 A733 上不能作为可用的按键输入设备。** 它的价值是完整的
驱动链路实现与诊断设施（`raw` / `inject` / `selftest`），以及换到 mainline
兼容 SoC 时的适配基座。

### 最终判定（2026-11-06 补充）

后续在核对「驱动面引入」时，拿到了两条互补的实测证据，足以把结论从
「ADC 不转换（原因待查）」升级为「本板 LRADC IP 不可用」：

| 证据 | 观测 |
| --- | --- |
| LRADC（`2524000.lradc`） | 驱动侧一切正常（probe 通过、寄存器可回读、时钟 26 MHz、复位已 deassert、IRQ 428 已注册），但硬件侧 `INTS` 恒 0、`DATA0` 恒 `0x3f`、IRQ 计数恒 0 |
| GPADC（`2521000.gpadc`） | `iio:device0` 正常工作，`in_voltage0..3_raw` 实测有稳定读数（1797 / 608 / 1567 / 1311） |

同一块 A733 上，同属 ADC 类的两个 IP：一个驱动全对但硬件不响应，另一个
驱动与硬件都正常。这排除了「我们驱动写错了」的可能性。

结合本文件前面「根因定位」一节已有的三条证据（`# CONFIG_AW_LRADC is not set`、
`sunxi-gpadc ... warn: keyadc not select`、DT 属性对比显示模拟前端配置全在 gpadc 上），
可以给出定论：**A733 上 LRADC 的按键功能已被 gpadc 取代**。`DATA0 = 0x3f`
不是「转换失败」，而是「模拟输入未连接，通道浮到 VREF 满量程」。缺的是 BSP
层面的模拟前端配置，不是驱动代码。

**因此本模块的处置是：保留、不接生产、不改写成 GPADC 驱动。**

不改写为 GPADC 的具体理由：

1. **通道已被占满**——`sunxi-gpadc` 已绑定 `iio:device0`，DT 里
   `channel_select = 0xff`（8 通道全开），没有空闲通道；
2. **要与在树驱动仲裁**——想接管就得 unbind `sunxi-gpadc` 或用裸 MMIO 强占。
   注意：实测板上**没有任何进程持有** `/dev/iio:device0`，仓库内也没有 iio
   消费者，所以「破坏下游」并不成立；真正的成本是与在树驱动打架；
3. **业务上无需求**——见下方「业务价值判定」。

### 业务价值判定

按键输入对本项目**零业务价值**。产品定位是工业无线诊断网关
（`docs/产品定位-工业无线诊断网关.md:39`：「不控制、不管理，只做观测与诊断」），
部署形态是无人值守的机柜侧设备：

- 没有用户会站在机柜前按设备上的键；
- 现场误触会污染事件流；
- 远程操作走 `weaknet-cli` → D-Bus，不需要本地按键。

所以本模块**不应被「修好」**。它的价值是驱动技术栈纵深 + 一套可复用的
硬件诊断方法学（`raw` 任意偏移读写 / `inject` 不依赖硬件验证上报链路 /
`selftest` 自证 ISR 是否活着），以及这个诚实的负面结论本身。

> 完整方案见仓库根目录 `驱动面能力引入与业务融合方案.md` 第 13 节。

## 九、来源与许可

本目录代码为本项目重写，遵循 GNU General Public License v2.0（见各文件
`SPDX-License-Identifier: GPL-2.0`）——内核模块必须与内核许可兼容。

设计参照：

- `monitor_system`（MIT，`cpp-agan-team/monitor_system`）
  `worker/src/kmod/cpu_stat_collector.c`、`worker/src/kmod/softirq_collector.c`
- Linux 内核 `kernel/sched/cputime.c`、`kernel/softirq.c`、`kernel/time/tick-sched.c`

背景与完整讨论见仓库根的 `内核模块技术栈引入讨论.md`。

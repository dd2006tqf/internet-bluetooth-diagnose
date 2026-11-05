# kmod —— 实验性内核模块技术栈（B1 路径）

本目录是 `内核模块技术栈引入讨论.md` 中**方向二 / 路径 B1** 的落地实现。
目的是把项目的观测能力从用户态/eBPF 延伸到内核态，验证这条技术路线是否值得
继续投入。

> **定位：实验代码，不进生产链路。**
> 不进 `tools/ci.sh`、不进 OTA manifest、不注册 `IMonitorPlugin`、
> 不新增 D-Bus 方法、不进 `config.yaml`。全部使用方式就是板端手工
> `make` → `insmod` → 跑 `tools/wn_kmod_read` → `rmmod`。
>
> 换句话说：**当前项目的主构建、部署流程、发布产物都不会因为本目录存在而改变。**
> 如果有人发现某个脚本引用了本目录，那是个 bug。

---

## 一、两个模块

| 模块 | 源文件 | 作用 | 设备节点 |
| --- | --- | --- | --- |
| `weaknet_cpu_stat` | `weaknet_cpu_stat.c` | per-CPU 累计时间（user/nice/system/idle/iowait/irq/softirq/steal/guest/guest_nice） | `/dev/weaknet_cpu_stat` |
| `weaknet_softirq` | `weaknet_softirq.c` | per-CPU 十类软中断累计触发次数 | `/dev/weaknet_softirq` |

两者同构：`hrtimer` 1 Hz 采样 → 写入内核私有缓冲区 → seqlock 保护下
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

补充一处源实现没有、但这里必须加的：**映射存活期间禁止卸载**。
`mmap` 里 `try_module_get(THIS_MODULE)`，`vm_ops->close` 里 `module_put()`。
否则用户态还持有映射时 `rmmod` 成功，`vm_ops` 回调会跳进已释放的代码段。

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
| 卸载安全性 | 映射存活时 `rmmod` 被拒绝（`Module weaknet_cpu_stat is in use`，refcount=3）；进程退出后 rc=0 |
| 撕裂检测 | 30 s 内 666,726 次读取（约 22k 次/秒），覆盖约 60 个写入窗口，**0 次撕裂、0 次重试耗尽** |

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

## 六、没有做的事（边界）

- **没有**接入生产链路（`IMonitorPlugin` / D-Bus / `config.yaml` / SQLite）。
- **没有**做热插拔 CPU 的动态扩容验证（只在 8 核静态环境测过）。
- **没有**在 `CONFIG_VIRT_CPU_ACCOUNTING_GEN=y` 的内核上验证
  （本板未开，`kcpustat_cpu_fetch` 走的是内联 `*dst = kcpustat_cpu(cpu)` 分支）。
- **没有**做长期稳定性测试（>1 小时）与内存泄漏检测。
- **没有**做跨内核版本兼容性矩阵，只验证了 5.15.147 与编译期的 6.8。

## 七、来源与许可

本目录代码为本项目重写，遵循 GNU General Public License v2.0（见各文件
`SPDX-License-Identifier: GPL-2.0`）——内核模块必须与内核许可兼容。

设计参照：

- `monitor_system`（MIT，`cpp-agan-team/monitor_system`）
  `worker/src/kmod/cpu_stat_collector.c`、`worker/src/kmod/softirq_collector.c`
- Linux 内核 `kernel/sched/cputime.c`、`kernel/softirq.c`、`kernel/time/tick-sched.c`

背景与完整讨论见仓库根的 `内核模块技术栈引入讨论.md`。

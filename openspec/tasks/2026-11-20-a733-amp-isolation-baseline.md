# 路线二 A55/A76 隔离 AMP：硬件事实与延迟基线（未实施）

> 日期：2026-11-20
> 分类：可行性实测（只读 + 可逆运行时调参，已全部回滚）
> 结论：**隔离方案硬件前提成立，但纯 IRQ 迁移无收益；真正的收益必须靠 `isolcpus` + cpuset**

## 构思

**要回答什么**：路线二（把若干核从 Linux 调度器里摘出去跑裸机/RTOS）在这块板子上能不能做？做了能拿到多少延迟收益？

**背景**：E902 路线已被判定不可行（见 `2026-11-19-a733-e902-bsp-support-verdict.md`），
路线二成为唯一自主可达路径。但"能做"需要硬件事实支撑，不能只凭架构常识。

**为什么走 `openspec/tasks/`**：只读取证 + 运行时可逆调参，不改产品代码，零 footprint。

**取舍**：

| 手段 | 取舍 |
| --- | --- |
| `cyclictest`（rt-tests 1.5，已预装） | ✅ 标准工具，比手写更可信 |
| 内核 cmdline / sysfs topology | ✅ 决定能怎么隔离 |
| `/proc/interrupts` 迁移实验 | ✅ 可逆，且能直接量化"中断是不是抖动源" |
| 改 bootloader/cmdline | ❌ **本次不做**。需重启，且 cmdline 在 U-Boot 环境里 |
| 裸机核间通信验证 | ❌ 本次不做。先证明隔离有意义，再投入 |

## 实现方式

### 一、CPU 拓扑（关键，且此前记错过）

```
cpu0-5  MIDR=0x412fd050  part=0xd05  Cortex-A55 r2p0  capacity=385   max 1794 MHz
cpu6-7  MIDR=0x414fd0b1  part=0xd0b  Cortex-A76 r1p0  capacity=1024  max 1950 MHz
```

**不是 8×A55。** 此前我根据 `lscpu` 的 `Model name: Cortex-A55` 一度判成
"8 个 A55"，那是错的——`lscpu` 只显示 cpu0 的型号。用 MIDR 逐个读才看清。

两个独立 cpufreq policy：

```
/sys/devices/system/cpu/cpufreq/policy0  → cpu0-5  (max 1794000)
/sys/devices/system/cpu/cpufreq/policy6  → cpu6-7  (max 1950000)
governor = ondemand，available = ondemand performance schedutil
```

全部 8 核 online，负载均衡（`/proc/stat` 差分 103-106 jiffies/核）。

### 二、基线延迟（未做任何隔离）

```
cyclictest -p 80 -t 1 -a <cpu> -i 1000 -d 0 -D 30s -q

A76 (cpu6)  Min 3  Act 10  Avg 10  Max 612  (us)
A55 (cpu0)  Min 7  Act 27  Avg 27  Max 618  (us)
```

`Avg` 是个位数到二十几微秒，但 **`Max` 到 ~600us**。这个尾部才是隔离要解决的问题。

### 三、中断迁移实验（可逆，已回滚）

**假设**：Max 600us 主要来自中断打在隔离核上。

**操作**：把 49 个可迁移中断的 `smp_affinity` 从 `ff` 改成 `3f`（只留 cpu0-5），
5 个不可迁移的中断（irq 11/14/468/478/479）保持原值。

**结果——假设被否定**：

```
A76 (cpu6，中断已迁走)  Min 1  Avg 9  Max 636
A55 (cpu0，对照)         Min 5  Avg 27 Max 992
```

`Avg` 从 10 → 9（噪声级），**`Max` 从 612 → 636，没有改善**。

**⇒ 抖动不来自中断，来自内核调度器本身。**
只要还有普通任务（包括 kernel 线程、migration、ksoftirqd）能被调度到该核上，
尾部延迟就不会因为迁走中断而改善。

**这直接决定了方案设计**：单纯调 IRQ affinity 是无效的，必须：
1. `isolcpus=` 把核从调度器里摘出去（减少 CFS 负载均衡把它们当目标）；
2. `nohz_full=` 减少 tick 干扰；
3. `rcu_nocbs=` 把 RCU 回调移走；
4. cpuset 把用户态任务强制限制在非隔离核；
5. 内核线程（`kthreadd` 派生的 per-cpu 线程）需要 `taskset` 或 cpuset 排除。

### 四、状态回滚确认

实验后已把全部 54 个中断的 `smp_affinity` 恢复到 `ff`，并清理 `/tmp` 临时文件。
校验：`ff 的: 54   其它: 0`。

> **过程中的一处操作失误（记录）**：第一次备份 affinity 用了
> `sudo cat "$f" > /tmp/backup/$i`——重定向由**非 root 的 shell** 执行，
> 54 个全部 `Permission denied`，备份为空。此时我已经改完了 affinity，
> 原始值事实上已经丢失。
>
> 之所以还能完整恢复：Linux 默认 `smp_affinity` 就是 `ff`，
> 而被跳过的 5 个不可迁移中断本来就保持 `ff`，所以"恢复到 ff"是安全的。
> **但这是运气，不是流程。** 正确写法是
> `sudo sh -c 'cat "$f" > /tmp/backup/$i'`（整条命令在 root shell 里）。
> 更稳的是先备份、**校验备份非空**，再动手改。

## 结果

### 已确认的硬件前提

| 项 | 结论 | 用途 |
| --- | --- | --- |
| 8 核异构（6×A55 + 2×A76） | ✅ MIDR 确认 | A76 适合跑确定性任务（capacity 1024，1.95GHz） |
| 两个独立 cpufreq policy | ✅ | 可单独给隔离核设 `performance` governor |
| 全部核 online 且负载均衡 | ✅ | 隔离是"可选项"，不是"已经在做" |
| 可迁移中断 49 / 不可迁移 5 | ✅ | 隔离后剩 5 个中断可能仍打到隔离核，需在方案里处理 |
| `reserved-memory` 仅 `bl31` 16MiB | ✅ | 共享内存**必须**通过 CMA 或 cmdline 新增，不能复用现成块 |
| cgroup 版本 = **v2**（`cgroup2fs`） | ✅ | 用 cpuset 控制器需走 v2 接口，不是 v1 的 `cpuset/cpus` |

### 已否定的假设

**"迁走中断就能把 Max 从 600us 降下来" —— 错。** 实测 612 → 636，无改善。
抖动源是调度器，不是中断。

### 未做（需重启或需评审）

1. **改 cmdline 加 `isolcpus=6,7 nohz_full=6,7 rcu_nocbs=6,7`** —— 需重启，
   且要改 U-Boot 环境变量或 `/boot` 下的 cmdline 文件。**未做，需用户授权。**
2. 在隔离核上验证裸机代码能否被唤醒（`sev` / spin-table / PSCI）。
3. CMA / reserved-memory 共享内存块的划法与 mmap 导出。
4. msgbox 作为门铃在隔离核上的中断路由（`RD_IRQ_EN` 由我们自己开）。

### 建议的下一步顺序

```
1. 先只加 isolcpus（不加 nohz_full/rcu_nocbs），重启后重测 cyclictest
   → 量化"摘出调度器"单独的收益
2. 再加 nohz_full + rcu_nocbs，重测
   → 量化 tick/RCU 的额外收益
3. 用 cpuset 把所有用户态任务限制到 cpu0-5，重测
   → 量化"没人打扰"的收益
4. 三步都做完后如果 Max 仍 >100us，就要考虑 PREEMPT_RT 或直接上 Jailhouse
```

**每一步都必须先测再进下一步**，否则无法归因收益来自哪一项。

### 边界

- 本次只测了 30s × 30000 次，未做长时间（小时级）稳定性测试。
  `Max` 这类尾部指标需要长跑才稳定。
- `ondemand` governor 本身会引入频率切换抖动；隔离核应设 `performance`。
  本次未改 governor。
- 未验证 A76 的 `cpu6-7` 是否支持热插拔（`/sys/devices/system/cpu/cpu6/online`
  存在不代表可 offline，需实测写入）。
- 路线二**丢掉** E902 的"A 核复位后仍存活"特性。若将来确实需要这个，
  只能回到 E902 路线，而那条路已被判定需要原厂文档。

## 关联

- `openspec/tasks/2026-11-19-a733-e902-bsp-support-verdict.md` — 路线一被否定的完整证据
- `openspec/tasks/2026-11-18-a733-msgbox-path-verification.md` — msgbox 通路实测
- `docs/ideas/A733跨核rpmsg通信-可行性与路线.md` — 原始设想与 2026-11-19 修正

# 内核模块技术栈 B1 实验（cpu_stat + softirq）

> 日期：2026-11-05
> 分类：工作流外实施 / spike
> 结果：已完成（真机全链路验证通过）

## 构思

### 要解决什么

`内核模块技术栈引入讨论.md`（2026-11-02）提出三个方向，其中方向二「引入驱动模块
技术栈」长期悬而未决。11-03 实测解除了它的两个硬前置（板端 `linux-headers` 完整
可用、`CONFIG_MODULES=y`/`CONFIG_MODULE_UNLOAD=y`），于是它从「待验证」变成
「随时可开工」，优先级被上调到第 1 位。

本次要回答的问题是：**这条技术路线在本项目里到底能不能落地**——不是纸面可行，
而是真的在目标板上编译、加载、采集出正确数据、安全卸载。

### 为什么不做进 OpenSpec change

B1 路径的定位就是「实验性引入，不进生产链路」：不进 `tools/ci.sh`、不进 OTA
manifest、不注册 `IMonitorPlugin`、不新增 D-Bus 方法、不进 `config.yaml`。
它没有生产消费者，也没有可验收的表面。

按 CLAUDE.md「规划与实现分离」一节：没有真实消费者与可观测命令的表面在 OpenSpec
里就是 orphan，强行套 change 流程会因 footprint 判定与 fingerprint 反复变化而报废。
因此走工作流外任务记录，结论回写讨论稿。

### 备选方案与取舍

| 方案 | 做法 | 取舍 |
| --- | --- | --- |
| B1（选定） | 板端手工 `make` → `insmod` → 跑测试程序 → `rmmod` | 零构建污染、零部署风险；代价是不能进 CI |
| B2 | 生产级引入，纳入 CI / OTA | 需要独立 change 定义 L5 执行面契约，成本远超本次验证目的 |
| B3 | 纯 procfs 用户态插件 | 零风险但拿不到内核态能力，与本次诉求不匹配（可并行做） |

源码位置在讨论稿 §8.2 原写作 `server/kmod/`，本次定为**仓库根 `kmod/`**：
`server/CMakeLists.txt` 用显式清单管理源文件，把内核模块放进 `server/` 树下会让
「这块代码不参与主构建」这件事不显然。

引用 vs 重写：**选重写**。需要修的点（D1 撕裂读、D2 cache 属性、D3′ idle 精度、
D4 CPU 上限、D5 离线标记、D6 capability、映射存活期卸载）已覆盖采集主链路，
逐处打补丁的 diff 比按目标结构重写更难审查；重写还能把 ABI 抽成独立头文件让内核
与用户态共享同一份结构定义。源项目仅作规格参照，MIT 条款在 `kmod/README.md` 保留。

## 实现方式

### 新增文件

| 文件 | 行数 | 作用 |
| --- | --- | --- |
| `kmod/weaknet_kmod_abi.h` | 97 | 内核与用户态共享的 ABI 结构 |
| `kmod/weaknet_cpu_stat.c` | 305 | per-CPU 累计时间采集 |
| `kmod/weaknet_softirq.c` | 247 | per-CPU 十类软中断计数采集 |
| `kmod/Makefile` | 35 | 三种用法：板端就地 / 宿主对拍 / 交叉编译 |
| `kmod/tools/wn_kmod_read.c` | 422 | 用户态读取工具 |
| `kmod/README.md` | 157 | 实验边界、改造点、实测结果、未做项 |

### 关键决策

1. **seqlock 快照协议**：内核写侧 `seq+1 → wmb → memcpy → wmb → seq+2`；
   用户态 `校验偶数 → memcpy → 再校验`。顺序不能反——先校验后拷贝，两次操作之间
   内核可能又写一轮。这一点在实现中自查发现并修正过一次。
2. **`vmalloc_user` + `remap_vmalloc_range`** 替代源实现的
   `__get_free_pages` + `remap_pfn_range` + `pgprot_noncached`。一致性靠内存屏障
   不靠 cache 属性；把普通 RAM 设成非缓存只会拖慢每次读。
3. **`try_module_get()` + `vm_ops->close = module_put()`**：防止映射存活期间卸载，
   否则 `vm_ops` 回调会跳进已释放的代码段。这是源实现没有的。
4. **ABI 头用 `WN_U64`/`WN_U32` 宏**而非 typedef：用户态包含 `<sys/stat.h>` 会间接
   引入 `asm-generic/int-ll64.h` 的 `__u64`，typedef 方案必然冲突。

### 验证手段

全部在真机 `radxa-cubie-a7a`（`Linux 5.15.147-99btf1-a733` aarch64，8 核）上完成：

1. 板端 `make` 编译，`file` 确认产出 aarch64 ELF，`strings` 确认
   `vermagic=5.15.147-99btf1-a733 SMP preempt mod_unload aarch64` 与 `uname -r` 逐字匹配。
2. `insmod` 两个模块，检查 `dmesg`、`/dev/` 节点、`/proc/modules`。
3. 与 `/proc/stat`、`/proc/softirqs` 逐字段对拍。
4. 裸读 `mmap` 验证字段量纲（纳秒/微秒）与量化粒度。
5. `capsh --drop=cap_sys_admin` 对照组验证 capability 门禁。
6. 映射存活时 `rmmod` 验证卸载保护。
7. 30 秒 66.7 万次高频读取验证 seqlock 无撕裂。

## 结果

### 通过项

- 板端编译、加载、采集、卸载全链路通过，vermagic 逐字匹配
- 与 `/proc/stat` 逐字段一致（CPU0 `user` 23.13 s vs 2313 tick @USER_HZ=100）
- 与 `/proc/softirqs` 十类计数逐 CPU 全部一致
- capability 门禁生效：同一 root 身份仅摘 `CAP_SYS_ADMIN` 即 `EPERM(1)`
- 卸载保护生效：映射存活时 `rmmod` 报 `Module is in use`，进程退出后 rc=0
- seqlock 无撕裂：30 s / 666,726 次读取 / 约 60 个写入窗口，0 次撕裂、0 次重试耗尽
- 设备节点随模块卸载自动清理

### 提交后发现并修复的漏洞：`PROT_WRITE` 映射未被拒绝

首次提交（`c517137`）之后、回答「差什么没做」时实测发现的，属于安全缺陷。

**问题**：`wn_mmap()` 只检查了 `vm_pgoff`，没有检查 VMA 的写标志。
`remap_vmalloc_range` **不**替你拒绝 `PROT_WRITE`，于是 root 用
`PROT_READ|PROT_WRITE` 映射成功，直接把 `hdr.sample_ns` 从 `1374934036214`
改成 `18446744073709551615`，写入生效。

**为什么严重**：不只是「能改坏数据」。seqlock 的前提是「一个写者、多个读者」，
用户态一旦可写就成了第二个写者——改完 `seq` 后读者照常校验通过并采信，
**一致性保证被彻底架空**。所以这不是加固，是协议成立的必要条件。

**修法**：两个模块的 `wn_mmap()` 开头加 `if (vma->vm_flags & VM_WRITE) return -EACCES;`，
位置在 `try_module_get` **之前**（被拒绝的映射不该抬高模块引用计数）。

**复验（真机）**：
- 两个模块 × `O_RDWR`/`O_RDONLY` × `PROT_WRITE` 共 4 组，全部 `EACCES(13)`
- `PROT_READ` 读取正常，`seq`/`abi`/`nr_cpus`/`sample_ns` 均有效
- seqlock 回归：15 s / 390,112 次读取（26,007 次/秒），0 次违规、0 次重试耗尽

`kmod/README.md` 的验证清单与「改造点」一节已同步补充此条。

### 两条被实测推翻的判断（已回写讨论稿）

**1. 讨论稿 D3「`user` 与 `guest` 重复计入」不成立**

核实源项目 `worker/src/monitor/cpu_stat_monitor.cpp:31-41`：
`new_cpu_busy_time = user + system + nice + irq + softirq + steal`，**没有 guest**；
total 同样没有。原判断把「模块导出了全部 10 项」误推成「用户态把它们全加进了 busy」。
已在讨论稿 §5 加勘误块，§11 第 4 条同步改写。

**2. 「输出纳秒消除 HZ 量化误差」这个说法对 busy 类不成立**

本板 `CONFIG_HZ=250`、`CONFIG_TICK_CPU_ACCOUNTING=y`，`kcpustat` 的 busy 类字段
本身就按 `TICK_NSEC = 1e9/250 = 4 ms` 累加。源实现的 `nsec_to_jiffies()` 除以 4e6
对它们是**无损**的。实测确认 8 个 CPU × 8 个 busy 类字段全部是 4,000,000 ns 整数倍，
无一例外。

真正的精度收益只在 `idle`/`iowait`：它们来自 `get_cpu_idle_time_us()` 的微秒值，
源实现接着用 `usecs_to_jiffies()` 又压回 4 ms。此项在讨论稿中新增为 **D3′**。

### 遗留问题

- 只验证了 8 核静态环境，**未测**热插拔 CPU 的动态扩容
- 只在 `CONFIG_VIRT_CPU_ACCOUNTING_GEN` 未开的内核上验证（本板未开）
- **未做**长期稳定性测试（>1 小时）与内存泄漏检测
- **未做**跨内核版本兼容性矩阵，只覆盖 5.15.147 与编译期 6.8
- 板端无 `modinfo`/`depmod`（未安装 `kmod` 包），vermagic 改用 `strings` 从 `.ko` 读取
- 板端 `setpriv` 的 `setgroups` 被容器策略拒绝，capability 对照改用 `capsh`
  （注意 `capsh` 只在 root 的 PATH 里，`command -v` 查不到但 `sudo capsh` 可用）

### 是否升格为 OpenSpec change

**暂不。** 本次目的是验证技术路线可行性，目的已达成。若要推进到 B2（生产级），
需要先由 Planner 定义：谁消费这份数据（新 `IMonitorPlugin`？新 D-Bus 方法？）、
数据如何进 SQLite、OTA 如何管理模块版本、卸载/升级的崩溃点如何判定。这些都是
L5 执行面契约问题，必须走完整工作流。

值得注意的一个真实盲区（讨论稿 §8.2 B3 备注）：当前项目**完全不采集主机资源**，
`assurance/` 下 25 个头文件没有一个看 CPU/内存；`docs/网络体验评价体系.md:29`
把「✓ Observer healthy」列进证据样例，但 Observer 健康只看 eBPF 的
`capture_emit_failure_ratio`，**不看采集端自身压力**。本次的内核模块在技术上
已经能填这个坑（4 ms 粒度的 CPU/软中断），但要不要填、用什么形态填，是产品决策。

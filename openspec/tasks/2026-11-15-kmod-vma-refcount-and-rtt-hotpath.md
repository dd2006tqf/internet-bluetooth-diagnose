# kmod vma 引用计数下溢修复 + TcpRttMonitor::getStats() 热路径削减

- 日期：2026-11-15
- 类型：工作流外实施（未创建 OpenSpec change）
- 触发：用户指令「完整的修复这些问题」
- 结果：**kmod 修复已在真机验证闭合**（计数可逆、rmmod 成功）；
  **getStats() 优化算法层面 6.58x 但绝对收益仅约 1 ms/次**，
  真机无法测出（测量手段本身的启动开销 47 ms 远大于收益），
  如实记录为「小收益、低风险、不作为性能结论」。

---

## 一、kmod vma 引用计数下溢

### 构思

两个实验模块（`weaknet_cpu_stat` / `weaknet_softirq`）都是内核 hrtimer 采集 +
用户态只读 mmap 的形态。它们用一个共享快照页对外发布数据，因此涉及
`vm_operations_struct` 的生命周期管理。

原实现只在 `mmap` 回调里 `try_module_get(THIS_MODULE)` 一次，并只注册
`.close` 回调做 `module_put`。看上去是配对的，实际不是。

**内核的 vma 复制语义**：无论 `fork()` 继承映射，还是 `munmap()` 把一段
vma 拆成多段，内核都会为**每个副本**调用一次 `vm_ops->open`，而关闭时对
每个副本各调一次 `close`。只实现 `close` 的后果是 put 次数多于 get 次数。

引用计数是 `unsigned int`（`struct module.refcnt`），减到 0 之后继续减
就是下溢成 `UINT_MAX`：

```
实测（2026-11-15 板端，修复前）：
  /sys/module/weaknet_softirq/refcnt   → -1
  lsmod 第三列                          → -1
  rmmod weaknet_softirq                → rc=1（永久无法卸载）
```

更隐蔽的次生破坏：`module_put()` 在计数已为 0 时会命中
`kernel/module.c` 的 `WARN_ON`，内核被 taint 成 `G O`。任何进程
`execve()` 都会走 `exit_mmap → wn_vma_close → module_put`，所以这个
警告会反复出现，把内核日志刷满。

### 实现方式

修法是补上 `.open` 回调，并让 `mmap` 在挂上 `vm_ops` 之后显式调用一次：

```c
static void wn_vma_open(struct vm_area_struct *vma)
{
    if (!try_module_get(THIS_MODULE))
        pr_err("%s: vma_open 无法取得模块引用\n", DEVICE_NAME);
}

static void wn_vma_close(struct vm_area_struct *vma)
{
    module_put(THIS_MODULE);
}

static const struct vm_operations_struct wn_vm_ops = {
    .open  = wn_vma_open,
    .close = wn_vma_close,
};
```

`wn_mmap()` 里**删掉**原先内联的 get/put 对，改为：

```c
    ret = remap_vmalloc_range(vma, g_shared, 0);
    if (ret)
        return ret;

    vma->vm_ops = &wn_vm_ops;
    wn_vma_open(vma);   /* 首次映射不会自动触发 open，必须手工调一次 */
    return 0;
```

三个为什么：

1. **为什么 `mmap` 里可以不做 get**：`file_operations.owner` 已经保证
   `->mmap` 执行期间模块常驻（内核在调用前就持有 file 的模块引用）。
2. **为什么必须手工调一次 `open`**：内核只在驱动**设置 `vm_ops` 之后、
   vma 被复制时**才调用它。首次映射不复制 vma，不手工调用就少一次 get，
   而 close 照常 put —— 又回到下溢。
3. **为什么 `open` 不返回错误**：`vm_ops->open` 签名返回 `void`，拿不到
   引用也无法向上传递。但 `.owner` 已保证 mmap 期间常驻，这条分支只在
   极端竞态下可达，`pr_err` 已是能做到的最强反馈。

两个模块同构修改，`weaknet_softirq.c` 承载完整长注释，
`weaknet_cpu_stat.c` 用短注释指向它。

### 结果

编译：`tools/kmod_cross_build.sh` 交叉编译通过，4 个模块
（含 `weaknet_wdt` / `hid-multitouch`）vermagic 校验全过。

**真机验证**：先 `rmmod` 旧版（基线计数为 0，所以这一步本来就会成功，
不构成证据），再 `insmod` 新版，然后跑探针脚本
`/tmp/vma_refcnt_probe.py` —— 它刻意构造了修复前的崩溃序列
（mmap → `fork()` → 子进程退出 → `munmap` → 重复 3 轮）：

```
== weaknet_cpu_stat ==
  0. 加载后基线                 refcnt = 0
  1. 首次 mmap 之后             refcnt = 3
  2. fork 子进程退出之后         refcnt = 3
  3. munmap + close 之后        refcnt = 0
  4. 额外 3 轮 mmap+fork 之后    refcnt = 0
PASS: 计数回到 0，open/close 成对
```

`weaknet_softirq` 同样 PASS。

**计数构成已逐项核对**（同进程隔离实验）：

| 操作 | 计数变化 | 说明 |
| --- | --- | --- |
| `open()` | +2 | `cdev.owner` 与 `fops.owner` 各一份 |
| `mmap()` | +1 | 本次补上的那一次 get |
| `munmap()` | −1 | |
| `close(fd)` | −2 | |

**跨进程复验**（排除同进程测量的时序假象）：

```
基线（仅服务持有）        = 2
额外进程仅 open fd        → 4（+2），退出后 → 2
额外进程 open+mmap        → 5（+3），退出后 → 2
```

**决定性一步**：跑完探针后 `rmmod` 两个模块，`rc=0`，`/sys/module/<name>`
消失。修复前这一步是 `rc=1` 且计数为 -1。这就是修复闭合的判据。

**生产并行观察**：服务（`host_pressure_monitor` 的两条采样器）持有
`/dev/weaknet_cpu_stat` 与 `/dev/weaknet_softirq` 各一份 fd + mmap，
稳态计数为 2（fd 引出的 2 份，mmap 那份因 `vm_ops` 语义已含在其中），
持续 60 秒采样无漂移。`tainted = 4096`（`TAINT_OOT_MODULE`，
out-of-tree 模块的正常标记），不再是修复前的 `G O`。

### 一次真实的操作事故（必须记录）

验证过程中我执行 `sudo systemctl restart weaknet-server`，
**板子在 16 秒后整机复位**。原因不是 bug：

`weaknet-health-guard` 的健康判据包含「D-Bus 采集推进」。服务重启的
窗口期内该判据必然失败，而守卫的判定周期是 2 秒、硬件超时 16 秒，
于是它停止喂狗 → 硬件复位。守卫的失败证据文件写得很清楚：

```json
{"reason": "service_absent",
 "checks": {"sample_ns": {"ok": false, "detail": "busctl rc=256"}},
 "action": "stopped_petting", "reset_expected_in_s": 16}
```

**正确做法**（记忆化，下次必用）：
动 `weaknet-server` 之前先建维护标记，守卫看到它就无条件继续喂狗：

```bash
ssh board 'sudo touch /var/lib/weaknet/health-guard-maintenance'
# ... 做维护 ...
ssh board 'sudo rm /var/lib/weaknet/health-guard-maintenance'
```

我是先停服务才想起这条，代价是一次整机复位。后续部署
`./tools/ci.sh` 时已经先建了标记，未再复发。

---

## 二、TcpRttMonitor::getStats() 热路径

### 构思

板端实测该函数的调用频率比我最初判断的高：

| 调用方 | 周期 | 落点 |
| --- | --- | --- |
| `start_tcp_rtt_monitor_thread` | `cfg.tcp_rtt.interval_ms`（10 s） | `server.cpp:1337` |
| `start_network_quality_thread` SLE | 15 s | `server.cpp:494` |

即**平均约每 6 秒一次**。而它与 `isAvailable()` / `health()` / `metrics()`
共用同一把 `impl_->mutex`，持锁期间这些全部被阻塞。

原实现每次调用做三件昂贵的事：

1. 对 map 里**每一条**连接 `inet_ntop` + 构造两个 `std::string`
   （各一次堆分配）；
2. 全量 `std::sort` 一遍明细（按 `lastTimestampNs`）；
3. 全量 `std::sort` 一遍 RTT 数组算分位数。

而它的两个消费者都不需要全量明细：

| 消费者 | 实际需求 |
| --- | --- |
| `edge_wireless_uplink_exporter` | 先 `resize(10)` 再序列化 |
| D-Bus `GetTcpRttStats` | 诊断接口（CLI 用） |

`TcpRttEvaluator` **完全不读 `topConns`**，只用 `totalSamples` /
`activeConns` / `avgRttUs` / `p50` / `p90` / `p99`。所以遍历范围不能缩小
（`activeConns` 是总数、分位数要全量），但**明细物化可以只做被选中的少数**。

### 实现方式

新增 `server/include/utils/top_n_recent.hpp`：一个定长小顶堆选择器
`TopNRecent<T>`，边遍历边维护「最近 N 条」。关键设计是**把「是否值得
构造」的判断暴露给调用方**（`full()` / `oldest()`），这样昂贵的字符串
构造发生在过滤之后：

```cpp
if (top.full() && val.last_timestamp_ns <= top.oldest().lastTimestampNs) {
    continue;   /* 进不了 top-N，一次字符串都不构造 */
}
// 只有走到这里才 formatIpv4 + 组装 TcpRttConnItem
top.offer(std::move(item));
```

`getStats()` 相应改造：`summary.activeConns = conn_count`（仍是全量计数），
分位数仍用全量 `all_rtts`，`summary.topConns = top.take()`（上限 32，
降序，与原先全量排序后的前 32 条逐项一致）。

**为什么抽成独立头文件**：这段堆逻辑原先埋在 `getStats()` 里，而那个函数
依赖真实 BPF map，**零单测覆盖**。堆式选择恰恰是「写错了不报错」的那类
代码——顺序错了只是少一条明细，分位数和连接总数照样正确。

### 结果（含一次被自己推翻的结论）

**算法基准**（同机、`-O2`、N=2258、200 轮取均值）：

| 实现 | 单次耗时 |
| --- | --- |
| 旧：全量构造 + 两次全量 sort | 1.208 ms |
| 新：TopNRecent 堆式选择 | 0.184 ms |
| **加速比** | **6.58x（省 1.02 ms/次）** |

**单测**：新增 `test_top_n_recent_gtest`（8 项），覆盖「保留最新 N 条并
降序输出」「淘汰路径」「候选不足 N」「N=0」「空输入」「full/oldest
边界」「2258 条随机输入与参照实现逐项一致」「take() 可复用」。
x86 全量 **67/67 通过**（原 66 + 新增 1）。ARM64 交叉编译通过。

**RED 验证暴露了我一条错误注释**：我最初在 `offer()` 里写「顺序不能颠倒，
直接改 `front()` 再 `pop_heap` 会把刚插入的元素弹出堆外」。注入该变异后
**8 项单测全绿**——说明它在本类的调用约定下是等价的（调用方只在新元素比
`oldest()` 更新时 offer，被换下的堆顶必然最旧）。已改写注释为准确表述：
选当前写法是为了「少依赖一条外部不变量」，**不是修了一个能测出来的 bug**。
换掉比较器方向才会红 5 项，证明测试确实有牙齿。

**真机测不出收益，且我的两处归因是错的**：

- 第一处：我声称 47 ms 里大部分是原实现的开销。**错**。对照实验：
  `GetTcpRttStats` 30 次共 1351 ms，`GetVersion`（几乎空方法）30 次共
  1362 ms，两者完全相同。47 ms 是 `sudo` + `busctl` 子进程启动的开销，
  与 `getStats()` 无关。
- 第二处：我声称上行报文会「膨胀到数百 KB」。**错**。
  `edge_wireless_uplink_exporter::buildPayload` 里 `resize(10)` 发生在
  JSON 构造**之前**，所以上行报文从来就没大过。

**真实净收益**：约 1 ms × 每 6 秒一次 ≈ 0.017% 单核。可以忽略。
保留这次改动的理由是**代码质量**（明细上限明确、昂贵构造延后、逻辑可单测），
**不是**性能。这条结论必须如实写出来，否则后来人会以为这里曾经有个热点。

---

## 三、遗留

1. **getStats() 的真实热点在别处**。板端 `conns=1198` 时 p50≈115 ms、
   p90≈236 ms，而 LRU map 定标到 65536 条（`map_sizing_mode: auto`，
   物理内存 0.5%）。真正该问的是「为什么一张被动 RTT map 里会沉淀上千条
   早已关闭的连接」——LRU 只在容量压力下淘汰，没有按时间清理。这比优化
   遍历更值得做，但属于独立课题，本次未动。
2. **`kTopConnLimit = 32` 是新的对外行为边界**。若将来有消费者需要看更多
   连接，需要显式提高这个上限，而不是像以前那样「有多少给多少」。
3. **维护标记是手动前置条件**。停 `weaknet-server` 前必须先建
   `/var/lib/weaknet/health-guard-maintenance`，否则看门狗会在 16 秒内
   复位整机。这条已写入 `tools/` 相关文档与本次记录，但目前仍是人工纪律，
   没有工具强制。

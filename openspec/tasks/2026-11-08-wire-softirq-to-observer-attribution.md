# 接入 weaknet_softirq：CPU 饱和时细分成「收包主导」与「非网络负载主导」

> 日期：2026-11-08
> 分类：工作流外实施（承接 `2026-11-07-kmod-production-online.md` 与 `驱动面能力引入与业务融合方案.md` §5.3.1b）
> 结果：已完成（x86 全量 59/59 测试通过、ARM64 交叉编译 rc=0；真机 `SOFTIRQ_NET_RX_SHARE` 已跑通并复核）

## 构思

### 要解决的问题

`weaknet_cpu_stat` 上线后，`attributeObserverDegradation()` 能回答第一个问题：

> eBPF 掉证据，是网络真坏了，还是**采集端自己抢不到 CPU**？

但它回答不了第二个问题：**CPU 是被谁吃掉的**。这个区别直接决定处置方向：

| CPU 饱和的成因 | 含义 | 处置 |
| --- | --- | --- |
| NET_RX 软中断主导 | 采集端在拼命处理**被观测链路自身**的流量 | 降采样率/加过滤/加资源 |
| 非网络负载主导 | 与网络无关的本机进程把 CPU 打满 | 找出那个进程——这才是真正的「自伤」 |

前者过载与被观测对象强相关，后者是一个纯粹的本机性能问题。旧证据对二者**完全同形**，
`SELF_INFLICTED` 分支只能笼统地说「采集端过载了」。

`kmod/weaknet_softirq.c` 早在 `c992cc3` 前后就已写好，`.ko` 也一直随 OTA 包分发
（`kmod/Makefile` 里 `obj-m` 含它、`tools/kmod_cross_build.sh` 会构建它），但：

- `tools/weaknet-kmod.service` **没有** `insmod` 它；
- 用户态**没有**任何消费者。

也就是说它是一段「`.ko` 在包里、但从不加载、也从无人读」的死代码。

### 为什么不做进 OpenSpec change

用户明确指示「不用新开 change；以后只要我不让你新开，你就默认不用新开」。
按 `openspec/tasks/README.md` 的规则走工作流外实施 + 本记录。
（该偏好已写入项目 memory：`no-new-openspec-change-by-default`。）

### 一个先被否掉的方案：给 SLE 评估器加两个新 SLE

曾经考虑过把 `weaknet_cpu_stat` / `weaknet_softirq` **作为 SLE** 加进 SLE 评估器。
否决，理由三条：

1. **类别错误**：现有 13 个 SLE 描述的都是**被观测链路**的状态，而 CPU 压力描述的是
   **观测者自己**。把观测者塞进被观测对象的评分体系，等于让「板子忙」影响「链路健康度」。
2. **违反 L1/L2 边界**：`docs/架构设计-五层权责契约.md:118` 已把 `HOST_STALL`
   归到 L2（归因层），L1 只做确定性状态归约、不产出物性根因假设。
3. **成本与收益不匹配**：加一个 SLE 要动 9 个文件（枚举、评估器、6 处重载、2 个序列化器）、
   云端 schema、`DiagnosisEngine` 的 if 链，而这个字段**永远不能参与 `OverallPolicy`
   的评分与 veto**（见 `observer_attribution.hpp` 的语义边界声明）。

### 一个被放弃的实现方案：加新枚举值

更细的做法是给 `ObserverAttribution` 加两个新值，比如
`SELF_INFLICTED_NETWORK_DRIVEN` / `SELF_INFLICTED_LOCAL_LOAD`。否决：
结论本身并没有多出一个新分类，只是把 `SELF_INFLICTED` 讲得更清楚；而加枚举值要同步
序列化契约、云端 schema、全部真值表测试与 `observerAttributionToString()`。

所以最终做法是**第 3a 步：只追加 `explanation` 的一句话与一条 `softirq_net_rx_share`
证据，不动枚举值**。

### 三个必须先想清楚的技术边界

1. **NET_RX share 是计数构成比，不是时间占比。** 空载实测约 0.3%（TIMER/SCHED/RCU
   持续触发把分母撑大），线速小包洪水可达 99%，中间几乎没有分布。0.5 落在这段空白里，
   对「是不是收包把它压满的」够用；但它**不能**用来推断 CPU 时间分配。
2. **`wn_softirq_stat` 没有 `valid` 字段**（`wn_cpu_stat` 有）。软中断计数是纯单调递增，
   不存在热插拔重置，内核侧无置位需求；代价是用户态必须自己做 per-CPU 计数倒退检查。
3. **用户态 `mmap` 长度必须用自己的结构体。** 两个结构体大小不同（24600 vs 24576），
   混用不报编译错，只会让 `mmap` 返回 `EINVAL`，表现为「设备在、永远读不到数」。

## 实现方式

### 改动清单

| 文件 | 改动 |
| --- | --- |
| `tools/weaknet-kmod.service` | 增加 `rmmod`/`insmod weaknet_softirq.ko`（各带 `-` 前缀，**互不短路**）；就绪探针改为逐节点循环 |
| `server/include/host_pressure_monitor.hpp` | 新增 `HostSoftirqShare` 结构体与 `HostSoftirqShareSampler` 类 |
| `server/src/host_pressure_monitor.cpp` | `HostSoftirqShareSampler::Impl` + `sample()`（seqlock → ABI 校验 → `nr_cpus==0` 守卫 → 首轮建基线 → per-CPU 倒退跳过 → clamp）+ `publishHostMetric()` 复用；线程体内两个模块顺序采样 |
| `server/include/metrics/metric_types.hpp` | 新增 `MetricId::SOFTIRQ_NET_RX_SHARE` + `metricIdToString()` + 描述符（PERCENT/HOST/GAUGE，15000ms 新鲜度 / 30000ms 过期） |
| `server/include/assurance/observer_attribution.hpp` | 签名由 5 参扩到 8 参（后 3 个带默认值）；`ObserverAttributionResult` 新增 `softirq_net_rx_share` / `softirq_available` / `softirq_share_threshold` |
| `server/src/assurance/observer_attribution.cpp` | 第 3a 步细分 |
| `server/src/server.cpp` | 归因调用点读取 `SOFTIRQ_NET_RX_SHARE`（独立判新鲜度，不复用 CPU 的可用性）；`LOG_WARNING` 带 softirq 字段 |
| `server/test/unit/test_observer_attribution_gtest.cpp` | 新增 12 个测试（7 个归因细分 + 5 个采样器降级） |

### 关键决策

**1. 两条 insmod 都带 `-` 前缀，绝不用 `&&` 串联。**
这两个设备是两个独立证据来源。`cpu_stat` 加载失败时 `softirq` 仍能看见收包软中断的量级；
反之亦然。串联会让一个坏模块把另一个也拖死——最没道理的一种降级。

**2. 软中断数据不可用时**保持完全沉默**，`explanation` 里连 "softirq" 这个词都不出现。**
与 `UNATTRIBUTED` 分支同一纪律：没数据就说没数据，不猜。

**3. `explanation` 的前缀句必须逐字保留。**
`server.cpp:623` 把 `attr.explanation` 推进 `exp.warnings`，再经
`legacy_adapter.hpp:80`（`all_issues = exp.warnings`）流到 `toHealthCheckJson` 的
`issues`，即**用户能看到这段文本**。所以细分说明只能**追加**，不能改写已有句子。
单测里有断言守着：`r.explanation.find("Observer quality degraded while host CPU was saturated") == 0`。

**4. 第 3a 步只在 `SELF_INFLICTED` 分支内执行。**
CPU 没饱和时软中断构成比根本没有被咨询的资格——一台跑大流量的机器 NET_RX 占比天然高，
脱离 CPU 饱和去解读它必然误判。CPU 数据缺失时先落到 `UNATTRIBUTED`，同样跳过细分。

**5. 阈值 0.5，且判定为 `>=`（含等号）。**
边界值必须可判定，不留「擦边不算」的灰区。与 CPU 阈值 0.85 的 `>=` 语义保持一致。

**6. 双发布（但其第二份当前无消费者，见下方订正）。**
指标同时发到 `ctx->metrics_registry`（供 quality 线程归因读）与
`MetricsRegistry::global()`，与 `CPU_BUSY_RATIO` 的做法一致。

> **2026-11-08 订正**：本文最初写的是「`global()` 供 `database_manager.cpp`
> 持久化到 SQLite」——**这是错的**，与本文「结果」里「不进 `network_history` 表」
> 自相矛盾。实测核对三条事实：
> 1. `database_manager.cpp:540` 只调 `latestGlobal()`，读 `GLOBAL` 域，且只读
>    `TCP_RETRANS_LOSS_PCT` / `TCP_CONN_ACTIVE` / `TCP_CONN_ACCEPT_RATE`；
> 2. `network_history` 表没有 host 相关列（`CREATE TABLE` 见于
>    `database_manager.cpp:189`）；
> 3. 全仓 `"host:"` 作用域的读取方只有 `server.cpp` 的 quality 归因，无其它。
>
> 所以 `global()` 里这份 HOST 域副本**当前没有读者，不会进 SQLite**。保留双发
> 只为与既有 `CPU_BUSY_RATIO` 路径同构。要让它真正落库需新增表列 + 读取逻辑，
> 属另一件事。

### 验证手段

```bash
# x86 编译 + 全量单元测试
cmake -B build-x86 -DCMAKE_BUILD_TYPE=Debug -DBUILD_EBPF=OFF && cmake --build build-x86 -j$(nproc)
ctest --test-dir build-x86/server --output-on-failure

# ARM64 交叉编译
cmake -B build-arm64-native -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm64-cross.cmake \
      -DCMAKE_BUILD_TYPE=Debug -DBUILD_EBPF=ON
cmake --build build-arm64-native -j$(nproc)

# kmod 产物确认（走独立 Makefile，不是 CMake）
ls -1 dist-arm64/kmod/*.ko
```

## 结果

### 已验证

| 项 | 结果 |
| --- | --- |
| x86 编译 | rc=0（仅 `looper.cpp:61` 一条既有的 unused-parameter 警告） |
| `test_observer_attribution_gtest` | **29/29 通过**（原 17 + 新增 12） |
| x86 全量 `ctest` | **100% passed, 0 failed out of 59**，7.84 s |
| ARM64 原生交叉编译（`BUILD_EBPF=ON`） | rc=0（警告全是既有的 `-Wmismatched-tags` for `ServerContext`） |
| kmod 产物 | `dist-arm64/kmod/` 里 `weaknet_cpu_stat.ko` 与 `weaknet_softirq.ko` 均已存在 |

### 测试钉死的语义

- 收包主导 ⇒ 枚举值仍是 `SELF_INFLICTED`，`explanation` 前缀句逐字不变，追加说明含
  "dominated by the network receive path"，证据里 `softirq_net_rx_share` 值正确。
- 非收包主导 ⇒ 含 "NOT dominated by the network receive path" 与 "unrelated local workload"；
  互斥性由 `find("activity is dominated by") == npos` 断言（注意：不能直接用
  "dominated by the network receive path" 做反向断言，因为 "NOT dominated by..." 本身包含它）。
- 阈值可配且含等号：默认 0.5 时 0.5 判主导；自定义 0.9 时 0.5 判非主导。
- 软中断不可用 ⇒ 无 `softirq_net_rx_share` 证据，`explanation` 里 `find("softirq") == npos`。
- CPU 未饱和 ⇒ `NOT_HOST_CPU_BOUND`，即便 share 传 0.99 也不产生 softirq 证据。
- CPU 数据缺失 ⇒ `UNATTRIBUTED`，同样不产生 softirq 证据。
- 旧 5 参调用与显式 `(0.85, 0.0, false, 0.5)` 的结果逐字一致（回归守护）。
- 采样器设备缺失 ⇒ `available=false`、`reason="device_absent"`、`detail` 含 `"open: "`，
  且每轮都报缺失（不静默继承旧状态）。

### 遗留问题

1. ~~**真机 `SOFTIRQ_NET_RX_SHARE` 尚未验证。**~~ **已闭环（2026-11-08 复核）。**
   三项判据全部满足，证据如下：
   - `/dev/weaknet_softirq` 存在（`crw------- 1 root root 234, 0`）；
   - `lsmod` 有 `weaknet_softirq 16384 3`；`dmesg` 记
     `weaknet_softirq: loaded, 8 possible CPUs, map 24576 bytes`；
   - 服务端日志每 15 s 一条，30 分钟内 23 次采样，例如
     `softirq_net_rx_share=0.0100582 (net_rx=114/total=11334, 8/8 cpus sampled)`。

   过程中发现并修复了一个**本次新引入的缺陷**：就绪探针用了单个 `$`
   （`"${d##*/} ready"`），被 systemd 的 ExecStart* 变量替换先吃掉，
   日志打印成 `" ready"`——节点名丢失。已改为 `$$`
   （`"$${d##*/} ready"`），板端复测正确输出 `weaknet_cpu_stat ready` /
   `weaknet_softirq ready`。这个 bug 不会让单元失败（退出码仍 0），属
   「看起来在工作」的静默降级；对照实验与说明已写进该 unit 的注释。
   同时确认**全仓仅此一处**这种写法。
2. **0.5 这个阈值没有任何真机标定数据。** 目前只有两条定性观察（空载 ~0.3%、
   线速小包洪水 ~99%），中间段是空白。如果将来出现落在 30%~70% 区间的真实场景，
   需要重新评估阈值。
3. **`wn_softirq_stat` 缺 `valid` 字段**，与 `wn_cpu_stat` 的 ABI 不对称。
   统一它需要升 `WEAKNET_KMOD_ABI_VERSION` 并同步改两侧，本任务未做
   （详见下方「驱动面可深化方向」）。
4. **`SELF_INFLICTED` 及其细分仍只在单元测试中验证过。** 真机触发需要
   「CPU 满载 + eBPF 真丢证据 + 持续 DNS 流量」三条件同时成立，留作现场观察项。

### 驱动面可深化方向（本次未做，按优先级）

评估过但**没有**在本任务中实施的深化方向，按「收益/成本」排序：

| 优先级 | 方向 | 理由 | 成本 |
| --- | --- | --- | --- |
| 高 | **用真机流量标定 softirq 阈值** | 0.5 目前是纯推断，无现场数据支撑 | 低（只需观测） |
| 高 | **补上 `valid` 字段的 ABI 对称性** | 用户态现在只能靠计数倒退自己判，容易漏 | 中（升 ABI 版本 + 两侧同改） |
| 中 | **软中断的时间占比而非计数占比** | 计数比在空载时失真严重（TIMER/SCHED/RCU 撑大分母）；时间比更接近「CPU 花在哪」 | 高（需 `CONFIG_IRQ_TIME_ACCOUNTING`，本板未开） |
| 中 | **per-CPU 拆分而不是全局汇总** | 8 核里可能只有 1 核被收包压满，全局比会把它稀释掉 | 中（ABI 已按 per-CPU 存，只需改聚合） |
| 中 | **抓 `ksoftirqd` 积压 / NAPI backlog** | `/proc/softirqs` 只有触发次数，看不出「处理不过来」 | 中（新数据源） |
| 低 | **用 kprobe 区分小包洪水与大包** | 真正解释「为什么软中断饱和」的是包速率，不是字节速率 | 高（新探针 + 新 ABI） |
| 低 | **两个模块共享内核侧脚手架** | 现在两 .c 各写一遍 hrtimer+vmalloc_user+seqlock，重复约 200 行 | 中（但会让两个模块耦合，与「互不影响」冲突） |
| 低 | **编译期 ABI 版本对齐守护** | 现在只有运行期 `abi_mismatch` 检测 | 低（但收益也低，运行期已能拦住） |

**为什么把「共享脚手架」排那么低**：它看起来是明显的 DRY 收益，但两个模块
当前的核心优点正是**互不影响**——一个加载失败/写坏，另一个完全不受牵连。
抽公共层会引入共享状态与共同失败点，与刚在 `weaknet-kmod.service` 里
刻意避免的「一个坏模块拖死另一个」是同一类风险。

### 是否需要升格为正式 OpenSpec change

**不需要。** 这是一次典型的「既有代码 + 补齐消费者」的接线工作：
内核侧零改动，用户态复用既有的 seqlock 读者与双发布模式，归因层只加一个分支。
新引入的对外契约面为零（不新增 D-Bus 方法、不进 `config.yaml`、不进
`network_history` 表），唯一出口仍是既有的 `observer_attribution`。

若将来要做上表里「高」优先级的两项（真机标定 + ABI 对称），因涉及
`WEAKNET_KMOD_ABI_VERSION` 变更与跨版本兼容，那时值得走正式 change。

# weaknet_cpu_stat 真正上线：CAP_PERFMON 门禁、宿主交叉编译、OTA 打包与加载单元

> 日期：2026-11-07
> 分类：工作流外实施（承接 `2026-11-06-host-pressure-observer-attribution.md` 的遗留项 3、4）
> 结果：已完成（真机验证通过；`SELF_INFLICTED` 归因分支待真实过载时自然触发）

## 构思

### 要解决的问题

`驱动面能力引入与业务融合方案.md` §9.1 阶段 0 的第 2 项：「`weaknet_cpu_stat` 真正上线
（构建/打包/加载/ABI 校验）」。前一日的任务已经把**消费端**做完（`host_pressure` 插件、
`ObserverAttribution` 归因、双发布），但那天的记录诚实地留了两条遗留项：

- 「未上真机验证」；
- 「`kmod/weaknet_cpu_stat.ko` 仍不在 OTA manifest 里，属于设计边界」。

也就是说：代码写好了、单测过了，但**在真机上从未产出过一个有效读数**。
`weaknet-cli experience` 里的 `observer_attribution` 因此永远走 `UNATTRIBUTED`
分支（CPU 数据不可用），整个归因能力等于不存在。

### 为什么不新开 OpenSpec change

用户明确指示「不用新开 change；以后只要我不让你新开，你就默认不用新开」。
按 `openspec/tasks/README.md` 的规则，走工作流外实施 + 本记录。
（该偏好已写入项目 memory：`no-new-openspec-change-by-default`。）

### 路线选择：为什么用「驱动面」而不是 `/proc/stat`

用户明确要求「用路线 b，需要与驱动保持紧密联系」。备选是放弃 kmod、直接读
`/proc/stat` 算 `CPU_BUSY_RATIO`——那样一天就能收工，但会：

1. 丢掉这条已经写好、长稳 30 分钟、5851 万次读取零失败的采集链路；
2. 放弃「驱动面能力」这个项目定位（`kmod/` 目录的全部技术积累）；
3. `/proc/stat` 的 `CONFIG_HZ=250` 量化问题一样存在，并没有精度优势。

所以路线 B = 保留驱动面，把「写好但从未生效」的链路补成真正可部署的。

### 三个必须先解决的技术未知

1. **权限门禁与服务 capability 不匹配**（真因，见下）
2. **宿主机无法交叉编译**：板端 headers 包里带的是 aarch64 的 `fixdep`/`modpost`，
   x86 宿主跑不了；而 headers 包没有顶层 `Kconfig`，`make prepare` 也跑不了
3. **加载机制缺失**：没有任何 systemd 单元负责 `insmod`

## 实现方式

### 一、根因：`CAP_SYS_ADMIN` 门禁 + 掩盖真因的错误分类

**现象**：模块加载正常、`/dev/weaknet_cpu_stat` 节点存在、root 手工 `open`+`mmap`
读到 `abi=2 nr_cpus=8`，但服务端持续输出：

```
[host_pressure] cpu pressure unavailable: device_absent
```

**真因**（用 `capsh` 真降级实验定位）：

| 实验 | 命令 | 结果 |
| --- | --- | --- |
| 等价服务端能力集 | `capsh --drop=cap_sys_admin`（保留 PERFMON） | `OPEN_FAIL errno=1 EPERM` |
| 负对照 | `capsh --drop=cap_sys_admin,cap_perfmon` | `OPEN_FAIL errno=1` |

模块的 `open()` 只校验 `CAP_SYS_ADMIN`，而 `tools/weaknet-server.service` 的
`AmbientCapabilities` / `CapabilityBoundingSet` **刻意不含**它，只给
`CAP_NET_RAW CAP_NET_ADMIN CAP_BPF CAP_PERFMON CAP_DAC_OVERRIDE`。

**为什么改门禁而不是给服务加能力**：`CAP_SYS_ADMIN` 近乎全能，为了读一份
per-CPU 时间片就授予它，等于把整机管理权交给采集进程。`CAP_PERFMON` 是 Linux 5.8
为「观测系统」拆出的专用能力，语义恰好匹配。**改门禁后服务无需任何特权变更。**

**同时修掉的第二个坑**：消费端把所有 `open()` 失败都映射成 `device_absent`，
这正是真因被掩盖三天的原因。`HostCpuPressure` 早就声明了 `open_failed` 却从未使用。

### 二、宿主交叉编译：`tools/kmod_cross_build.sh`

板端 headers 包的 `scripts/` 里是 aarch64 二进制：

```
/bin/sh: 1: scripts/basic/fixdep: Exec format error
```

且没有顶层 `Kconfig`，无法 `make prepare` 重建。解决办法是**就地重编为 x86-64**，
只编外部模块真正用到的两个工具：

| 工具 | 编译要点 |
| --- | --- |
| `scripts/basic/fixdep` | 直接 `gcc fixdep.c` |
| `scripts/mod/modpost` | **必须 `-idirafter`** |

`modpost` 的头文件路径是双向陷阱：

- `-I` 把内核树插前面 → `include/linux/types.h` 覆盖 glibc `stdint.h`
  → `conflicting types for 'uint64_t'`
- 完全不指定 → 新版发行版 `/usr/include/linux/mei_uuid.h` 的 `uuid_le` typedef
  与 `file2alias.c` 自带定义冲突
- `-idirafter`（内核树排系统头**之后**，缺符号才补位）→ 两边都不冲突

**vermagic 是唯一可加载性判据**（内核 `load_module()` 逐字比对）。脚本对每个 `.ko` 显式校验：

```
期望: vermagic=5.15.147-99btf1-a733 SMP preempt mod_unload aarch64
实际: vermagic=5.15.147-99btf1-a733 SMP preempt mod_unload aarch64   ✅
```

实测交叉编译产物与板端就地编译产物 vermagic 逐字一致。

头文件缓存在 `$HOME/.cache/weaknet-kmod-headers/<release>/`（59 MB，不进仓库）。
已缓存时**不需要板子在线**也能出包——CI 与无板环境都适用。全量约 5 秒，增量约 2.5 秒。

### 三、加载单元：`tools/weaknet-kmod.service`

核心不变量是方案 §5.3.1 写明的「**模块加载失败必须不阻塞服务启动**」，落实为：

| 措施 | 原因 |
| --- | --- |
| 单元用 `Wants=` 而非 `Requires=` | `Requires=` 会把失败传播给依赖方 |
| `weaknet-server` **不**反向依赖本单元 | 依赖方向单向，模块缺席时服务照常起 |
| 所有 `ExecStart` 带 `-` 前缀 | 加载失败不算错误，不产生 `systemctl --failed` 噪声 |
| `After=weaknet-server` | 服务端是懒加载+每轮重试，模块后插能自愈 |
| `insmod` 走 `current/kmod/*.ko` | 随 OTA 翻链自动跟着翻版本 |
| 先 `rmmod` 再 `insmod` | 避免「用户态已升、模块还是旧的」跨版本错配 |

`weaknet_softirq` 与 `weaknet_lradc_keys` 也一起打包（同一套构建链，成本为零），
但**没有单元加载它们**、没有生产消费者。`.ko` 在包里不等于在用。

### 四、接入 `tools/ci.sh`

- **Step 2（打包）**：调用 `kmod_cross_build.sh --out dist-arm64/kmod`。
  失败**不中断主构建**——可选观测能力，缺它业务照常。
- **Step 3（部署）**：落盘 + enable `weaknet-kmod.service`。
  实际 `restart` 放在 **adopt 之后**——`current` 翻链前加载会拿到上一个版本的 `.ko`。
- 产物随 adopt 自动进 `manifest.sha256`（adopt 对 release 内**所有**文件做哈希），
  即 OTA 完整性覆盖自动生效，无需额外代码。

### 五、验证手段

| 层 | 手段 | 结果 |
| --- | --- | --- |
| 单测 | `test_observer_attribution_gtest` 新增 EPERM/EACCES 分类守护 | 先做 RED 验证（把实现改回一律 `device_absent`，两条新测试确实失败），再 GREEN |
| 全量回归 | `ctest --test-dir build-x86/server` | 57/57 通过 |
| 交叉编译 | `kmod_cross_build.sh` 从零重建工具 + 三个 `.ko` | 5.1 秒，vermagic 全通过；二次运行 2.6 秒（幂等） |
| 无板降级 | `BOARD=nonexistent` 跑脚本 | 复用缓存成功出包 |
| 单元语法 | `systemd-analyze verify` | 通过（宿主无关警告除外） |
| 真机部署 | `./tools/ci.sh` 全流程 | EXIT=0，kmod 进 release、模块加载、设备节点生成 |
| 真机读数 | `journalctl -u weaknet-server \| grep cpu_busy_ratio` | `0.0258 → 0.0117 → 0.0086`（空闲） |
| 真机满载 | 8 核 busy loop 30 秒 | `0.3547 → 0.999857 → 0.779363`，8/8 核参与 |
| 缺数据语义 | 模块未加载时 | `device_absent (open: No such file or directory)`，不输出 0 |

## 结果

### 已达成的

- `cpu_busy_ratio` 在真机上产出真实数据，服务端**未增加任何特权**
  （capability 集仍是 `CapabilityBoundingSet=cap_dac_override cap_net_admin
  cap_net_raw cap_perfmon cap_bpf`）。
- 模块随 OTA 发布、进 `manifest.sha256`、由独立单元加载、失败不阻塞服务。
- 错误分类如实：`EPERM` 不再被说成「设备不存在」，`detail` 带 errno 文本。
- 57/57 单测通过，新增测试有 RED→GREEN 证据。

### 遗留（诚实记录）

1. **`SELF_INFLICTED` 分支未在真机触发。**
   满载期间 `observer_attribution` 仍是 `NONE`——这是**正确行为**：归因前提是
   「观测确实降级」，而那次满载期间 eBPF 的 `capture_emit_failure_ratio` 与
   `perf_delivery_loss_ratio` 都是 0，没有证据丢失，自然无可归因。
   真机复现需要「CPU 满载 + eBPF 真丢证据 + 持续 DNS 流量」三条件同时成立，
   且丢证据不能人为注入（那会变成伪造证据）。留作现场观察项。

2. **归因白名单可能覆盖不全（新发现，未在本次修复）。**
   真机上 `tcp_connect` 报 `reason=observer_unreliable_pairing_incomplete`
   （`unmatched_ratio=79.07%`），但这个 reason **不在**
   `observer_attribution.cpp` 的 `kObserverReasonPrefixes` 白名单里
   （白名单仅 4 条：`capture_emit_failure` / `event_loss` / `ambiguity` /
   `tracker_capacity_overflow`）。
   而且归因调用点只传 `dns_sle`，`tcp_connect` 的降级**根本不进归因**。
   这意味着即使 CPU 数据已可用，TCP 侧的观测降级仍永远不会被归因。
   属于独立缺口，需单独评估（涉及归因语义边界「哪些 reason 算观测降级」），
   本次不动。

3. **`weaknet_softirq` / `weaknet_lradc_keys` 仍未接生产**，与本次前一致。

### 是否值得升格为 OpenSpec change

不必要。本次是「补链路」而非新增行为规格：所有业务语义（`CPU_BUSY_RATIO` 的
双发布、缺数据不伪造 0、归因不改 `OverallPolicy`）都已由前一日的实施确定，
本次只是让它们第一次真正跑起来。唯一的行为变更是错误分类细化
（`device_absent` → 区分 `open_failed`），已有单测守护。

遗留问题 2（归因白名单与 `tcp_connect` 未接入）**值得**单独作为一个任务或
change 评估——它有真实业务影响（TCP 侧降级不可归因），且涉及语义边界决策。

## 附：本次改动的文件

| 文件 | 改动 |
| --- | --- |
| `kmod/weaknet_cpu_stat.c` | 门禁 `CAP_PERFMON \|\| CAP_SYS_ADMIN` + 长注释说明取舍 |
| `kmod/weaknet_softirq.c` | 同上（同构改动） |
| `server/include/host_pressure_monitor.hpp` | 新增 `openErrnoToReason()` 纯函数、`detail` 字段 |
| `server/src/host_pressure_monitor.cpp` | 用纯函数分类、`detail` 带 errno、日志输出 detail |
| `server/test/unit/test_observer_attribution_gtest.cpp` | 新增 2 条分类守护测试 |
| `tools/kmod_cross_build.sh` | **新增**，宿主交叉编译 + vermagic 校验 |
| `tools/weaknet-kmod.service` | **新增**，fail-soft 加载单元 |
| `tools/ci.sh` | Step 2 打包 kmod、Step 3 落盘单元、adopt 后触发加载 |
| `kmod/README.md` | 定位更新、第零节交叉编译、三·五节踩坑记录、边界订正 |
| `驱动面能力引入与业务融合方案.md` | §5.3.1 落地状态与真机验证表、§9.1 第 2 项标完成 |
| `docs/ci-测试使用指南.md` | kmod 构建说明、套件数 56→57、测试覆盖描述 |
| `CLAUDE.md` | 快速操作指南里的套件数 56→57（与 `ctest -N` 实测对齐） |
| `docs/skills-and-openspec-guide.md` | 同上（同一处历史漂移） |

> 注：`驱动面能力引入与业务融合方案.md` 的 §6.2.2 / §10 摄像头选型内容
> 属于另一条并行会话的改动，不在本次任务范围内，提交时需与本任务的文件分开。

---

## 附二：「DNS 没有数据」的实验确证

本次顺带把悬着的「DNS 为什么没有数据」从**推断**升级为**实验证实**。

**此前的推断**：板端唯一的 DNS 流量是服务端自己的主动探测，被
`dns_monitor.bpf.c` 的 `is_self_traffic()`（按 `dns_self_pid` 排除自身 PID）
全部拦掉，因此被动计数恒为 0，DNS SLE 报 `no_dns_observations`。

**实验**（2026-11-07，板端，不改任何代码）：

| 步骤 | `send_dns_port` | `send_emitted` | `emitted` | DNS SLE |
| --- | --- | --- | --- | --- |
| 基线（仅服务端自身探测） | 125 | 0 | 0 | `UNKNOWN / no_dns_observations` |
| 从**另一个进程**发 3 次查询（`getent hosts`） | 135 | 6 | 12 | `GOOD / dns_service_healthy fail=0/6` |

**结论**：过滤逻辑是对的（服务端自流量确实必须排除，否则主动探测会人为
改善被动指标、掩盖真实业务失败）；「没有数据」是**环境事实**而非缺陷——
板子上没有第三方 DNS 客户端。一旦有外部进程解析域名，链路立即出数据。
无需修改任何代码。

（这也解释了为什么 `send_dns_port` 一直涨而 `send_emitted` 恒为 0：
前者在自流量判定**之前**累加，后者在其**之后**。两个计数器之间的落差
本身就是自流量过滤的观测窗口。）

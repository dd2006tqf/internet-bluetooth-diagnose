# 看门狗守卫自身可靠性加固：命令超时、连续复位熔断与磁盘取证

> 日期：2026-11-17
> 分类：**工作流外实施**（无活跃 OpenSpec change，`active_change: null`）
> 结果：**已完成，本地 118 项测试全绿 + 板端九项验收全过**
> 关联：`docs/ideas/看门狗可靠性加固-命令超时与复位熔断.md`（设想原文）、
> `openspec/tasks/2026-11-16-watchdog-plan-b-device-direct.md`（方案 B 交付记录）
> 影响面：`tools/weaknet-health-guard.cpp`、`tools/weaknet-health-guard.service`、
> `tools/test_weaknet_health_guard.py`、`弱网项目简历知识库.md`

## 一、构思

### 要解决的问题

方案 B 落地后，守卫的**判据链**是自洽的：四项判据全过才喂狗，任一持续失败即停喂复位。
问题出在判据**周边** —— 守卫自身的两个失效模式没有被覆盖。

**盲区一：守卫会被自己执行的命令焊死。**

改动前 `RunCommand()` 用 `popen` / `fread` / `pclose` 执行命令，零超时：

```cpp
FILE* fp = popen(cmd, "r");
size_t total = fread(buf, 1, cap - 1, fp);
int rc = pclose(fp);          // 子进程不退出，这里永久阻塞
```

两个调用点都在判据路径上（`busctl` 查 eBPF 健康、`systemctl show -p MainPID`）。
挂起的后果是：`pclose` 阻塞 → 主循环不推进 → 不再喂狗 → 硬件超时复位，
而 `WriteFailureJson` **永远执行不到**。

结果是「板子自己重启了，查不到任何原因」。
这违反看门狗第一原则：**它把自己的故障伪装成了被监控对象的故障**。

顺带暴露两个被低估的问题：

- `fread` 读满 64 KB 就返回，随后 `pclose` 等子进程退出，而子进程正阻塞在写满的管道上 —— 输出超限本身就会死锁，与 hang 无关；
- 「慢但没死」的 `busctl`（比如 5 秒）把判定周期从 2 秒拉长到 7 秒，`kDebounceK = 2` 去抖窗口的时间语义跟着漂移。

**盲区二：没有任何东西记得上一次启动是被守卫打掉的。**

| 机制 | 生命周期 | 复位后 |
| :--- | :--- | :--- |
| `RestartTracker` | 进程内存 | 归零 |
| `kTripFlag` | 跨进程 | **故意作废**（靠 `boot_id` 比对） |

`kTripFlag` 的 `boot_id` 设计是对的（保证重启守卫无法撤销已判定的复位），
但它同时意味着**没有跨 boot 记忆**。确定性故障（配置写坏、数据库损坏、失败的 OTA）下：

```
boot 1 → 判据失败 → 停喂 → 复位
boot 2 → 判据失败 → 停喂 → 复位     ← 一模一样
boot 3 → ...                        ← 无限
```

每次只活约 28 秒，**远程隧道来不及建连**，人进不去修 —— 在现网等于必须派人到现场。

### 为什么不做进 OpenSpec change

三项都是对既有守护进程的加固，不改需求、不加新表面、不引入新依赖，
且必须在同一份源码上一起验证（第二项依赖第一项：熔断要在启动阶段写文件，
若启动阶段也能挂住，熔断器本身就是新的死锁点）。走完整 Planner → Generator →
Evaluator 流程的收益不足以覆盖成本，按项目管理规则走工作流外实施 + 本记录。

### 一个被明确否决的方案

外部评审的常见建议是「把判据放到独立线程跑」。**不采纳**：

- 线程化只是把阻塞挪走，**判据本身仍然可以永远得不出结论**；而看门狗在
  「无法判定」时必须按「不健康」处理 —— 它照样需要一个超时。
- 一旦有了超时，单线程就够了。
- 多线程会引入「守卫卡死但喂狗线程还活着」这个最坏组合：**保护看起来在，判据已经死了**，
  比现状更糟。

真正的解耦是「判据不得无限阻塞」，不是「判据换个线程」。

### 第三个想法被自己推翻

初稿想给磁盘加第五项判据（剩余低于阈值即判不健康）。重新推演后否掉：

- 「磁盘满」是**复位无法修复的条件**。加进判据 = 磁盘满 → 停喂 → 复位 → 还是满 → 再停喂 → 复位，正是第二项熔断要防的循环；
- 这个洞**已经堵上了**：磁盘满 → SQLite 写不进 → WAL 的 `mtime` 停止推进 → `CheckWalMtime`（`kWalMaxAgeSec = 12`）失败 → 复位。判据 1 已覆盖，只是归因不显式。

于是磁盘水位降级为**纯取证信号**：只进日志与失败快照，不参与 `ok` 判定。

## 二、实现方式

### 1. `RunCommand()` 重写（`tools/weaknet-health-guard.cpp:438`）

签名从两参数变为四参数：

```cpp
constexpr int kCmdTimeoutMs = 1500;
char* RunCommand(const char* cmd, size_t cap, int timeout_ms, int* out_rc);
```

内部流程：`pipe2(O_CLOEXEC)` → `fork` → 子进程 `setpgid(0,0)` + `dup2` + `execl`
→ 父进程 `setpgid(pid,pid)` → `poll` 循环按**剩余时间**算 timeout → 超时 `kill(-pid, SIGKILL)`
→ `waitpid` 循环吃 `EINTR` → `rc = -ETIMEDOUT`。

三个必须做对的细节：

| 细节 | 做错的后果 |
| :--- | :--- |
| 子进程 `setpgid` + `kill(-pid)` | 只杀 `sh`，孤儿 `busctl` 泄漏并继续占管道 |
| 父进程也 `setpgid(pid,pid)` | 子进程还没跑到 `setpgid` 就被杀，`kill(-pid)` 打空 |
| 看门狗 fd 已带 `O_CLOEXEC` | 子进程继承 fd，父进程死了 fd 还开着 → 内核不执行 `release()` → **复位永远不来** |

缓冲满但子进程还活着时，继续 drain 到丢弃用小缓冲直到 EOF —— 保住「截断但不死锁」。

**超时值预算**：

```
kCheckCount(4) × kCmdTimeoutMs + kTickSeconds×1000 < 16000
       4 × 1500      +       2000          = 8000 ms  < 16000   ✅ 余量 50%
```

实际只有 2 个判据起子进程，真实最坏 `2×1500 + 2000 = 5000 ms`。
取 1500 ms 而非 500 ms：板端 `busctl` 实测 25 ms，但 SQLite checkpoint 叠加 12 个
eBPF 程序时 D-Bus 应答可能抖到几百毫秒，1500 ms 是实测值的 60 倍。

**超时后的语义**：算失败（`ok=false`，detail 写 `timeout Nms`），交给现有
`kDebounceK = 2` 去抖吸收。核心不变式：**无法判定 ≠ 健康**。

### 2. `GetMainPid()` 三态化（`:730`）

超时被引入后暴露出第三个缺陷：`GetMainPid` 返回 0 时无法区分
「systemctl 超时」与「服务真的没了」。若混为一谈，一次 systemctl 抖动会走
「服务缺席 → 12 秒宽限 → 停喂 → 复位」—— **正是这次加固要消灭的
「守卫把自己的故障伪装成被监控对象的故障」**。

改法：加 `bool* out_unobservable` 出参，主循环用 `kPidUnknownMaxStreak = 3`
有界容忍（`fprintf` 明示 `UNOBSERVABLE (n/3)`，期间继续喂狗但判据挂起），
超过 3 轮才按 `pid_unobservable` 触发，且这条路径会正常写失败快照。

### 3. 连续复位熔断（`:324-368`、`:1535`、`:1599`、`:1628`）

**关键约束：这块板没有 RTC**，启动初期系统时间在 1970 附近，NTP 校正会跳变。
任何「最近 N 小时内触发过几次」的时间窗都不可靠 ⇒ 改用**不同 boot_id 计数**。

数据结构 `/var/lib/weaknet/health-guard-trip-history`，纯文本一行一次：

```
<boot_id> <reason> <unix_epoch_or_0>
```

- 触发时 append 并 `fsync`；只保留最后 `kTripHistoryMax = 5` 行（读-截断-重写）；
- 用 `boot_id` 去重，同一 boot 内重复触发不重复计数。

判定（启动时，**必须在 `open()` 之前**）：最后 `kTripBreakerThreshold = 3` 行的
`boot_id` 互不相同且都不含当前 boot → 熔断。

阈值取 3 而非 2：一次真实故障（内存瞬时耗尽、磁盘临时写满）复位后自愈，
不该判死；连续三次说明复位无法修复。

**熔断模式动作**：

| 动作 | 理由 |
| :--- | :--- |
| **不 `open` 看门狗** | open 会 arm 硬件，进程一旦意外退出反而触发复位 |
| 退出码 **98**（`kExitBreakerOpen`） | 与 `kExitTripped = 99` 并列；`RestartPreventExitStatus=98 99` |
| 写 `/var/lib/weaknet/health-guard-rescue.json` | 记录进入原因、触发的 boot_id 窗口、时间 |
| 每轮 `fprintf(stderr, ...)` | journal 里一眼看见「守卫已熔断，无硬件保护」 |

98 与 99 分开是给运维的信号：**看到 99 查采集服务，看到 98 查设备本身**。

**健康复位**：正常满 `kTripHistoryResetSec = 30 分钟` 且期间未触发 → 清空 history。
没有它，一台设备跑几年偶发两三次真实故障，历史会永久累积，第四次直接熔断 ——
等于把「偶发」当成「确定性」。

**清退方式**：人工删除 history 文件。与 `kTripFlag` 同一哲学 —— 自动机制无权
推翻「系统已判定不健康」；同理，自动机制也无权推翻「保护本身正在制造故障」的判断。

**必须承认的取舍**：熔断模式下**没有硬件看门狗保护**，之后若发生真正的内核挂死，
没有任何东西会复位它。这是刻意的权衡：**「确定性故障下必然砖化的保护」<「偶发挂死时
没人兜底的裸奔」**。一个每 28 秒重置的网关等于永久离线；一个能长期在线但少了兜底的
网关，至少还能被远程修好。

### 4. 磁盘取证（`:996-1060`、失败快照 `disk` 块）

```cpp
struct DiskWatermark { const char* path; unsigned long free_pct;
                       unsigned long long avail_mb; bool ok; };
void SampleDisk(const char* path, DiskWatermark* out);   /* statvfs(3) */
void SampleBothDisks(DiskWatermark* ev, DiskWatermark* data);
void LogDisk(DiskWatermark& ev, DiskWatermark& data);
```

采样两个路径：`/var/lib/weaknet`（证据与标记）与 `/home/radxa/weaknet/data`（WAL）。
**不做「取较小值」合并** —— 合并正好会把「哪个分区满了」这个关键信息抹掉。

用 `f_bavail` 而非 `f_bfree`：后者含保留块，对「我这进程还写不写得进去」没有意义。

日志尾部：` disk[evidence=41%/2048MB data=3%/52MB] DISK_LOW`。
阈值 `kDiskWarnPct = 10`（只触发日志，不改喂狗行为）。

失败 JSON 增加 `disk` 块，并把 `"reset_expected_in_s": 16` 改名
`"hardware_timeout_s": 16` —— 旧名会让人读成「16 秒后板子就回来了」，
而实测端到端约 28 秒，不是同一条时间线，名字必须改。

### 5. 静默失败改为可见（`RecordTrip()` `:1383`）

改动前 `RecordTrip()` 与 `WriteFailureJson()` 都是 `if (fd < 0) return;` 静默返回。
`RecordTrip` 失败意味着 trip flag 没写下 → 复位后无任何记忆 → **第二项的熔断器
也跟着失效**，而且没人知道。

改为 `fprintf(stderr, ...)` 明确喊出「trip flag LOST，breaker will not see this boot」，
并在 `open` 失败路径同时打印磁盘水位。不改变行为，只改变「故障是否可见」。

### 6. `tools/weaknet-health-guard.service`

- `RestartPreventExitStatus=99` → `98 99`；
- 新增 `1b)` 段解释退出码 98 的语义与运维含义；
- 「停止语义：三种情况」→「四种情况」。

### 7. `tools/test_weaknet_health_guard.py`（89 → **118 项**）

新增：

- `TestCommandTimeout`（9 项）：无 `popen`/`pclose`、用 `poll` 且按剩余时间算 deadline、
  杀整个进程组、无条件 `waitpid` 收尸、子进程不继承看门狗 fd、
  **预算不等式** `CMD_TIMEOUT_MS*4 + TICK_S*1000 < HARDWARE_TIMEOUT_S*1000`、
  超时算失败不算通过、`MainPID` 超时不等于服务死亡、pid 未知容忍有界；
- `TestResetBreaker`：`breaker` 先于 `open` 判定、触发写历史、静默失败已可见，
  外加 Python 复现 `BreakerOpen()` 的计数规则（0/1/2/3 次、同 boot 去重、当前 boot 排除、健康复位清空）；
- 磁盘采样：Python 复现 `statvfs` 百分比与阈值。

## 三、结果

### 本地验证（全部通过）

| 项 | 命令 | 结果 |
| :--- | :--- | :--- |
| 判据测试 | `python3 tools/test_weaknet_health_guard.py` | **118 项通过** |
| x86 编译 | `cmake --build build-x86 --target weaknet_health_guard` | 通过 |
| ARM64 交叉 | `cmake --build build-arm64-native --target weaknet_health_guard` | 通过，ELF aarch64 |
| 单元解析 | `systemd-analyze verify tools/weaknet-health-guard.service` | 仅报「本机无板端路径」，预期 |
| shell 语法 | `bash -n` 相关脚本 | 通过 |

### 超时行为真实进程验证（不是结构断言）

写了一个独立复刻 `RunCommand` 的探针（**已删除**，不留在仓库），5 项全过：

```
fast: echo hi                       rc=0      elapsed=   1ms    PASS
hang: sleep 30                      rc=-110   elapsed=1500ms    PASS
big output 200KB（不死锁）           rc=0      elapsed=   3ms    PASS
hang after output（保留部分输出）    rc=-110   elapsed=1200ms    PASS
hang with grandchild                 rc=-110   elapsed=1200ms    PASS
```

**孤儿进程正反双向验证**（用独立符号链接做唯一进程名标记，避免上一次的误报）：

| 方向 | 实现 | 结果 |
| :--- | :--- | :--- |
| 正向 | `kill(-pid, SIGKILL)` | 超时后残留 **0** 个标记进程 |
| 反向对照 | `kill(pid, SIGKILL)`（只杀直接子进程） | 残留 **2** 个孤儿 |

反向对照是必须的：如果 naive 实现也不泄漏，说明这个测试构造不出孤儿，
正向的「0 残留」就证明不了任何事。对照成立后才确认 `kill(-pid)` 的进程组语义是必需的。

> 中途出现过一次误报：`pgrep -c -f "^sleep 60"` 报了 1 个残留，
> 查下去是 `/usr/local/bin/weaknet-tunnel-guard --watch`（root 进程）的子进程，
> 与探针无关。改用唯一标记后正反两向都干净。

### 文档同步

| 文件 | 改动 |
| :--- | :--- |
| `弱网项目简历知识库.md` | 条目六新增开发过程第 7~9 条（命令超时、熔断、磁盘取证）；0.1 速查表补 5 行；0.4 不可主张清单补 2 条；检索索引同步 |
| `docs/ideas/看门狗可靠性加固-命令超时与复位熔断.md` | 状态从「设想（未实施）」改为「已实施（源码级）」；三项 checkbox 勾选 |
| 本文件 | 新建 |

### 板端验收（2026-11-17，开发板在线，全部通过）

开发板：`radxa-cubie-a7a`，Linux 5.15.147-99btf1-a733 aarch64。

| 项 | 手段 | 结果 |
| :--- | :--- | :--- |
| **1. 超时杀挂死进程** | 构造假 `busctl`（`sleep 60` 再返回）注入 PATH，跑 dry-run | `sample_ns=FAIL(timeout 1500ms)`，第 8 轮起 `persistent failure -> stop feeding`；**轮次持续推进不失锁** |
| **1b. 孤儿清除（正向）** | 唯一标记符号链接 + `kill(-pid)` | 4 次超时后残留 **0** 个 `sleep 60`（用 `/proc` 精确统计，不靠 pgrep 自匹配） |
| **1c. 孤儿清除（反向对照）** | 同上，改成 `kill(pid)` 只杀直接子进程 | 残留 **2** 个孤儿 —— 对照组成立，证明进程组语义必需 |
| **2. 熔断真实触发** | 构造 3 个不同历史 `boot_id` 的触发记录 | 退出码 **98**、rescue JSON 正确、**设备未被 arm**（`/proc/*/fd` 无持有者） |
| **2b. 熔断边界 a** | 只 2 条历史（不足阈值） | 不熔断，正常进入主循环 |
| **2c. 熔断边界 b** | 3 条但含当前 `boot_id` | 不熔断，契约 OK 并开始喂狗 |
| **2d. 熔断边界 c** | 3 条但 `boot_id` 全重复 | 不熔断 |
| **3. 磁盘取证** | 真机 `statvfs` 对比 `df` | `disk[evidence=88%/104785MB data=88%/104785MB]` 正常输出；`f_bavail`(88%) 与 `f_bfree`(92%) 差 4 个点，验证选 `f_bavail` 正确 |
| **4. 真实模式 + 设备契约** | 装上部署路径跑 20 秒（真打开设备喂狗） | `watchdog contract ok: identity=sunxi-wdt options=0x8180 timeout=16s`；10 轮喂狗；`graceful shutdown done` rc=0；**未复位**，板子存活 |
| **5. 维护标记** | 带 `/var/lib/weaknet/health-guard-maintenance` 运行 | 正确识别 `MAINTENANCE -> feeding, judgement disabled` |

**板端发现的一个事实（与设想不符，必须记录）**：

`/var/lib/weaknet` 与 `/home/radxa/weaknet/data` 在板端**是同一个分区**
（`stat -c %D` 都是 `803` = `/dev/sda3` 根分区）。因此两条采样必然得到相同数字。
这不影响正确性（分开采样在同分区时退化为冗余但不错误），但
**「分开采样」的价值在当前的板端布局下不成立** —— 若将来 `/home` 独立分区才有意义。
文档与代码都保留两条路径（对未来布局变化有防御性），但不得主张「已捕获两个不同分区的水位」。

**测试过程中的两次自纠**（记录下来，因为都是「差点被自己骗过」）：

1. `pgrep -af "sleep 60"` 与 `pgrep -af "guard-new"` 会**匹配到自身 shell 的命令行**，
   三次误报「有残留」。改用 `/proc/[0-9]*/cmdline` 精确遍历才得到真值 0。
2. 用 `grep -E "^\[round (1[0-9]|20)\]"` 数轮次时，把 round 11 之类的行漏掉，
   看起来像「判据行跳号」。展开完整输出后确认**没有跳号**，是正则的缺陷。

### 未做（必须说清，不得含糊带过）

1. **熔断器在「真实连续复位 3 次」后自然生效** —— 已用构造的历史文件验证判定逻辑与
   退出路径（等价于第 3 次复位后的启动状态），但没有真的让板子复位三次。
   差别只在于历史文件是构造还是真实 accumulate，判定代码路径完全相同；
2. **`statvfs` 在双分区布局上的价值** —— 板端两个路径同分区，该场景无法实测验证；

其他遗留：

- **产品侧决策未定**：熔断模式（停止复位、长期在线降级、无硬件看门狗兜底）是否可接受。
  代码已实现；若答案是「不可接受」，第二项应降级为**只计数 + 告警**，不进熔断。
  这是本次改动里唯一需要产品拍板的取舍。
- **`ci.sh` 仍不 `enable` 守卫**，一次复位后保护就没了，需要人工再拉起（与上次记录一致）。
- 上次记录的四项遗留（「采集停但进程活」端到端验收、1 小时接喂狗空跑、`D` 状态长稳基线、
  开机时看门狗是否已被使能的只读取证）本次**未推进**。

### 是否需要升格为 OpenSpec change

不需要。三项加固不改变任何对外契约：仍然只有 `write(fd,"1",1)` 与 `write(fd,"V",1)`，
**零 ioctl 改硬件状态**；仍然是「四项判据全通过才喂狗」，仍然只有停喂这一个复位出口。
新增的全部是「守卫自身的可靠性」与「故障的可读性」。

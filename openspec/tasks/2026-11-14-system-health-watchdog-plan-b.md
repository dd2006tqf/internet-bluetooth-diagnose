# 系统健康看门狗（方案 B）：从喂狗条件到硬件接管

- 日期：2026-11-14
- 类型：工作流外实施（未创建 OpenSpec change）
- 关联设想：`docs/ideas/喂狗条件检查-系统健康看门狗.md`
- 触发：用户指令「写进idea对应文档；然后开始完整做完吧」，随后「待会开发板断线后先把不用开发板的内容都做了」

## 一、构思

### 要解决的问题

工业网关部署在无人值守现场。硬件看门狗存在的唯一理由是「系统挂死时自动复位」，但标准用法把喂狗条件建立在**极弱事实**上：

```c
for (;;) { write(fd, "x", 1); sleep(2); }   // 证明的是「这个线程还被调度」
```

多线程程序里这几乎说明不了什么。板端实测：28 条线程的进程，任何一条卡在 `D` 状态、D-Bus 派发死锁、落库线程挂掉，喂狗循环都不受影响。后果是最难诊断的那一类故障 —— **云端看到「在线、心跳正常、但没有新数据」**。这比彻底离线更糟：彻底离线运维会去看，半死不死运维会以为现场本来就没异常。

目标：把喂狗条件从「进程活着」升级为「系统健康」。

### 三条被否掉的技术路线

| 路径 | 否掉原因 |
| --- | --- |
| 模块包装真实 watchdog，转发喂狗 | `watchdog_ping` 未导出（源码级事实，与配置无关）；唯一变通是 `dev_get_drvdata()` 拿别人私有结构直调 `.ops->ping`，跨驱动访问内部结构，内核版本一变就失效 |
| 走 pretimeout governor | `CONFIG_WATCHDOG_PRETIMEOUT_GOV` 未设置，整个 governor 框架不存在 |
| 重编内核补配置 | 板端 `/usr/src/linux-a733-5.15` 只有 1.4 MB，是打包残留，不是可编译源码树 |

### 选定路径：接管硬件 + 用户态判据

关键在于把「喂狗权」从内建 `sunxi_wdt`（超时固定 16 秒、`WDIOC_SETTIMEOUT` 返回 EINVAL）拿到用户态可控的自注册设备上，再由一个用户态守护进程按健康判据决定喂不喂。

这需要先回答一个没人回答过的问题：**喂狗到底写哪个寄存器、写什么值**。这一步花了两次整机复位才查清，也是整个任务最大的成本。

## 二、实现方式

### 1. 硬件事实的发现（两次复位换来的）

用 `/dev/mem` 直读运行中内核的物理内存（`/proc/kcore` 未开、`CONFIG_STRICT_DEVMEM` 未开），定位 `sunxi_wdt_ping` 的真实指令，得到：
- **喂狗 = `*(base + 0x0C) = 0x14AF`**（写 `1` 会被硬件静默丢弃 —— 这是前两次实验的失败原因）
- **enable/cfg 寄存器必须带使能键 `0x16AA0000`**，否则整字被丢弃且不报错。这是第二个坑：驱动第一版「open 成功、write 成功、硬件毫无反应」

双向验证（这一步不能省）：
```
只写 +0x0C = 0x14AF，每 2 秒，持续 24 秒 → 存活（> 16 秒超时）
停写 → 第 15~16 秒复位，boot_id 变更
```
只有「喂 → 活」+「停喂 → 死」同时成立，才能排除「写入把狗关掉了」这个替代解释。

### 2. `kmod/weaknet_wdt.c`（内核模块，约 310 行）

薄到什么程度：硬件部分只有三条写。

```c
writel(0x14AF,            base + 0x0C);   /* 喂狗 */
writel(0xB1 | ENABLE_KEY, base + 0x14);   /* 开狗 + 16s */
writel(0x00 | ENABLE_KEY, base + 0x14);   /* 关狗 */
```

关键设计决策：

- **不匹配 DT 节点**。`sunxi_wdt` 已绑定 `watchdog@2050000`，同一节点不能被第二个驱动 probe。走 `platform_device_register_simple()` + `devm_ioremap(0x02050000, 0x20)`，物理地址硬编码（A733 专用，源码顶部显式声明该假设）。
- **probe 里不做任何 cfg 自检写**。探测阶段改写配置会在「另一条路径正在喂狗」时打断它，也可能把已有超时编码改错。开发期的自检块已删除，理由写进注释。
- `.set_timeout` 只接受 16，其余返回 `-EINVAL` —— 与 `sunxi_wdt` 行为一致，不承诺未验证过的超时值。

### 3. `tools/weaknet-health-guard.cpp`（守护进程，约 700 行）

四条判据（全部只读）：

| 判据 | 出口 | 为什么是它 |
| --- | --- | --- |
| 落库推进 | `history.db-wal` mtime ≤ 12 s | 必须判 WAL 而非主库：WAL 模式下主库长期静止（实测 28 分钟不变），判主库会得到恒定假告警 |
| 采集推进 | `GetEbpfMonitorHealth` 的 `last_successful_sample_ns`，非零项中 ≥2/3 在 20 s 内 | 8 个监控器里有 2 个时间戳恒为 0（按需启用），要求全部推进会恒假告警 |
| 进程活性 | `/proc/<pid>/stat` 的 utime+stime，6 s 内 ≥1 tick | 不能看单线程 CPU：全部线程 comm 被截断成同一个名字，无法按名识别角色 |
| D 状态 | 任一 `D` 线程持续 > 8 s | 必须判持续而非瞬时，否则正常 IO 抖动即误复位 |

16 秒硬约束下：周期 2 s、去抖 K=2（最坏 4 s 确认）。K=3 被明确禁止。

### 4. 实现过程中发现的七个设计缺陷

这部分是本任务真正的价值所在 —— 每一条都会在真机上造成「每隔几天无故复位」或「故障了却永远不复位」，而且都不会在编译期暴露。

| 缺陷 | 后果 | 修法 |
| --- | --- | --- |
| `Restart=always` | guard 判定不健康后停止喂狗并退出，systemd 立刻拉起新实例 → `open()` 触发 `.start` → **使能并喂狗，复位被静默取消**。故障还在、保护没了，比不加保护更糟 | `Restart=no` + boot_id 触发标记在 `open()` 之前拦截（双层） |
| 「MainPID 为 0 立即断喂」 | `RestartSec=5`，每次自动重启、`ci.sh` 部署、OTA 升级都会把整机打复位 | 12 秒容忍窗口（首次取 8 秒被测试拦下：8 = 5+3 刚好够，启动慢 1 秒即误复位） |
| 没有归还步骤 | guard 正常退出写 `'V'` 停用硬件看门狗，而 `sunxi_wdt` 仍 unbind ⇒ `systemctl stop` 后**两套看门狗都没了**，`status` 却一切正常 | `ExecStopPost`（不是 `ExecStop`：后者在主进程自己退出/启动失败时不执行） |
| `ci.sh` 停服数分钟 | 部署期间服务缺席远超容忍窗口，部署中途复位整机，最坏落点是「指针已翻、release 未转正」 | 维护标记（停服前创建、EXIT 陷阱清理、30 分钟自动过期）+ OTA 回执识别 |
| 新 PID 继承陈旧判据值 | 服务重启后 WAL mtime 仍是几分钟前，两个周期后直接复位 —— 升级成功却变成刷机 | 12 秒豁免窗口 + 20 分钟内重启 >6 次则取消豁免（防崩溃循环被无限豁免） |
| **D 状态判据用 `awk '{print $3}'` 数线程** | 字段 2 是 `(comm)`，comm 含空格时按空白切分整体错位：`1234 (my service name) D ...` 的第三段是 `service` 而非 `D`，**卡在 D 的线程数不出来、判据恒通过**。且 `popen` 退出码被丢弃，扫描失败时 `grep -c` 输出 `0` 同样被当成「没有 D 线程」。两个方向都是漏报 | 改 `opendir` + 逐 tid `ReadFile`，用 `strrchr(')')` 定位状态字段（与同文件 `ReadProcCpuTicks` 统一），扫描失败返回 `-1` |
| **`dist-arm64/tools/` 目录从未创建** | 打包段 `install ... dist-arm64/tools/...` 在 `set -e` 下必然失败，整个打包中断，后续升级脚本、内核模块、部署全部不执行 | `mkdir -p` 列表补 `dist-arm64/tools` |

**两个缺陷的发现方式值得记录**：第 6 条是 ARM64 交叉编译的 `-Wunused-const-variable` 告警牵出来的 —— 告警本身只是「`kDStateMaxTasks` 定义了没用」，顺着查为什么没用，才发现数线程那段根本没走目录遍历。第 7 条是在核对 `ci.sh` 打包段路径时，用 `install` 实测「父目录不存在会失败」确认的。**两者在 x86 g++ 编译和 78 项测试全绿的状态下都存在**，靠的是换编译器和逐行核对路径，而不是任何自动关卡。

**为什么第 6 条特别危险**：漏报意味着「保护看起来在、实际不在」。误报会复位板子留下痕迹，漏报什么都不留 —— 板子照常运行，`systemctl status` 一切正常，直到某天有人问「为什么采集停了两周没人发现」。

**维护标记为什么必须过期**：如果 `ci.sh` 创建标记后被 Ctrl-C，标记会留在板上。永久有效的标记 = 永久无脑喂狗 = 看门狗在、保护不在、没人知道。这比误复位更糟（误复位至少是响的）。

**OTA 回执为什么用 mtime**：升级进程若死掉，receipt 会永远停在 `IN_PROGRESS`。而升级脚本每完成一步就 `touch` 一次，因此「mtime 新鲜」等于「升级进程还活着」。停住即恢复正常判定 —— 否则一次崩死的升级会永久关掉保护。

### 5. 安全网：`tools/weaknet-wdt-takeover.sh`

接管有一个「期间系统没有任何硬件看门狗保护」的窗口，顺序不可颠倒：

```
insmod → 确认 /dev/watchdog1 存在 → unbind sunxi_wdt → 任一失败立刻 bind 回来
```

`unbind` 后 `sunxi_wdt` 的设备静默消失（`sunxi_wdt_stop()` 什么都不打印），`/dev/watchdog1` 不会重编号成 `watchdog0`（编号不回收）。两点都板端实测确认。

已触发（判定不健康）状态下，`take` 与 `give-back` 都拒绝执行：`rmmod` 会经 `watchdog_stop_on_unregister` 调 `.stop`、bind 回原驱动会让本模块退场 —— 两者都会关掉硬件看门狗，把已排定的复位取消。

### 6. 测试：`tools/test_weaknet_health_guard.py`（78 项）

判据全是「解析别人给的字符串」，错的方向有两种且都不好发现：偏严 → 恒假告警 → 每次波动都复位；偏松 → 恒真通过 → 判据形同虚设。两者在板端都表现为「看起来在跑」。

测试用 Python 复刻判定规则并与之断言，其中：
- **常量漂移守护**：从 C++ 源码正则抽取 `constexpr` 值，与本文件断言值对照。常量改了但测试没改会立刻失败。
- **边界逐项钉死**：WAL 年龄、sample_ns 的 2/3 比例与时钟回跳、`/proc/<pid>/stat` 的 comm 含空格/括号、D 状态持续、OTA 新旧、boot_id 换行。
- **`test_grace_covers_systemd_restart` 抓到了真缺陷**：断言 `容忍窗口 > RestartSec + 启动耗时`，8 > 8 失败 → 常数从 8 改为 12。这是本次唯一由测试（而非推理）发现的参数错误。
- **`TestDThreadCounting` 把「数错」这个方向钉死**：原先的 D 状态测试只覆盖「数出来之后怎么判」，覆盖不到「数不出来」。新用例用 comm 含空格 / 含右括号的 stat 构造行，同时断言正确实现数得出、旧的空白切分实现数不出，并加源码守护禁止退回 `awk '{print $3}'` 与 shell 管道。

## 三、结果

### 已完成并验证（不需要开发板）

| 产物 | 验证 |
| --- | --- |
| `kmod/weaknet_wdt.c` | 交叉编译通过；板端实测接管成功、双向验证复位行为 |
| `tools/weaknet-health-guard.cpp` | x86 g++ `-Wall -Wextra -Werror -Wpedantic` 零告警；**ARM64 clang 交叉编译同样零告警**；CMake 集成构建通过 |
| `tools/weaknet-wdt-takeover.sh` | `bash -n` 语法通过 |
| `tools/weaknet-health-guard.service` | `systemd-analyze verify` 通过（仅报板端路径不存在，符合预期） |
| `tools/test_weaknet_health_guard.py` | **78 项全通过**（含 D 线程计数方向的源码守护） |
| `tools/ci.sh` | 打包接管脚本（含 `dist-arm64/tools` 目录创建）、落盘单元（**不 enable**）、部署期维护标记、`bash -n` 通过 |

### 改动清单

- 新增：`kmod/weaknet_wdt.c`、`tools/weaknet-health-guard.cpp`、`tools/weaknet-wdt-takeover.sh`、`tools/weaknet-health-guard.service`、`tools/test_weaknet_health_guard.py`
- 修改：`kmod/Makefile`（`obj-m += weaknet_wdt.o`）、`client/CMakeLists.txt`（构建目标）、`tools/ci.sh`（打包/落盘/维护标记/构建目标）、`tools/kmod_cross_build.sh`（日志文案）、`docs/ideas/喂狗条件检查-系统健康看门狗.md`（寄存器事实、使能键、方案 B 规格、实现后修正）

### 未完成（需要开发板）

1. `--dry-run` 跑 10 分钟，确认四项判据恒通过（不误报）
2. 接喂狗空跑 1 小时不误复位
3. 故障注入 `kill -STOP` → 验证 16 秒复位 + JSON 落盘
4. **真正的验收标准**：「采集停但进程活」必须触发复位。这一条不过，方案 B 就白做了

### 边界与已知风险

- **默认不启用**：`ci.sh` 只落盘单元、不 `enable`。这是行为改变型能力，启用是运维决策，须先 `--dry-run` 观察。
- `/dev/sda3` 是 `write back`，失败 JSON 的 `fsync` 只保证「内核 → 设备」，复位瞬间仍在缓存里的块可能丢。**该 JSON 不保证一定留下来。**
- `WDIOC_GETBOOTSTATUS` 恒为 `0x0`、`WDIOF_CARDRESET` 从不置位 ⇒ 无法事后从内核判断「上一次复位是否看门狗所致」，这也是必须自己写 JSON 的原因。
- 板端硬件超时固定 16 秒、不可调；所有阈值都压在 16 秒预算内，改动任一参数都必须重新核对 `test_16s_budget`。

---

## 四、2026-11-15 板端验收（第二轮）：两个恒失败缺陷的发现与修复

### 4.1 缺陷实证

2026-11-15 首次在板端跑 `--dry-run`，四项判据里**两项恒失败**，且都是「代码看着对、编译零告警、78 项测试全绿，真实环境里 100% 失败」：

| 判据 | 故障现象 | 根因 |
| --- | --- | --- |
| `cpu_delta` | `FAIL(read stat failed)` 恒失败 | `ReadProcCpuTicks` 从 `rp + 2` 起 `strtoull`。`rp` 指向 `)`，`rp+2` 是**状态字符**（`S`/`D`），不是数字，`strtoull` 立即失败 → 对任何进程都返回 false |
| `sample_ns` | `FAIL(no active monitor)` 恒失败 | `kKey` 找的是不转义的 `"last_successful_sample_ns":`，而 busctl 返回的是 D-Bus 字符串渲染形式 `\"last_successful_sample_ns\":`（引号带反斜杠）→ `strstr` 永不命中 |

**若当晚直接 enable，板子会陷入 16 秒重启循环。**

### 4.2 为什么测试没抓到（本次最有价值的发现）

两个缺陷同源：**测试从规格出发，没有从实现出发**。

- `TestProcStatParsing.parse_cpu_ticks` 用 `split()` 后取 `fields[11]+fields[12]`，这是**正确逻辑**，但它复刻的是「应该怎么写」，不是 C++ 里那个 `strtoull` 循环。夹具从没经过真实代码路径。
- `TestSampleNsParsing` 的夹具是**裸 JSON**，而 busctl 真实输出每个引号都带反斜杠。「夹具是否忠实于真实输入」这件事，测试自己无法验证 —— 只能拿真实输出回来对。

### 4.3 修法

```c
/* 缺陷 A：跳过状态字符与其后的空格，从字段 4 开始 */
char* p = rp + 2;
if (p[0] == '\0' || p[1] != ' ') return false;
p += 2;                              /* → 字段 4 */

/* 缺陷 B：键必须带转义的反斜杠 */
static const char* kKey = "\\\"last_successful_sample_ns\\\":";
```

同时把「键失配」与「值全为 0」的 detail 分开：一个解析 bug 伪装成业务事实（`no active monitor`）是本缺陷最危险的地方，运维会去查采集，而真正坏的是判据自己。

### 4.4 变异测试：验证新守护是真守护

补的回归测试必须自身可验证。做了三组变异，全部被捕获：

| 变异 | 注入内容 | 捕获者 |
| --- | --- | --- |
| A | 删掉 `p += 2`（字段起点退回状态字符） | `test_source_starts_at_field_4` ✅ |
| B | `kKey` 退回不转义 | `test_source_uses_escaped_key` ✅ |
| C | 两条 detail 合并成一条 | `test_key_not_found_reported_separately` ✅ |

**变异 B 第一版守护是假的**：原先写 `assertIn('\"last...\"', src)`，而正确写法 `"\\\"last..."` 与错误写法 `"\""..."` **都包含**该子串，所以改错了也通过。改为「提取 kKey 字面量 → 按 C 语义解码 → 比对解码后的真实值」后才真正生效。这条经验值得记住：**源码守护不能靠子串包含，必须解码后比对值。**

测试从 78 项增至 **84 项**，全绿。

### 4.5 板端复验（修复后）

```
[round 7]  wal_mtime=ok(age_s=4) sample_ns=ok(fresh=7 nonzero=7) cpu_delta=ok(baseline established) d_state=ok(d_threads=0)
[round 10] wal_mtime=ok(age_s=5) sample_ns=ok(fresh=7 nonzero=7) cpu_delta=ok(delta=192 in 6s)         d_state=ok(d_threads=1 starting)
```

两项恒失败判据均转绿。`nonzero=7` 与真实 busctl 输出逐项核对一致：10 个监控器中 3 个恒为 0（`SkbDropMonitor`、`TcpRttMonitor`、`BtAudioAnalyzer` 未初始化），7 个活跃。

### 4.6 反向验证：`kill -STOP` 注入

冻结 `weaknet-server` 后（板端本地脚本控制时序，避免 ssh 往返错乱）：

```
[round 9]  sample_ns=FAIL(busctl rc=256,f1)          ← busctl 调用超时被杀
[round 10] wal_mtime=FAIL(age_s=29,f1) sample_ns=FAIL(fresh=1 nonzero=7,f2)
[round 10] persistent failure -> stop feeding, expect reset in ~16 s
```

三点确认：
1. **去抖 K=2 生效**：单次失败不动作，连续两轮才停喂。
2. **新诊断分支正确**：`fresh=1 nonzero=7` 报的是「值陈旧」，不是「键失配」——键命中了，只是值没推进。
3. **停止喂狗路径确实被触发**，这正是设计意图。

### 4.7 新发现的边界：部分采集停止不被捕获

禁用 5/7 个活跃监控器（`dns`、`http_latency`、`process_profiler`、`tcp_retrans`、`tcp_conn`）后：

```
sample_ns=ok(fresh=2 nonzero=2)   ← 比例仍是 2/2 = 100%，判据通过
```

**被禁用的监控器时间戳归 0，等于从分母里剔除**，所以「部分采集停止」不会被此判据捕获。这不是代码 bug，是判据语义的必然结果，但必须写清边界：

- **能捕获**：活跃监控器集体停更（进程冻结、采集线程死锁、全部监控器挂掉）
- **不能捕获**：部分监控器停止、且停止的那些时间戳恰好吃 0

若要把「部分停止」也纳入，需要改用「总量基线」而非「每项比例」，属于设计变更，不在本轮范围。

### 4.8 硬件链路验证（安全、可回滚）

| 步骤 | 结果 |
| --- | --- |
| `insmod weaknet_wdt.ko`（不动 sunxi_wdt） | ✅ 注册成功：`phys=0x2050000 window=0x20 ping=0xc magic=0x14af`；`/dev/watchdog1` 出现 |
| 打开 `/dev/watchdog1` 喂狗 20 s（> 16 s 超时） | ✅ 10 次喂狗全成功，**未复位** —— 证实 `ping=0xc` 是正确喂狗寄存器 |
| 写 `'V'` 优雅关闭 | ✅ 成功 |
| `rmmod weaknet_wdt` | ✅ 恢复原状，仅剩 `/dev/watchdog0` |

**这一步解决了历史会话里悬而未决的核心问题**：`+0x0C` 是喂狗寄存器，且喂的是 `0x14AF` 而非 `1`。

### 4.9 未执行：真实复位验证（需用户授权）

「停止喂狗 → 16 s 内硬件复位 → boot_id 变更」尚未验证。这一步会**真的重启板子**，属破坏性操作。历史记录显示 2026-11-14 曾做过两次整机复位（用于定位寄存器），但那是在接管链路尚未完备时做的。

### 4.10 当前状态

- 模块：已卸载，板子处于实验前状态
- 服务：`weaknet-server` active，PID 9194，无残留 guard 进程
- `/dev/watchdog0`：未被任何进程打开（**板子当前本就没有硬件看门狗保护**，这是既有事实，不是本次改动造成）
- 单元：已落盘、**未 enable**

---

## 五、2026-11-15 板端验收（第三轮）：give-back 的静默取消复位缺陷

### 5.1 缺陷：三种退出路径会让「已排定的复位」被静默取消

第五节之前，`give-back` 只有一道闸：`tripped_this_boot`（读 trip 标记比对 boot_id）。这道闸漏掉了三类退出 —— 它们**根本来不及写标记**，而 systemd 在所有这些情况下都会执行 `ExecStopPost`：

| 退出方式 | 有机会写 trip 标记 | `ExecStopPost` 执行 | 原实现的后果 |
| --- | --- | --- | --- |
| `SIGKILL` / `SIGSEGV` / OOM | ❌ | ✅ | `rmmod` → `.stop` → 硬件看门狗被关 → 永不复位 |
| `return 1`（open/喂狗失败） | ❌（代码本就不写） | ✅ | 同上 |
| `RecordTrip()` 写文件失败（磁盘满） | ❌ | ✅ | 同上 |

后果是最坏的一种：**故障还在、保护没了、`systemctl status` 一切正常**。

### 5.2 修法：用 systemd 注入的退出事实，而不是只看标记

先用与本任务无关的无害探针单元在板端实测取到真实取值（**不是推测**）：

| 场景 | `SERVICE_RESULT` | `EXIT_CODE` | `EXIT_STATUS` |
| --- | --- | --- | --- |
| `systemctl stop`（优雅 SIGTERM） | `success` | `killed` | `TERM` |
| 捕获 SIGTERM 后 `exit 0` | `success` | `exited` | `0` |
| `kill -9` | `signal` | `killed` | `9` |
| 主进程 `return 99` | `exit-code` | `exited` | `99` |
| `TimeoutStopSec` 超时 | `timeout` | `killed` | `9` |

**第一行是陷阱**：正常 stop 时 `EXIT_CODE=killed`、`EXIT_STATUS=TERM`，**都不是 0**。因此唯一可靠的主判据是 `SERVICE_RESULT`：

```
success → 允许归还    其余 → 拒绝归还
```

实现过程中踩到并修正的两个真 bug（都由矩阵测试抓到，不是推理出来的）：

1. 第一版比较 `EXIT_CODE != 0` —— 但 `EXIT_CODE` 的取值是 `exited`/`killed` 这类**符号**，不是数字。
2. 第二版比较 `EXIT_STATUS != 0` —— 板端实测正常 stop 是 `TERM`，于是**每次正常停止服务都被拒绝归还**，模块与设备永久残留。

### 5.3 板端场景一验证：正常 stop 必须归还且不复位

部署修好的脚本后执行 `systemctl stop weaknet-health-guard`：

```
stop rc=0
BOOT=bce458fc-b5c5-4adf-92a3-90c08acc793e   ← 与停止前一致，未复位
uptime=1713.28                              ← 持续增长，未重启
服务状态: inactive
设备:   /dev/watchdog0 恢复（另有 udev 创建的 /dev/watchdog 别名）
模块:   已卸载
sunxi:  2050000.watchdog 重新绑定
```

日志确认走了 allow 分支：

```
[wdt] 开始归还
[wdt] 已 bind 回 sunxi_wdt
[wdt] 已卸载 weaknet_wdt
[wdt] 归还完成：/dev/watchdog0 已恢复
weaknet-health-guard.service: Succeeded.
```

### 5.4 测试：89 项，含「真跑 do_give_back」的副作用断言

新增 `TestGuardExitSemantics`（11 个场景矩阵，真跑 bash）与 `TestGiveBackDoesNotReturnOnUncleanExit`（真跑 `do_give_back`，用 PATH 桩观察 `rmmod` 是否被调用）。

**本轮又抓到一次假守护，值得记录**：`TestGiveBackDoesNotReturnOnUncleanExit` 的第一版只断言源码里出现 `guard_exited_uncleanly` 字符串。把实现改成 `if false; then` 之后，该测试**依然全绿** —— 与 4.4 节 sample_ns/cpu_delta 是同一个病根：**检查「代码写了什么」，而不是「代码做了什么」**。

| 变异 | 注入内容 | 第一版（字符串断言） | 现在（真执行） |
| --- | --- | --- | --- |
| A | 主判据退回 `EXIT_STATUS != 0` | ✅ 捕获 | ✅ 捕获 |
| B | `do_give_back` 完全不查退出方式 | ❌ **漏过** | ✅ 捕获（3 个场景） |
| C | `SERVICE_RESULT` 判据改成永远放行 | ✅ 捕获 | ✅ 捕获 |

写「真执行」测试时又踩了两个环境坑，都表现为**假通过**而不是报错，因此必须写下来：

- 脚本会在文件头把 `SYSFS_DRV` / `NEW_DEV` / `TRIP_FLAG` **重新赋值**成板端写死路径。在 source 之前覆盖会被冲掉，必须在 source 之后覆盖。（这个特性本身是好的：生产行为不会被环境变量意外改变。）
- `kmod_loaded` 查 `/sys/module/weaknet_wdt`，宿主机上必然不存在 → 永远进不了 `rmmod` 分支 → 测试对「不洁退出不 rmmod」恒真通过。必须用桩覆盖 `kmod_loaded`。

### 5.5 更正：1 小时 soak 并未完成

第四轮记录写过「1 小时 soak 已跑完」，**这是错的，现予更正**。按 journal 实测：

```
服务启动 07:53:42 → 最后一次运行 08:18:55
实际时长  25 分 13 秒
轮次      738 轮（round 1 .. round 738），2 秒周期
失败       0 次；warning 0 条；NRestarts=0；无 trip 标记
```

即：**25 分钟接喂狗空跑全绿**，而非 1 小时。结论方向不变（判据在真实环境下稳定、不误报），但时长证据必须按实际写。1 小时空跑仍未做。

### 5.6 本轮未完成（全部阻塞于开发板）

用户于 2026-11-15 16:27 主动断开开发板，以下四项均未完成，不做任何「已经通过」的声明：

1. `--dry-run` 持续 10 分钟（已启动一次，运行到第 3 轮时随板子断开而中止，**无结论**）
2. 接喂狗 1 小时空跑（上一次实跑 25 分钟，见 5.5）
3. 真实复位验证：停喂 → 16 秒复位 → `boot_id` 变更
4. `enable` 服务（运维决策，按设计默认不开启）

### 5.7 复核发现的未取证前提：`sunxi_wdt` 是否设 `WDIOF_MAGICCLOSE`

2026-11-15 复核「为什么必须接管硬件」时发现一处**记录空档**。方案 B 选「自建模块接管」
而非「直接 `open("/dev/watchdog0")` 按健康判据喂狗」，技术上依赖一个前提：

> 自定义模块设了 `WDIOF_MAGICCLOSE`，因此裸 `close()` 不停用硬件，
> 「判定不健康 → 裸 close → 16 秒复位」这条路才成立。

**但 `sunxi_wdt` 自身的 `options` 值从未取证。** 本地已核对：仓库、`board-assets/`、
`~/.cache/weaknet-kmod-headers/` 都没有 `sunxi_wdt.c` 或内核镜像，无法离线查。

影响：若 `sunxi_wdt` **也设了** `MAGICCLOSE`，则「接管硬件」不是唯一可行路线 ——
同一套健康判据直接写 `/dev/watchdog0` 即可，能省掉内核模块、安全网脚本与
`take`/`give-back` 状态机（以及第三轮才发现的静默取消复位缺陷）。

因此「必须接管」目前是**设计选择**，不是被证据强制的结论。待板子恢复后取证：

```bash
cat /sys/class/watchdog/watchdog0/options    # bit 5 (0x20) = WDIOF_MAGICCLOSE
```

> 注：本轮已把方案 B 的代码缺陷修完、测试补到 89 项并做了变异验证，这些工作
> 即使将来改走「直接开原厂设备」路线也不白费 —— 健康判据、去抖、维护标记、
> OTA 过渡识别、失败 JSON 全部可复用，只有接管层会被替换。

另有一项板端遗留需清理：`/etc/systemd/system/` 下三个无害探针单元（`probe-execstop`、`probe2`、`probe3`）已删除，`/tmp/probe*.log` 因属 root 未能删除（不影响任何功能）。

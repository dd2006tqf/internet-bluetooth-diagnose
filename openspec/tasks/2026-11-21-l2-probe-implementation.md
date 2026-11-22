# 二层回流探测（T4-a/b/c）实施：从 LoopGuard 吸收的最后一项

> 日期：2026-11-21（2026-11-22 补板端验证与两处缺陷修复）
> 分类：工作流外实施
> 结果：**已完成** — 代码、x86/ARM64 构建、回归、隔离 netns 四分支、
> 板端 ARM64 四分支、生产部署（`releases/1.1.0`）、端到端 `ci.sh` 全部通过。
> 验证过程中抓到五个缺陷并全部修复（详见「验证抓到的缺陷」各节）

## 构思

### 起点：T4 曾被判定「不该做」，后来判定「只做观测那一半」

2026-11-18 的 LoopGuard 吸收记录（`2026-11-18-loopguard-engineering-absorption.md`）
里，T4（AF_PACKET 主动探测帧 + 端口/VLAN 隔离）被明确推迟，判词是：

> 它属于 L5 执行层，与产品定位「只观测、不控制」冲突。要用它，必须先改
> 产品定位文档承认主动注入探测帧这一能力，并重新划定权责边界——这是产品决策，
> 不是工程决策。

用户随后做了那个产品决策，选择了**方案 B**：把 T4 拆成两半——

| 子项 | 内容 | 决定 |
|---|---|---|
| T4-a | 探测帧收发引擎（AF_PACKET） | 做 |
| T4-b | 轮次调度与限速 | 做 |
| T4-c | 回流证据 → 归因结论 | 做 |
| T4-d | 端口/VLAN 隔离（Netlink） | **永久禁止**，写进产品定位文档的禁止项 |

这个拆分点不是工程折衷，而是权责边界：**发探测帧是「测量方法」，隔离是「执行动作」**。
两者在任何实现里都相邻（拿到「A 出 B 回」之后，「那我关掉 B」是极自然的下一步），
所以 T4-d 不是「暂不做」，而是明确写成**永久的禁止项**，并给出契约级理由
（见 `docs/架构设计-五层权责契约.md` 的「主动测量的层归属」）。

### 为什么不走 OpenSpec change

沿用本仓库既定做法（`2026-11-18-loopguard-engineering-absorption.md` 与
`no-new-openspec-change-by-default` 记忆）：这是工作流外实施，记录放 `openspec/tasks/`。
代码先写后补 change 会因 footprint 为 0 报废，禁止。

### 关键设计决策

**1. 自定义 EtherType `0x88B5`，理由是「隔离」不是「不冲突」**

IEEE 802 Local Experimental Ethertype 1。选它的决定性理由不是「没有协议冲突」，
而是**内核级隔离**：项目已有全部 eBPF 程序都不解析 `0x88B5`，所以探测帧在
BPF 捕获路径里**根本不存在**。

这是本项目踩过一次坑之后的产物——`docs/网络体验评价体系.md` 记录了主动探测的
DNS 报文被被动 DNS 探针捕获、导致 `queue_emitted` 虚增的事故，并由此确立了
「Active/Passive 绝不共享捕获路径」这条纪律。那次靠的是「记得加 PID 过滤」，
这次靠的是协议本身不存在于解析路径——更硬的保证。

**2. `SOCK_RAW` 而非 `SOCK_DGRAM`**

`SOCK_DGRAM` 会剥掉以太头，就拿不到 dst MAC，也就无法排除「探测帧被自己的
IP 栈收下又吐回来」这个假阳性。必须看到完整以太头。

**3. 引擎只产出账本，归因是独立纯函数**

```text
L2ProbeEngine  →  L2ProbeRoundRecord（事实，落两表）
                        ↓
                  attributeL2Loop()（推断，纯函数）
```

与 `ObserverAttribution` / `ThermalAttribution` / `VideoQoeAttribution` 四同构：
纯函数、无 I/O、**绝不修改** SLE / OverallPolicy 的 state/coverage/reason，
只在一旁追加独立结论。

纯函数拆分的实际理由：没有 `CAP_NET_RAW`、没有第二块网卡的 x86 CI 上
可以穷举所有退化组合——而那恰恰是最容易出错的部分。

**4. 措辞 `LOOP_SUSPECTED` 而不是 `LOOP_CONFIRMED`**

从「帧从另一个口回来」到「这是环路」仍是推断。命名成 `CONFIRMED` 会把推断
伪装成事实，违反「事实与推断必须分开」的总原则。它也不得写入
`site_incidents.suspected_cause`——那是 Phase 4b 诊断器的权力。

**5. 默认关闭 + 不内置任何默认出口/对端**

三个理由：自定义 EtherType 裸帧在生产网络里是**异常流量**，可能触发客户
IDS/审计；出口网卡选错就真的往业务链路注入探测帧；「向谁发」必须由部署方
显式声明（与 `active_probe.targets` 同一纪律）。

**6. 「未采集 ≠ 零值」**

`vlan_id` 默认 `NULL`/`-1` 而非 `0`（`0` 是合法的 priority-tagged 语义，
与「无 tag」严格区分）；`recv_ms`/`rtt_us`/`sent_ms` 在不适用时绑 NULL。

## 实现方式

### 新增文件

| 文件 | 行数 | 职责 |
|---|---|---|
| `server/include/assurance/l2_probe_plan.hpp` / `.cpp` | 141 / 129 | 令牌桶限速、单轮批次规划、覆盖完整性（纯逻辑，无 socket） |
| `server/include/assurance/l2_loop_attribution.hpp` / `.cpp` | 146 / 83 | 三值归因纯函数（`NONE` / `NO_LOOP` / `LOOP_SUSPECTED`） |
| `server/include/l2_probe_engine.hpp` / `.cpp` | 196 / 857 | AF_PACKET 收发、seq 配对、假阳性排除、配置接线 |
| `server/test/unit/test_l2_probe_plan_gtest.cpp` | 436 | 34 项（25 项设计测试 + 3 项 netns 实测回归 + 6 项出口资格与分母口径） |

### 改动文件

| 文件 | 要点 |
|---|---|
| `server/include/weaknet_config.hpp` | `l2_probe` 配置结构（默认关闭，无默认目标） |
| `server/src/weaknet_config.cpp` | 5 处接线：YAML、get/setMonitorEnabled、运行时 setConfigValue、JSON 序列化；**不含** `trialableKeyNames()`（出口/对端变更会改变探测帧落到哪条链路） |
| `server/include/database_manager.hpp` | `L2ProbeFrameRow` 增加 `bool sent` |
| `server/src/database_manager.cpp` | 两张表 schema、`insertL2ProbeRound()`、`queryL2ProbeRounds()`、`cleanup()` 扩展为八张表 |
| `server/include/server.hpp` | `l2_probe_thread`、`l2_probe` unique_ptr、`l2_loop_mutex` + `l2_loop_result` |
| `server/src/server.cpp` | 探测线程、持久化、窗口装配、归因发布、启动/关停接线、quality 循环回填 |
| `server/src/dbus_service.cpp` | `SetMonitorParam` 走**运行期**入口（`applyL2ProbeRuntimeConfig`） |
| `server/include/assurance/network_experience.hpp` | 三个字段 `l2_loop_attribution` / `_detail` / `_issues` |
| `server/include/assurance/legacy_adapter.hpp` | v2 JSON 序列化（**用转义**，因为 detail 里带网卡名） |
| `server/CMakeLists.txt`、`server/test/CMakeLists.txt` | 源文件与测试目标注册 |

### 三个必须写下来的实现要点

**1. `sent` 必须单列一维，不能靠 `sent_ms` 推断**

```sql
-- sent=0 会被读成"1970 年发出的"，而"sendto 失败、根本没出去"
-- 与"发出去了但没人回"是两种完全不同的证据缺口：
--   sent=0 → 本轮不完整
--   sent=1 且 ingress NULL → 真的没回流
```

**2. 配置 API 必须分两层，否则是 UAF**

`applyL2ProbeConfig` 会 `stop()` + `reset()` 引擎。若 `SetMonitorParam` 直接调它，
探测线程此刻可能正 `runRound()` 里持有那台引擎的裸指针 → UAF。

故拆成两个入口：

| 函数 | 时机 | 行为 |
|---|---|---|
| `applyL2ProbeConfig` | 启动（线程未建）/ 关停（线程已 join） | 重建引擎 |
| `applyL2ProbeRuntimeConfig` | 运行期任意时刻 | **绝不 reset**；只能改限流三参数（`updateLimits`）或置停止标志 |

只有「帧数上限 / 速率 / 超时」能在线改——它们不改变哪些 ifindex 参与、
帧是否带 802.1Q tag。出口/对端/VLAN 变更必须重建引擎，否则会跑出
「一半新目标一半旧目标」的轮次，那一轮的回流证据无法解释。

**3. `std::mutex` 不可重入，`configure()` 内层不能再加锁**

重构 `updateLimits` 时第一次实现给 `configure()` 里插了一层 `lock_guard`，
而 `configure()` 开头已经持 `mutex_` → 立刻自死锁。已删并留注释：
「**不要**在这里再套一层 lock_guard —— std::mutex 不可重入，内层构造会立刻自死锁。」

### 验证手段

**x86 单元测试**：`test_l2_probe_plan_gtest` 共 **34 项**，全绿。
覆盖令牌桶限速与批次上限不静默吞帧、覆盖完整性、三值归因真值表、
出口资格判据（单口拒绝 / 同口多 VLAN 仍算单口）、降级轮不进分子，
以及三条 netns 实测回归（见下）。

**全量回归**：`ctest --test-dir build-x86/server` → **72/72 通过**。

**ARM64 交叉编译**：`build-arm64-native/server/weaknet-dbus-server`
（23,481,696 字节，ELF aarch64）构建通过（仅既有 `-Wmismatched-tags` 告警）。

**隔离 netns 真实验证**（不需要开发板）：

在 `sudo -n unshare -n` 里用 veth 对 + netns + Linux bridge 搭受控二层域，
链接**真实引擎**（`L2ProbeEngine` + plan + attribution + logger）跑三个场景：

最终拓扑：测试 ns 内 `va`/`vc`，**对端 ns 内 `vb`/`vd` 用 bridge 连起来**，
形成 `va → vb →(桥)→ vd → vc` 的真实二层闭环。

| 场景 | 期望 | 实测 |
|---|---|---|
| 单口 `egress_ifaces="va"` | 拒绝启动 | `CONFIGURE_REJECTED: 只包含 1 个接口（va）…` ✅ |
| 对端不桥接 | 无回流 | `sent=4 recv=0 cross=0 covered=1 multi=0` ✅ |
| 对端桥接（真实环路） | 跨口回流 | `sent=4 recv=4 cross=4 pairs=va->vc,vc->va` ✅ |
| 对端 = 本机口 MAC | 降级 | `degraded=self_egress_loop covered=0` ✅ |

搭夹具的四个坑（都不在代码里，在测试手法里）——其中前两个曾让结论完全反过来：

- **`ip link set X netns $$` 在 `unshare -n` 里会静默失败**（被 `|| true` 吞掉），
  veth 对端留在同一 netns，`AF_PACKET` 把对端接口报成入口 → 误得 `cross=4`。
- **把对端两个口分到两个互相隔离的 ns** 时，它们收到帧直接丢弃 → 桥接分支永远 `recv=0`。
- **Linux bridge 不会把帧从 ingress 口发回去**，且 dst MAC 若是桥端口就本地投递。
  所以「构造环路」必须用**桥未学到的单播 MAC**（`02:00:00:00:00:01`）强制泛洪。
- **netns 里读不到 `/sys/class/net/<if>/address`**（`/sys` 反映宿主 netns），
  MAC 要从 `ip -br link show <if>` 第三列解析。

验证材料在 `/tmp/l2probe-verify` 构建，验证后已 `rm -rf` 删除。

### 验证抓到的缺陷（五个，全部已修）

#### 缺陷 1：`ip_forward == 1` 当作整轮降级判据 —— 功能永远没有结论

这个判据来自「防止帧被 IP 栈转发而假装成二层回流」的想法。netns 实测发现
`ip_forward=1` 时每一轮都被判降级。两条独立理由说明它是错的：

1. **判据与机制不匹配（主因，独立成立）。** `ip_forward` 决定的是 **IP 包**
   是否被转发；探测帧是自定义 EtherType `0x88B5`，IP 栈根本不解析它，更不会
   转发它。用 IP 转发开关否决二层观测，是拿一个不相干的开关否决一份不相干证据。
2. **它会把功能在部分部署里打成永久失效。** 只要部署方开了 `ip_forward`
   （网关型部署的常见形态），每轮就降级 → 归因恒 `NONE` → **装了等于没装**。

> **板端实测更正（2026-11-22）**：本文档此前写的「开发板作为网关 `ip_forward`
> 恒为 1」是**假设，且与事实不符**。板端实测
> `cat /proc/sys/net/ipv4/ip_forward` = **0**（内核 `5.15.147-100btf1amp-a733`）。
> 所以在本板上这条判据并不会真的把每轮打成降级——它是个**潜伏**缺陷：
> 一旦部署方打开转发，或换到别的网关型部署，才会显形。
> 删除该判据的决定**不变**，但依据必须诚实：成立的是第 1 条（机制不匹配）
> 和「其他部署可能为 1」，不是「本板恒为 1」。

第 2 条（修正后的形态）仍值得记住：这种「看起来在工作、实际永远没有结论」
的形态比功能缺失更难发现——日志里有轮次、有收发数字、没有报错，只有结论永远
是 `NONE/insufficient`。

真正的假阳性只有一种：**目标 MAC 就是本机某个口**（帧被自己的协议栈收下）。
已保留为唯一降级判据（`self_egress_loop`）。

桥转发**不是**需要排除的假阳性——它正是「二层通路存在」的证据，
正是我们要观测的东西。

#### 缺陷 2：单口配置被接受 —— `NO_LOOP` 变成谎话

头文件与配置注释都写着「至少 2 个接口才有判定资格」，但 `parseIfaces()`
**只检查了非空**。于是单口配置被接受，而 `has_multi_return` 的定义是
「帧从**非出口**接口回来」——只有 1 个出口时，任何回流帧的 ingress 必然
等于 egress，每轮 `MULTI_RETURN` 恒为 0，`attributeL2Loop()` 返回 **`NO_LOOP`**：
一个「确证没有环路」的结论，而证据在这个配置下**根本不可能存在**。

修法不是「在引擎里补一个 if」，而是把判据搬到纯逻辑层
（`checkL2ProbeEgressEligibility()` in `l2_probe_plan.cpp`）：那条规则是**语义契约**，
必须能被不需要 `CAP_NET_RAW` 的 CI 穷举，否则它只会在真机部署时以
「归因恒为 `NO_LOOP`」的形态暴露。判据看**去重后的接口数**而非 target 数——
同一接口配两个 VLAN 仍是一个出口（这条最容易写错，已有专门单测）。

#### 缺陷 3：降级轮被计入回流分子 —— 分母剔了、分子没剔

`L2ProbeWindow` 的字段注释写着「`multi_return_rounds`：仅统计非降级轮」，
但 `server.cpp` 的窗口装配把所有 `verdict == MULTI_RETURN` 的轮都算进分子，
分母却剔掉了降级轮。后果：一个「回流证据全部来自不可信轮次」的现场，
仍会被判成 `LOOP_SUSPECTED`。已修为两侧口径一致。

### 验证夹具本身踩的两个坑（不是代码缺陷，但会骗人）

第一次：`ip link set vb netns $$` **静默失败**（被 `|| true` 吞掉），veth 对端
留在同一 netns，`AF_PACKET` 把对端接口报成入口 → 误得 `cross=4`。
第二次：把 vb/vd 分到两个互相隔离的 netns，它们收到帧直接丢弃 → 桥接分支
永远 `recv=0`。

两次都是**夹具错了**，但只看输出数字会得出完全相反的结论。教训：环境构造
本身也要断言（先确认对端真的在另一个 netns、真的 up）。最终正确的拓扑是
**对端两个口在同一 netns 内桥接**，形成 `va → vb →(桥)→ vd → vc` 的真闭环。

## 结果

### 已完成的

- T4-a/b/c 全部落地：探测引擎、调度限速、归因纯函数、两表持久化、配置接线、
  D-Bus 运行时调参、`experience` 三字段出口。
- x86 全量 **72/72 通过**（新增 `test_l2_probe_plan_gtest` 34 项 +
  `test_weaknet_config_gtest` 的 `L2ProbeConfigTest` 5 项）；ARM64 交叉编译通过。
- 隔离 netns（x86）**四分支** + 板端 ARM64 **四分支**实测通过
  （单口拒绝 / 无桥 `NO_LOOP` / 真环 `LOOP_SUSPECTED` / 自环降级 `NONE`）。
- 生产部署到板端 `releases/1.1.0` 完成，默认关闭、零注入流量。
- 六个回归测试 + 五个配置接线测试固化进单测：
  - `L2ProbeRealWorldRegression.IpForwardMustNotDegradeTheRound` — 防止该判据换个形式回归
  - `L2ProbeRealWorldRegression.BridgeFloodingProducesGenuineMultiReturn` — 桥转发是证据不是噪声
  - `L2ProbeRealWorldRegression.SelfEgressLoopIsTheOnlyRealFalsePositive` — 唯一真假阳性
  - `L2ProbeEgressEligibilityTest.*`（5 项）— 单口拒绝、同口多 VLAN 仍算单口、空列表拒绝
  - `L2ProbeAttributionDenominatorTest.DegradedRoundsMustNotFeedTheNumerator` — 分子分母口径一致
  - `L2ProbeConfigTest.*`（5 项）— 默认关闭无默认目标、序列化覆盖、区间双向一致、`vlan_ids=0` 语义
- 文档同步：产品定位 v1.3（新增三之三「主动测量」能力类别与三条判据、
  把端口/VLAN 隔离写成永久禁止项、第八节登记实现状态）、五层权责契约 v1.1
  （「主动测量是方法不是层，按产物归属」）、模块与代码结构（第四同构归因入表）、
  体验评价体系（`l2_loop_*` 三字段与三条硬边界）、CLI 文档（监控器名与参数表）、
  `config.yaml`（全注释的样例块）、CI 测试指南（新测试目标一行）。

### 板端真机验证（2026-11-22，Radxa Cubie A7A / ARM64）

隧道恢复后补做。手法：把引擎编成**独立 ARM64 可执行文件**（依赖只有
glibc / libglog / libstdc++ / libm / libgcc_s），scp 到板端，在板端用
`unshare -n` 建 `veth + bridge` 拓扑跑四个分支。选独立 harness 而不是启动整个
服务，是为了让「引擎行为」与「服务/配置/数据库」解耦——后者任何一环出问题
都会让结论无法归因。

板端实测（内核 `5.15.147-100btf1amp-a733`）：

| 分支 | 输出 | 结论 |
|---|---|---|
| 单口配置 | `CONFIGURE_REJECTED: ... 只包含 1 个接口（va）...` | ✅ 拒绝启动，不是静默接受 |
| 不桥接，3 轮 | `sent=4 recv=0 cross=0 covered=1` | ✅ `ATTRIBUTION=NO_LOOP` |
| 桥接 + 未知单播，3 轮 | `sent=4 recv=4 cross=4` | ✅ `LOOP_SUSPECTED`（`3/3 轮观察到跨口回流（va->vc, vc->va, ...）`） |
| 对端 = 本机口 MAC，2 轮 | `degraded=self_egress_loop` | ✅ `NONE` / `reason=all_rounds_degraded` |

四个分支在真实 ARM64 内核上全部符合预期，与 x86 netns 结果一致。
验证后板端 harness 已删除，无残留 netns / veth，接口回到
`lo / sit0 / eth0 / wlan0`，服务 `active`。

**顺带核实的事实**：板端 `/proc/sys/net/ipv4/ip_forward` = **0**，
推翻了本文档此前「开发板作为网关恒为 1」的假设（详见缺陷 1 的更正框）。

### 生产部署验证（2026-11-22）

`./tools/ci.sh` 完成 ARM64 编译 + 打包 + 部署，板端 `releases/` 从 `1.0.0`
切到 `1.1.0`，健康窗口连续 3 次绿探针通过。部署后的产物确认含新代码：

- `grep -a 'l2_probe.egress_ifaces 只包含' /home/radxa/weaknet/current/server/bin/weaknet-dbus-server` 命中
- `weaknet-cli get l2_probe` 返回正确默认值且 `enabled:false`
- 板端配置**没有** `l2_probe:` 段，服务日志零条 `l2_probe` 行 —— 默认关闭，
  部署本身不产生任何注入流量

**`ci.sh` Step 4 未拿到绿色结果**（诚实记录）：该步因板子整机复位中断
（`Connection to 127.0.0.1 closed by remote host`, exit 255）。根因在
`weaknet-health-guard` 日志：`sample_ns=FAIL(timeout 1500ms,f1)` → `f2` →
`persistent failure -> stop feeding, hardware watchdog will reset the board`。
即 D-Bus 采样两轮超时触发守卫停喂狗、`sunxi_wdt` 硬件复位。**与 l2_probe 无关**
（板端无该配置、日志无该能力），是守卫判据侧的独立问题。因此 Step 4 的板端
冒烟测试仍待板子稳定后重跑。

### 验证抓到的第四个缺陷（Defect C）：`weaknet-cli list` 漏了 `l2_probe`

真机核对时发现 `weaknet-cli get l2_probe` 能读，但 `weaknet-cli list` 里没有它。
根因：`client/weaknet_cli.cpp` 的 `list` 输出是一段**硬编码 printf 列表**，
加配置接线时漏了这里。同一处历史上已经漏过 `skb_drop / tcp_connect /
active_probe / edge` 四个——它们能被 `set` 但 `list` 不显示，用户无从发现。
已补，并在代码里写明「两处改动必须同时做」。

同时发现 `l2_probe` 在 `test_weaknet_config_gtest.cpp` 里**零覆盖**。
补了 `L2ProbeConfigTest` 5 项：默认值（关闭 + 三个 ConfigString 为空）、
`serializeMonitorJson` 在 `l2_probe` 与 `all` 两个 key 下都包含它、
运行时越界拒收（rate 100 / frames 128 / interval 10 / timeout 1）与边界接受
（20 / 64）、YAML 与运行时**同一套区间**（走 `parse()` 公共入口验证，避免
「yaml 能写、`weaknet-cli set` 拒收」的双标准）、`vlan_ids=0` 必须存成 `"0"`
而不是空（VLAN 0 是合法的 priority-tagged 语义，与「无 tag」严格区分）。

这类漏接线只会在真机上由人肉对账发现，所以必须同时补测试锁死。

### 验证抓到的第五个缺陷（Defect E，与 l2_probe 无关但阻塞了验证）

`tools/weaknet-test-full.sh` 的手法与 `weaknet-health-guard` 的判据**直接冲突**，
会导致整机复位。

脚本第 37 行 `systemctl stop weaknet-server`，然后手动 `./bin/weaknet-dbus-server &`
跑测试，整个测试期（Phase 1 到 Phase 3，实测约 3 分钟）服务**不在 systemd 的
active 状态**。守卫把「`weaknet-server` 不在 active」当硬失效判据，容忍窗口只有
`kInactiveGraceSec = 12` 秒，超时即停喂狗 → `sunxi_wdt` 16 秒内复位整机。

2026-11-22 02:37 实测现场（守卫 `health-guard-last-failure.json`）：

```
wal_mtime:  stat failed errno=2        (手动进程没有 WAL)
sample_ns:  busctl rc=1                (手动进程不在 D-Bus 上)
cpu_delta:  read stat failed pid=6970  (pid 与 systemd 记录不一致)
d_state:    scan failed
action:     stopped_petting, hardware_timeout_s: 16
```

**四条判据同时失败看起来像「服务大面积损坏」，实际只是「服务被测试脚本按设计停掉了」。**
这正是最坏的失败形态：复位抹掉失败现场，运维只看到「板子自己重启了」。

此前多次部署没触发，是因为 12 秒 grace + `kDebounceK=2` 去抖偶然捡回；一旦某轮
`sample_ns` 超时（D-Bus 忙），四条判据同轮归零即刻触底。

**修法**（`tools/weaknet-test-full.sh`）：在停服**之前**建
`/var/lib/weaknet/health-guard-maintenance`，cleanup 里**先**等
`systemctl is-active` 真的回到 active、**再**撤标记。顺序与 `tools/ci.sh` 的
`cleanup_maint_flag` 完全一致，理由也相同——撤快了标记一撤就被判「服务不在」。
若服务真起不来则**保留**标记（30 分钟自动过期），让失败可诊断而不是被复位抹掉。

修复后板端冒烟 **PASS 28 / FAIL 0 / SKIP 1 / EXIT=0**，整机未复位，随后完整
`./tools/ci.sh` 端到端通过（`✅ 全部完成`），板子 uptime 持续增长、守卫连续
327 轮全部 ok。

### 未完成的（明确不算做完）

- 无。引擎、配置、CLI、文档、板端 ARM64 验证、生产部署、端到端 ci.sh 均已完成。
- 独立遗留（不在本次范围）：`weaknet-health-guard` 的 `sample_ns` 单轮超时即可
  与其它判据叠加触发复位，去抖策略值得独立评估——这已是一次真实生产中断，
  见 Defect E。

### 遗留与风险

| 项 | 说明 |
|---|---|
| T4-d 端口/VLAN 隔离 | **永久不做**。已写进产品定位文档禁止项与五层契约三条硬约束，不是待办 |
| 板端 `ip_forward` 实测值 | 实测 **0**，不是 1。删除该判据的依据是「机制不匹配」+「其他部署可能为 1」，不是「本板恒为 1」。已在缺陷 1 更正 |
| 单口配置的兼容性 | 单口现在**拒绝启动**（此前会被接受并恒报 `NO_LOOP`）。若已有部署配了单口，升级后该能力会变成 disabled 并在日志写明原因——这是刻意的行为变更，不是回归 |
| 上线开关 | 代码合入后仍需部署方显式配置 `egress_ifaces` + `probe_peers` 才生效；默认 `enabled: false`，合并本身不产生任何注入流量 |
| `ci.sh` Step 4 | **已通过**（PASS 28 / FAIL 0 / EXIT=0）。首次失败的真因是 `weaknet-test-full.sh` 与守卫判据冲突导致整机复位，已修（Defect E） |
| `weaknet-health-guard` 去抖策略 | 独立遗留。`kDebounceK=2` 是**逐判据**计数，「多条判据同轮全部失败」时各自都到 2，等于零缓冲。真实的服务消失/重启中间态会精准命中。建议独立评估 |
| 是否升格 OpenSpec change | 不需要。能力已完整落地并有单测 + netns + 板端 ARM64 + 端到端 `ci.sh` 证据；属于工作流外实施，记录在本文件 |

### 建议的下一步

1. 独立评估 `weaknet-health-guard` 的去抖策略：当前判据在「服务被合法停掉」与
   「服务真的死了」两种情形下形态相同，只能靠维护标记区分。`kDebounceK=2`
   对多判据同轮触底没有缓冲，已经造成两次真实整机复位（2026-11-21、2026-11-22）。
   这与 l2_probe 无关，但值得独立处理。

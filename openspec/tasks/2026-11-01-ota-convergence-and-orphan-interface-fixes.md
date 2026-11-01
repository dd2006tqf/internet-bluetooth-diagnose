# OTA 收敛链路与孤立接口修复（full-code-review 修复批次 3）

> 日期：2026-11-01
> 分类：工作流外实施（承接 [[2026-10-29-ota-p0c1-startup-gate]]，属 P0-C 序列）
> 结果：已完成。本机（Python 3.12 / systemd 255）与板端（**Python 3.9.2 /
> systemd 247.3**）全量验证结论一致；板端复验过程中新发现并修复 2 个只在
> 3.9 暴露的真实缺陷（D1/D2），详见「板端复验」。剩余遗留见文末。

## 构思

### 要解决什么

对 P0-C1 交付做了一次全仓代码评审，发现 **1 个会毁掉 OTA 闭环的缺陷** +
若干确定性缺陷与孤立接口。最严重的一条是「恢复服务在健康窗口耗尽后静默死亡」：

```
恢复器：return EXIT_RETRY(75)   // "我还没收敛，请重启我再试一轮"
单元  ：Restart=on-failure + SuccessExitStatus=75
```

`SuccessExitStatus=75` 的含义是「75 算干净退出」，于是 `Restart=on-failure` **不重启**。
设备停在未验证候选上：不推进、不回滚、不进救援。修法有两半，缺一不可：

1. **接线**：`SuccessExitStatus=75` → `RestartForceExitStatus=75`；
2. **计数**：退出前必须先把这次"窗口耗尽"落进持久计数，否则重启后计数不动。

第二半更容易被忽略：原实现依赖"重启后 boot_attempts 会加一"，但 systemd 重启的是
**恢复进程本身**，内核 `boot_id` 不变 → 控制器按 boot_id 幂等 → 计数原地不动；
而 `elapsed` 取自 `updated_at`（TRIAL 期间不变）→ 每轮立刻又"窗口耗尽"。
接上 `RestartForceExitStatus` 就直接变成重启风暴。

### 为什么不做进 OpenSpec change

交付面是板端启动闸门与运维接口：不新增对外 D-Bus 协议、不改云端契约。
与 P0-A / P0-B / P0-C1 同构，按 `docs/ai/implementation-economy.md` 判据
走工作流外实施并在此留记录。

### 先复现再改

`SuccessExitStatus` 与 `RestartForceExitStatus` 的差异**不靠推断**，直接拿 systemd
跑对照（退出码固定 75）：

| 配置 | `Result` | `NRestarts` | `ActiveState` |
|---|---|---|---|
| `on-failure` + `SuccessExitStatus=75` | `success` | 0 | `inactive`（**静默死亡**） |
| `on-failure` + `RestartForceExitStatus=75` | `exit-code` | 递增 | `activating`（**持续重启**） |

（本机 systemd 255 实测。板端是 247，复验未完成，见遗留 1。）

## 实现方式

### 改动文件

| 文件 | 性质 | 内容 |
|------|------|------|
| `tools/weaknet-ota-recover.service` | 修改 | `SuccessExitStatus=75` → `RestartForceExitStatus=75`；`StartLimitIntervalSec=0` 移到 **`[Unit]` 段** |
| `tools/weaknet-ota-recover` | 修改 | 窗口耗尽时先调 `bump-trial-round` 再 `return EXIT_RETRY`；控制器不可用时 **fail-closed 进救援**（不返回 75，否则无界重启）；`phase_started_at` 改用 `calendar.timegm`；`decide()` 默认参数不再在定义时绑定常量；`wait_trial`/`wait_rollback` 两支合并 |
| `tools/weaknet-ota-ctl` | 修改 | 新增 `bump-trial-round`（**不按 boot_id 幂等**，并刷新 `updated_at`）；`cmd_rolled_back` 补 `clear_rescue_flag`；删除零调用者的模块级 `sync_rescue_flag` |
| `tools/weaknet-ota-gate` | 修改 | 删除 `--dry-run`（解析后从不读取，且仍返回非零） |
| `tools/test_weaknet_ota_recover.py` | 修改 | 42 → 52 项：EXIT_RETRY 单元契约（含段位校验）、窗口耗尽计数、时区解析、wait 分支、两份 sync 规则一致性。**板端复验后追加 3 项跨版本守护，最终 55 项** |
| `tools/test_weaknet_ota_ctl.py` | 修改 | +5 项：`bump-trial-round` 四例（不幂等/刷新起点/仅 TRIAL/对照组）、`ROLLED_BACK` 清理派生视图 |
| `tools/test_weaknet_ota_gate.py` | 修改 | 28 → 32 项：恢复服务单元重启契约、门禁无 `--dry-run`、恢复单元不依赖业务服务 |
| `tools/ota_p0b_drill.sh` | 修改 | 21 → 30 项，新增第 9 节：窗口耗尽 → 持久计数 → 下一轮判定真的变化 → 次数耗尽转回滚 → 控制器不可用时 fail-closed |
| `.github/workflows/build-and-deploy.yml` | **删除** | 见下「孤立接口处置」O3 |
| `tools/ci.sh` | 修改 | 补「反向隧道收尾重启」（I5）；修正已过时的注释 |
| `tools/llama-server.service` | 修改 | 补齐 `MemoryMax=4.5G` + `CPUAffinity=0 1`，标注为唯一权威来源 |
| `server/include/bt_monitor.hpp` / `server/src/bt_monitor.cpp` | 修改 | `calculateAudioScore` 从私有成员提成命名空间自由函数，删 `friend class ::BtMonitorAudioScoreTest` |
| `server/src/active_connectivity_monitor.cpp` | 修改 | 删零调用者 `probeTcpConnect`、恒返回 `"A"` 的 `qtypeName` |
| `server/src/net_iface.cpp` / `server/src/net_wifiriss.cpp` | 修改 | 删零调用者 `ifFlagsToString` / `getInterfaceBssid` |
| `client/weaknet_client.h` | 修改 | 事件订阅族加失效声明与 `@deprecated` |
| `client/example_usage.cpp` | 修改 | 不再演示空转的订阅路径，改为真实可用的 `get_*` 查询 |
| `docs/OTA固件更新与安全发布指南.md` | 修改 | §1.4 补「两种计数分工」与「恢复进程重启契约」；§5.1 同步测试数量 |
| `docs/架构设计-模块与代码结构.md` | 修改 | 订阅族失效行为逐条精确化；llama 单元单源化说明 |
| `docs/本地LLM部署方案.md` | 修改 | 删自带 heredoc，改为引用仓库 `tools/llama-server.service` |
| `docs/ci-测试使用指南.md` | 修改 | workflow 表更新（删已失效的 build-and-deploy，补 release.yml 与四个作业） |

### 孤立接口处置（full-code-review 必产交付物）

| ID | 接口 | 证据 | 处置 |
|---|---|---|---|
| O1 | `weaknet_subscribe_*` / `weaknet_check_*` 订阅族 | 服务端 2026-10-19 已停发全部信号，零生产订阅者 | **标注失效**（头文件 + `@deprecated` + 文档），示例改走查询路径。彻底删除属独立变更 |
| O2 | `ListInterfaces` | 与 `GetInterfaces` 是同一 handler 的别名（`dbus_service.cpp:99/105`），文档列为兼容别名 | **保留并确认**为有意向后兼容 |
| O3 | `.github/workflows/build-and-deploy.yml` | build job 被 `release.yml` 完全覆盖且更弱（不签名、不产 OTA 包）；deploy job 直接 `rsync --delete dist-arm64/ → /home/radxa/weaknet/` + `restart`，**绕过 `adopt` 的 releases/current 原子翻链与健康窗口，并跳过验签** | **删除**（板端唯一入口是 `tools/ci.sh`，发布走 `release.yml`） |
| O4 | `tools/llama-server.service` | 仓库副本无任何安装方（`ci.sh`/`release.sh`/`.github` 均不引用），且与文档声称的现网值（`MemoryMax`/`CPUAffinity`）不一致 | **单源化**：仓库文件为唯一权威并补齐两项限制，文档改为引用 |
| O5 | `weaknet_get_bluetooth_*` | `test_client.cpp`（集成示例）消费 | **保留并确认**，非孤立 |
| O6 | 模块级 `weaknet-ota-ctl:sync_rescue_flag` | 零调用者（CLI 有独立的 `cmd_sync_rescue_flag`） | **删除**，并加测试锁死两份判定规则一致 |
| O7 | `weaknet-ota-gate --dry-run` | 解析后从不读取，仍返回非零 | **删除** |
| O8 | `BtMonitor::calculateAudioScore` + `friend class ::BtMonitorAudioScoreTest` | 生产头文件为测试开后门 | **提成自由函数**，删友元 |

### 关键决策

1. **两种计数必须分开**。`bump-boot-attempt` 按 boot_id 幂等（进程重启 ≠ 设备重启）；
   `bump-trial-round` **不**幂等（窗口耗尽就是新的一轮）。合成一个就不可能同时正确。
2. **`bump-trial-round` 顺带刷新 `updated_at`**。窗口起点就是它，不刷新则下一轮一进来
   就超时，等于没换窗口。
3. **控制器不可用时 fail-closed 进救援，绝不返回 75**。计数落不下去还请求重启，
   进程重启后内存计数归零 → 无界重启。
4. **`StartLimitIntervalSec` 属于 `[Unit]`**。写进 `[Service]` 被 systemd 当
   `Unknown key name` 静默忽略（只有 `systemd-analyze verify` 给一行 warning）。
   这与 P0-C1 那次 `ConditionPathExists` 写错段位是同类陷阱，已写成测试断言。
5. **`decide()` 的默认参数不能在定义时绑定常量**。`max_rounds=MAX_TRIAL_ROUNDS`
   在定义时求值，之后再改模块常量完全无效——演练里改上限失败就是撞到这里。

### 本机验证

```
x86 全量编译                          → 0 error（2 条 warning 均为既有）
ctest --test-dir build-x86/server     → 53/53 通过（4.6s）
python3 tools/test_weaknet_ota_ctl.py     → Ran 91 tests, OK
python3 tools/test_weaknet_ota_recover.py → Ran 52 tests, OK
python3 tools/test_weaknet_ota_gate.py    → Ran 32 tests, OK
bash tools/ota_p0a_drill.sh           → PASS=44  FAIL=0
bash tools/ota_p0b_drill.sh           → PASS=30  FAIL=0
bash tools/ota_p0c1_drill.sh          → PASS=36  FAIL=0
```

补充检查：`ast.parse(feature_version=(3,9))` 全部通过（板端 Python 3.9.2）；
`bash -n` 四个脚本通过；两个 workflow YAML 可解析；
`systemd-analyze verify` 四个单元无 `Unknown key/section` 告警；
`example_usage` 编译通过。

### 已知的口径修正

- 板端 CPU **是 8× Cortex-A55，没有 A76 大核**（实测 `/proc/cpuinfo` 与
  `scaling_available_frequencies`）。文档旧称"绑定两颗 A76 大核"已改写为
  "固定两颗核、其余留给弱网服务与系统"。

## 结果

### 已完成

- C1（恢复服务静默死亡）两半都修了，并用真实 systemd 做对照实验验证方向
  —— **本机 systemd 255 与板端 systemd 247 双向验证，结论一致**
- I1 时区、I2 派生视图生命周期、I3 死分支、I5 隧道不重启、O6/O7/O8 死代码
- O3 删除会破坏 OTA 布局的遗留部署 workflow
- O4 llama 单元单源化（文档/仓库/板端三处漂移收敛为一处）
- 测试从 86/42/28 增至 91/55/32，演练从 44/21/36 增至 44/30/36
- **板端 3.9 复验**：修掉 2 个只在该版本暴露的真实缺陷（staticmethod 实例打补丁、
  PEP 479 哨兵），并加了 3 项跨版本守护测试；板端全量 91/55/32 + 44/30/36 与本机一致
- **真机全链路演练**：真实 systemd 247 + 真实恢复器，验证「窗口耗尽 → 75 →
  强制重启 → 计数前进 → 新窗口」与「计数耗尽 → 确定性回滚」两条链路

### 遗留（**明确未做，不得宣称完成**）

1. ~~**板端 systemd 247 未复验**~~ → **已完成（2026-11-01 补跑，见下「板端复验」）**
2. ~~**未跑完整 `./tools/ci.sh`**~~ → **已完成（2026-11-01，见下「完整 ci.sh 实跑」）**。
   跑通过程中连续暴露并修复了 3 个部署链路缺陷（D4 暂存区残留、D5 恢复服务抢事务、
   D6 隧道重启自断通道），最终端到端退出码 0、板端测试全绿。
3. **O1 订阅族只做了标注，没有删除**：删除会牵动 `client/test_client.cpp`、
   `client/test_network_quality.cpp` 与 `libweaknet.so` 的 ABI，属独立变更；
   且"删除还是重建发射"需要产品决策（重建必须先有真实订阅者）。
4. ~~**`weaknet-tunnel` 重启的实际效果未验证**~~ → **已在完整 `ci.sh` 中实跑**：
   重启会掐断自身执行通道（预期），通道约 5～50s 自行重建，`ci.sh` 现已等待其
   恢复后再继续。见下「完整 ci.sh 实跑」D6。
5. **O2/O5 是"确认非孤立"而非"新增消费者"**：本次只做了判定，没有改动它们。

### 板端复验（2026-11-01 补跑，P0-C2 剩余项）

板端：Radxa Cubie A7A，aarch64，**systemd 247.3-7+deb11u8**，Python 3.9.2，bash 5.1.4。

#### 1. `RestartForceExitStatus` 的 systemd 247 对照实验

退出码固定 75，用 `systemd-run --property=` 起一次性单元：

| 配置 | `Result` | `NRestarts` | `ActiveState/SubState` |
|---|---|---|---|
| `on-failure` + `SuccessExitStatus=75` | `success` | 0 | `inactive/dead`（静默死亡） |
| `on-failure` + `RestartForceExitStatus=75` | `exit-code` | 3 | `activating/auto-restart` |

与本机 systemd 255 结论**完全一致**，P0-C1 缺陷 C1 的修法在 247 上成立。

#### 2. 真实 systemd + 真实恢复器的全链路演练

沙盒根/状态目录（`/tmp/wn-c1-live`），生产事务零触碰。`--service no-such-service-xyz`
让健康探测必然失败，制造"窗口耗尽"：

| 场景 | 观察结果 | 判定 |
|---|---|---|
| TRIAL + 窗口耗尽 + 计数 0 | `NRestarts=1`；`boot_attempts 0→2`；`updated_at` 刷新 | **确实被强制重启**（旧配置下会静默停在 inactive），且新窗口真的换来下一轮，无重启风暴 |
| TRIAL + 窗口耗尽 + 计数 3（已达上限） | `NRestarts=0`；`phase=ROLLBACK_PENDING`；`current→releases/1.0.0`；`stable_version=1.0.0` | **计数耗尽后确定性回滚，不再请求重启** |

#### 3. 板端发现的真实缺陷（本机 3.12 全部掩盖）

**D1（严重）：`staticmethod` 实例打补丁在 3.9 不可调用**

```python
w.read_boot_id = staticmethod(lambda: "boot-drill")   # 3.9 上 TypeError
```

给**实例**赋值不经过描述符协议，取回来就是包装对象本身。Python 3.10 起
`staticmethod` 对象变为可调用（bpo-43682），本机 3.12 侥幸通过；3.9 直接
`TypeError: 'staticmethod' object is not callable`。板端实测：recover 单测
**8 个用例 error**。该写法 HEAD 中已有 2 处（P0-B 遗留），本次新增 2 处，
共 4 处。修法：直接赋 lambda（实例属性不做绑定，行为等价）。

**D2（严重）：`ota_p0b_drill.sh` 第 9 节在板端整段失效**

```python
sleeper=lambda _s: (_ for _ in ()).throw(StopIteration())   # PEP 479
```

Python 3.9 会把从生成器逸出的 `StopIteration` 转成 `RuntimeError`
（"generator raised StopIteration"），调用方的 `except StopIteration` 抓不到
→ `RC=` 变空串 → 2 项断言失败；Python 3.12 在同场景（同步 `.throw()`）下
不转换 → **本机全绿**。这意味着上一轮"演练 30/30 通过"的结论在板端不成立。
修法：自定义哨兵异常 + 普通函数 `raise`。

**D3（方法论）：`ast.parse(feature_version=(3,9))` 不足以证明 3.9 兼容**

它只校验**语法**，D1/D2 都是运行期语义差异，语法树完全合法。上一轮据此
得出的"板端兼容"结论是错的。补齐方式：把脚本真的上传到板端用 3.9 跑
（本轮做法），并加了两个源码模式守护测试（见下）。

#### 4. 新增守护测试（防止 D1/D2 回流）

`TestInstancePatchIsCallable`（3 项）：

- `test_no_staticmethod_wrapping_on_instance_assignment` — 扫 `.py` 与本文件
  与 `ota_p0b_drill.sh` 的**可执行代码**，禁止 `x.y = staticmethod(...)`；
- `test_no_generator_throw_sentinel` — 禁止 `(… for … in ()).throw(…)` 哨兵；
- `test_descriptor_wrapping_on_instance_is_not_callable_on_39` — 反向确认语义前提。

两个扫描都先剥离 `#` 注释与 `.py` 的 docstring 再匹配——否则守护会被自己的
反例说明误伤（实测踩过：docstring 里写问题写法示例 → 自我误报）。两条都做了
RED 验证（注入问题写法 → 准确报红 → 恢复 → 转绿）。

#### 5. 板端全量结果（与本机完全一致）

```
本机 Python 3.12.3          板端 Python 3.9.2
91 / 55 / 32                91 / 55 / 32        （三套单测）
44 / 30 / 36                44 / 30 / 36        （三套沙盒演练）
```

### 后续方向

- **P0-C2 剩余**：真实物理重启（断电重启）下的收敛验证——本轮用的是
  `systemd-run` 起独立单元，**不是**开机自启链路。
- **P0-C3**：真实断电测试台（用户已明确暂缓）。
- ~~**P0-C4**：完整 `./tools/ci.sh` 部署准入~~ → **已完成（2026-11-01，见下「完整 ci.sh 实跑」）**
- **订阅族处置**：删除或在建好真实订阅者后重建发射（须走 OpenSpec change）。

### 完整 `ci.sh` 实跑（遗留 2 关闭，P0-C4）

在恢复服务 active、事务 COMMITTED 的真实状态下跑完整 `./tools/ci.sh`，
**连续暴露 3 个部署链路缺陷**，全部修复后端到端跑通（退出码 0，板端测试全绿）。

#### D4：暂存区 root 属主残留导致 rsync 恒失败

```
rsync: delete_file: unlink(server/server/log/server_*.log) failed: Permission denied (13)
rsync error: some files/attrs were not transferred (code 23)
```

`.deploy-staging` 是纯中转目录（rsync 灌入 → adopt 搬走），但历史上服务曾把它
当工作目录写日志，留下 `server/server/log/`、`server/logs/`（Oct 22 的 52MB，root 属主）。
radxa 用户的 `rsync --delete` 删不掉 root 文件 → **每次部署都恒败**。
原脚本只清了 `__pycache__`，属打地鼠。

修法：部署前 `sudo rm -rf` 整个暂存区（含注释说明为何必须整目录）。

#### D5：恢复服务与 adopt 抢同一事务（**最严重**）

板端 journal 实锤：

```
13:45:41 weaknet-ota-recover  判定 action=commit（试运行版本健康，转正）
13:45:41 weaknet-ota-recover  试运行版本已转正（COMMITTED）   ← 它先 commit 了
         ci.sh                健康窗口通过
         ci.sh                commit → 被拒（当前阶段 COMMITTED 不允许该操作，要求 TRIAL）rc=3
         ci.sh                ❌ adopt 失败 → 跳过后续全部步骤
```

恢复服务是常驻轮询（30s），看到 TRIAL + 健康就自动 commit——这是 P0-B 的**正确
设计**（开机自愈），但与 adopt 的「翻转→健康窗口→commit」是同一件事。二者抢
同一事务时恢复服务先赢，adopt 就撞上"非法阶段转换"。**部署其实成功了**，
ci.sh 却误报失败并跳过隧道重启与板端测试。

修法（两半）：
1. **部署期间停掉恢复服务**，结束再拉起。必须配 `trap ... EXIT INT TERM`——
   `fail()` 是 `exit 1`，中途失败若不兜底会把恢复服务**永久停着**，下次真出事
   就没人自愈。
2. **放宽 adopt 错误判定**：返回非零时用客观事实复核（`phase=COMMITTED`
   ＋ `releases/<ver>/.committed` 存在 ＋ 服务 active），三条都满足则降级为告警
   并**继续后续步骤**，不做"假失败"。

#### D6：隧道收尾重启掐断自己的执行通道

```
Connection to 127.0.0.1 closed by remote host.
⚠️  反向隧道重启失败——板端 SSH 反向通道可能仍用旧配置
⚠️  weaknet-test-full.sh 上传失败
ssh: connect to host 127.0.0.1 port 2222: Connection refused
❌ 开发板测试失败（退出码 255）
```

两处问题叠加：
1. **自指断连是设计必然**：`systemctl restart weaknet-tunnel` 的命令经反向隧道
   下发，重启隧道 = 断掉承载本次连接的通道。ssh 因此返回 255，但这**不是失败信号**。
   改用「重启后通道能否恢复」作准据（下方轮询），该行只记 `info` 不告警。
2. **重启后没等通道恢复**：单元是 `Restart=always` + `RestartSec=3` +
   `-R 2222:localhost:22`，云端 sshd 要重新绑定端口，实测约 **50s**。原先重启完
   立刻往下走，紧接的 scp 与板端测试全撞在断链窗口里。
   修法：轮询等待，每 5s 探一次、总预算 90s，**判据用完整 SSH 握手**而非只看
   端口 listen（端口早被云端 sshd 监听着，但转发链路要等会话建立才通，只看端口
   会得到"假可用"）。

#### 修复后的端到端结果

```
✅ 编译 + 打包完成
✅ 恢复服务已暂停（部署期间由 ci.sh 独占 OTA 事务）
✅ manifest.sha256 全量校验通过（24 个文件）
✅ 健康窗口通过（连续 3 次探针全绿）
✅ 升级完成并转正：releases/1.1.0
✅ 恢复服务已恢复运行
ℹ️  反向隧道已断开（预期：重启隧道必然掐断本次连接），等待其自行重建
✅ 反向通道已恢复（等待 5s）
✅ 部署完成
✅ 测试完成（健康检查 / 网络接口 / 库初始化 / 5 次 API 调用全通过）
✅ 全部完成
```

#### 板端生产环境最终状态

```
weaknet-ota-recover.service : RestartForceExitStatus=75   （C1 修复已上板）
/usr/local/lib/weaknet/weaknet-ota-recover : 含 bump-trial-round（3 处），14:04:51
weaknet-server / weaknet-tunnel / weaknet-ota-recover : active
weaknet-ota-gate : inactive（oneshot + RemainAfterExit=no，设计如此）
OTA 事务：phase=COMMITTED, stable_version=1.1.0, revision=57
启动门禁：allow=true rc=0
恢复器 --once：action=none（事务已结束）
```

### 一条操作性教训

本轮第一次用生产控制器跑沙盒演练时，恢复器的健康探测连到了**真实**
`weaknet-server` 并判定 healthy=True，于是把沙盒事务推进到 COMMITTED。
生产事务未受影响（`revision` 仍 30、`transaction_id` 未变），但说明：
**在真机上演练"探测失败"场景时，必须用 `--service` 指向不存在单元**，
不能只依赖沙盒根目录——探测走的是系统总线上的真实 D-Bus 服务。

---

## 附：工作区里的非本批次改动

`server/src/looper.cpp` 在本工作区存在一份**不属于本批次**的未提交修改
（D-Bus 派发循环，mtime 17:47，早于本批次任何改动）。多个会话共享同一
worktree 时会出现这种情况，本批次未触碰该文件，也未将其纳入任何验证结论。

# OTA P0-C1 救援模式持久化启动门禁与 systemd 启动顺序加固

> 日期：2026-10-29
> 分类：工作流外实施（承接 [[2026-10-22-ota-p0b-boot-recovery]]，属 P0-C 序列第一项）
> 结果：已完成（门禁 + 接线 + 单测 28 项 + 流程兼容演练 36 项 + 真机 P0-C2 重启矩阵）

## 构思

### 要解决什么

P0-B 交付的救援模式有一个**可绕过性缺口**：`systemctl stop` + `mask`/`disable`
只挡住 systemd 的**自动拉起**。任何人手动 `systemctl start weaknet-server`、
或者别的 unit 用 `Wants=` 拉它，救援态就被绕过——设备会跑起一个未经验证
（甚至已被判死刑）的版本，而事务状态还停在 `RECOVERY_REQUIRED`。

用户给的 P0-C1 判据是三件事：

1. 救援模式下无论正常重启还是手动启动业务服务，都**不会绕过保护**
2. 恢复器必须在业务服务**启动前完成判定**
3. 故障情况下 **SSH 救援通道仍然独立可用**

### 为什么不做进 OpenSpec change

交付面是板端启动闸门与运维接口：不新增对外协议、不改云端契约、不新增服务端
DBus surface。与 P0-A / P0-B 同构，按 `docs/ai/implementation-economy.md`
判据走工作流外实施并在此留记录。

### 先做实验再设计（这次的关键）

用户明确要求「**不建议未经审查就直接修改服务依赖**」。所以先在一台真板上把
systemd 247 的各种"看起来像门禁"的写法逐个试了一遍，用结果反推设计：

| 候选写法 | 板端实测 | 结论 |
|---------|---------|------|
| `ExecStartPre=` 检查脚本 | 失败 → `Restart=on-failure` **重启循环**（NRestarts=3），加 `RestartPreventExitStatus` 也拦不住 | ❌ 拒绝启动会变成烧电循环 |
| `ExecCondition=` 检查脚本 | 跳过执行、`Result=success`、无风暴；但自动重启时会**重跑** | △ 语义是"跳过"不是"禁止"，且无独立审计单元 |
| `Wants=` + `After=` | 依赖失败时业务服务**照样启动**（CONSUMER-RAN） | ❌ 完全不是门禁 |
| 门禁单元内写 `ConditionPathExists=!flag` | 被 Condition 跳过的单元对 `Requires` 而言**不算失败** → 业务服务照样起 | ❌ 单靠 Condition 不是门禁 |
| `Requires=` + `After=` + 独立 oneshot 门禁单元 | 门禁非零退出 → 业务单元 `rc=1` / `Result=dependency` / 进程零次执行 / `NRestarts=0` | ✅ **采用** |

一个附带发现：`ConditionPathExists` 写进 `[Service]` 段会被 systemd 当
"Unknown key name" 忽略（我第一次的探针 unit 写错了，白测一轮）——必须写
`[Unit]` 段。

### 救援标记的定位（用户明确的约束）

用户要求「可以增加持久化 `rescue.flag` 作为额外保护，**但不能让这个标记变成
另一套相互矛盾的权威状态**」。落实为三条硬规矩：

1. **权威永远是 `transaction.json`**。门禁本体 `weaknet-ota-gate` **不读**
   `rescue.flag`（单测两个方向锁死：只有 flag 不放行也不拒绝；只有救援态
   没 flag 也拒绝）。
2. **flag 只作为第二层廉价防线**给 `weaknet-server` 的
   `ConditionPathExists=!` 用（systemd 读不了 JSON，只能看存在性）。
3. **投影可以陈旧，但修正方向永远朝"更安全"**：进入救援 → 先写 flag 再落
   状态；离开救援 → 先落状态再清 flag。任意中途失败都落在"多挡一次"。
   恢复器每轮判定前调 `sync_rescue_flag()` 修复投影并留 `rescue.flag.stale`。

### 备选方案与取舍

| 方案 | 取舍 |
|------|------|
| A. 在 `weaknet-server` 单元里塞 `ExecStartPre` + `RestartPreventExitStatus` | ❌ 实测仍有重启循环；且判定逻辑与业务单元耦合，改业务单元会误伤门禁 |
| B. 只加 `ConditionPathExists=!rescue.flag` | ❌ flag 缺失/陈旧即失守；单测能证明"标记不是权威" |
| C. 独立 oneshot 门禁单元 + `Requires=` + 第二层 Condition（**选中**） | ✅ 权威判定与廉价防线分离；失败方向零重启风暴；门禁自身可单测、可独立排障 |
| D. 改成 socket/target 依赖编排 | △ 能行，但改动面大、对 systemd 247 行为需要更多实验，收益不足 |

## 实现方式

### 改动文件

| 文件 | 性质 | 内容 |
|------|------|------|
| `tools/weaknet-ota-gate` | 新增 | 门禁本体（~200 行纯标准库）：`read_state()` fail-closed 读状态、`decide()` 纯函数判定、CLI 输出 JSON 判定 + 人读日志 |
| `tools/weaknet-ota-gate.service` | 新增 | `Type=oneshot`（**无** `RemainAfterExit`，故每次业务启动/崩溃重启都重跑）、`TimeoutStartSec=15`、`NoNewPrivileges`、`ProtectSystem=strict`、`ReadOnlyPaths=/var/lib/weaknet-ota` |
| `tools/weaknet-server.service` | 修改 | `[Unit]` 增加 `Requires=`/`After=weaknet-ota-gate.service` + `ConditionPathExists=!/var/lib/weaknet-ota/rescue.flag`，并写清为什么必须是 `Requires` 而不是 `Wants` |
| `tools/weaknet-ota-ctl` | 修改 | 新增 `enter-rescue` / `sync-rescue-flag`；`require-recovery`、`commit`、`abort`、`recover`、`bootstrap` 全部按"先标记后状态 / 先状态后清标记"的顺序维护 flag；`clear_rescue_flag()` 连带清除 `RESCUE.md`（同为派生视图） |
| `tools/weaknet-ota-recover` | 修改 | `enter_rescue()` 先写 flag 再 `ctl enter-rescue`；`run_once()` 每轮先 `sync_rescue_flag()` 修投影；`RESCUE.md` 增加门禁状态段与修正后的退出步骤 |
| `tools/weaknet-ota-gate` 的 `decide()` | 修订 | 第一版把活跃阶段一律拒绝（见下文"第一版设计是错的"），收窄为只拒绝 `RECOVERY_REQUIRED` / 状态不可信 / 未登记阶段 |
| `tools/test_weaknet_ota_gate.py` | 新增 | 28 项：`decide()` 矩阵、`read_state()` 错误路径、"门禁不读 flag"两方向、CLI 退出码、**systemd 接线断言**（Requires 不是 Wants / oneshot 无 RemainAfterExit / 门禁不依赖 weaknet-server / 门禁单元自身不靠 Condition） |
| `tools/test_weaknet_ota_ctl.py` | 修改 | 新增 `TestRescueFlag`（12 项）：flag 的写/清/同步时序、陈旧修复、`RESCUE.md` 生命周期 |
| `tools/test_weaknet_ota_recover.py` | 修改 | 新增 P0-C2 重启矩阵（7 项）与 `TestGateConsistency`（3 项）：回滚续跑、崩溃循环救援、尝试耗尽回滚、同 boot_id 不重复计数、门禁与恢复器阶段词表一致 |
| `tools/ota_p0c1_drill.sh` | 新增 | 36 项断言，3 节：阶段矩阵、升级全流程重放（每个活跃阶段门禁必须放行）、救援链。支持 `OTA_GATE_BIN` 做变异验证 |
| `tools/ci.sh` | 修改 | 部署顺序调整：门禁单元必须先于带 `Requires` 的 `weaknet-server` 单元落盘，否则 `daemon-reload` 后业务单元因依赖单元不存在而起不来 |
| `.github/workflows/ci.yml` | 修改 | 门禁单测 + 流程兼容演练 |
| `docs/OTA固件更新与安全发布指南.md` | 修改 | v1.4：新增 §1.5（门禁设计、判定表、四方案实测对比、活跃阶段必须放行的理由、人工出口），§5.1 补门禁自测与演练，§5.2 补部署顺序，§1.4 的退出步骤补 `enable` 与 `RESCUE.md` 生命周期 |
| `CLAUDE.md` / `docs/架构设计-模块与代码结构.md` | 修改 | 意图表补 2 条；目录树补门禁与其单元 |

### 关键决策

1. **门禁不读 flag，只读权威状态**。flag 是投影，读它就等于承认两套权威。
   单测用两个反方向锁死这一点。
2. **门禁自身必须是一个独立单元**，不能挂在业务单元的 `ExecStartPre` 上。
   实测 `ExecStartPre` 失败会进重启循环；独立单元 + `Requires` 不会。
3. **`Requires` 不是 `Wants`**。这条在板端实测过（`Wants` 时业务服务照样起），
   已写成单测断言，防止未来"简化"回退。
4. **门禁不依赖 weaknet-server / weaknet-ota-recover**。它只看状态文件，
   保证恢复器挂了门禁仍然有效（单测锁死）。
5. **`read_state()` 的三态区分**：文件不存在（`(None, None)`，全新设备放行）
   vs 读到了 vs 读不出来（`(None, str)`，必须拒绝）。坏了就拒绝，不猜。
6. **`ci.sh` 的落盘顺序**：门禁单元 → `weaknet-server` 单元。反了就是
   "依赖单元不存在"，新部署的设备直接起不来。

### 第一版设计是错的：门禁把升级流程掐死了

第一版把 **所有非终态一律拒绝**，包括 `TRIAL` 与 `ROLLBACK_VERIFY`。
单测 27 项全绿——因为期望值就是那么写的。但真机上立刻暴露：

```
do-switch → TRIAL
systemctl restart weaknet-server   → rc=1 "A dependency job ... failed"
gate: 事务处于 TRIAL：升级尚未结束，版本未经验证，拒绝启动
```

升级脚本的流程是 `arm-switch → do-switch → restart → health_window`，
而健康窗口的判据是 `systemctl is-active` + D-Bus `HealthCheck` +
`GetEbpfMonitorHealth` + `GetUpdateStatus.installed_version`——**服务不在线就
根本无法判定"已验证"**。于是：

- 健康窗口必然超时 → 自动回滚 → 回滚的 restart 又被拒 → 升级 100% 失败
- 回滚验证同样永远无法通过 → 每次回滚直接掉进救援

`ROLLBACK_VERIFY` 期间重启也实测被挡（`rc=1`），确认不是 TRIAL 特例。

**与用户的原始口径对齐**：用户的设计说明是"处于救援状态**或状态不可信**时
拒绝启动"，这句话本身就把拒绝集合限定在"不可运行"上，而不是"没跑完"。
按用户确认的口径收窄后：

| 权威状态 | 判定 |
|---|---|
| 无事务 / 无进行中事务 / 终态 | 允许 |
| 活跃阶段（PREPARED / SWITCH_PENDING / TRIAL / ROLLBACK_PENDING / ROLLBACK_VERIFY） | **允许**（健康验证依赖服务在线） |
| RECOVERY_REQUIRED / 状态损坏 / schema 不认识 / phase 缺失 / 未登记阶段 | **拒绝** |

fail-closed 仍然成立，只是拒绝集合是"**不确定能不能跑**"而不是"没跑完"。
活跃阶段的收敛由 `boot_attempts` 持久计数 + 恢复器负责，"有限次数内结束"
是 P0-B / P0-C2 的职责。

**这个教训写进了三层防回归：**

1. `tools/ota_p0c1_drill.sh`（36 项断言）：把门禁接到**真实阶段推进序列**上
   重放升级与回滚全流程，每个活跃阶段都必须放行。
2. 门禁单测里 `test_live_phases_allow` 的 docstring 明确写"不允许为了看起来
   更严格改回去"。
3. `test_weaknet_ota_recover.py::TestGateConsistency` 断言恢复器判断为
   "继续推进"的阶段门禁必须放行，且两边的阶段词表必须一致。

演练本身也做了变异验证：把门禁改成"活跃阶段一律拒绝"后用
`OTA_GATE_BIN=` 跑，演练 `PASS=24 FAIL=12`、退出码 1；用真门禁跑
`PASS=36 FAIL=0`、退出码 0。这证明演练不是摆设。

### 真机演练暴露的第二个缺陷：陈旧 RESCUE.md

`RESCUE.md` 之前只在恢复器进入救援时写、**从不删除**。退出救援后它留在
`/var/lib/weaknet-ota/`，下一次真进救援时日志里重放的是**上一份**现场报告
（旧版本号、旧触发原因、旧时间戳），会把运维引向错误的判断。

修法：`RESCUE.md` 与 `rescue.flag` 同属救援态的**派生视图**，由同一个
`clear_rescue_flag()` 在**状态落盘之后**一起清除。板端验证：
`require-recovery → RESCUE.md 存在 → recover --accept → RESCUE.md 已清除`。
三条单测锁死（recover 清、sync 清、仍在救援时不清）。

### 真机演练暴露的第三个缺陷（文档类）

| 缺陷 | 根因 | 修法 |
|------|------|------|
| `recover --accept` 之后服务只能手动 `start`，重启又没了 | 恢复器救援时 `mask` 失败退到 `disable`；`recover --accept` 是**状态机**，不动 systemd 的 enable/disable 状态 | 文档与 `RESCUE.md` 的人工恢复步骤补 `systemctl enable weaknet-server`，并把"顺序不能反"写成显式告诫。这是**文档缺陷而非代码缺陷**：让状态机去管引导状态是越界，正确修法是让操作步骤完整 |

（演练中还发现板端 `weaknet-server` 停在 `disabled` 状态——正是上面这条路径的
残留。最终已恢复为 `enabled`。）

### 验证手段

**本地（x86 宿主机）**

```
python3 tools/test_weaknet_ota_ctl.py       → Ran 86 tests, OK
python3 tools/test_weaknet_ota_recover.py   → Ran 42 tests, OK
python3 tools/test_weaknet_ota_gate.py      → Ran 28 tests, OK
bash tools/ota_p0a_drill.sh                 → PASS=44  FAIL=0
bash tools/ota_p0b_drill.sh                 → PASS=21  FAIL=0
bash tools/ota_p0c1_drill.sh                → PASS=36  FAIL=0
# 变异验证（故意改坏的门禁）：PASS=24 FAIL=12，退出码 1
```

Python 3.9 语法兼容性用 `ast.parse(..., feature_version=(3,9))` 逐个校验通过。

**板端（Radxa Cubie A7A，aarch64，systemd 247，Python 3.9.2）**

前置：`weaknet-ota-gate` / `weaknet-ota-ctl` / `weaknet-ota-gate.service` /
`weaknet-server.service` 与仓库 sha256 一致；事务基线 `COMMITTED / stable 1.1.0
/ previous 1.0.0` 备份为 `tx.baseline`（演练后已删除）。

**证明一：救援模式不能被绕过**

```
# 进入救援：controller require-recovery → RECOVERY_REQUIRED + rescue.flag 生成
sudo systemctl start weaknet-server
  → rc=1  "A dependency job for weaknet-server.service failed"
  → server=inactive / pgrep none / NRestarts=0

# 删掉 rescue.flag，只剩权威门禁那一层
sudo rm -f /var/lib/weaknet-ota/rescue.flag
sudo systemctl start weaknet-server
  → rc=1 同样失败
  → gate journal: {"allow": false, "phase": "RECOVERY_REQUIRED", ...}
    gate: Main process exited code=exited status=1/FAILURE
  → server=inactive / pgrep none / NRestarts=0
```

第二段是关键：**证明挡人的不是标记，而是门禁读的权威状态**。

**证明二：门禁必须先于业务服务完成判定，且每次启动都重跑**

```
系统启动时 journal 顺序：gate "放行" 与 server "Started" 同一秒（gate 在前）
sudo kill -9 <MainPID>
  → gate "Finished" 计数 4 → 5，server 重新 active，NRestarts=1
  → 崩溃后的自动重启**重新经过门禁**
```

**证明三：SSH 救援通道独立可用**

救援态全程 `systemctl is-active ssh` = `active`、`weaknet-tunnel` = `active`、
`weaknet-ota-recover.service` = `active`。门禁只挡 `weaknet-server`，不碰任何
救援通道（单元依赖图上无交集）。

**P0-C2 重启矩阵（用户授权的真机演练，按验收表逐行）**

| 场景 | 板端结果 |
|---|---|
| `TRIAL` 期间 restart | 修复后 `rc=0`，server `active`，gate 日志 `放行：事务处于 TRIAL…` |
| `ROLLBACK_VERIFY` 期间 restart | `rc=0`，server `active`，`current → releases/1.0.0`，随后 `rolled-back` 成功 |
| `RECOVERY_REQUIRED` 期间 restart / start | `rc=1`，server `inactive`，进程零次执行，`NRestarts=0`；ssh/tunnel `active` |
| 删掉 `rescue.flag` 后 start | 仍 `rc=1`，gate 日志 `{"allow": false, "phase": "RECOVERY_REQUIRED"}` |
| 人工 `recover --accept 1.1.0` | 门禁解除，flag 与 RESCUE.md 均清除，`stable=1.1.0 / previous=1.0.0` 不丢 |
| 恢复器自身不可用（stop） | 门禁独立生效：gate 单独跑 `rc=1`，start `rc=1`，server `inactive`，ssh/tunnel `active` |
| 同一 `boot_id` 内重复 bump | 第二次返回 `本次开机已计数（幂等）`，`boot_attempts` 不增长 |
| 换 `boot_id`（模拟真重启） | `boot_attempts` 1 → 2 |
| 连续重启超过试运行次数 + 候选不健康 | `TRIAL → ROLLBACK_PENDING → ROLLBACK_VERIFY → ROLLED_BACK`，`current` 回 1.0.0，server `active`；日志 `判定：action=rollback（试运行已尝试 4 次仍不健康，回滚）` |
| 候选健康 + 尝试次数已满 | `commit`（健康优先于次数，符合设计） |

**撤退与基线恢复**

```
cp tx.baseline → 恢复 COMMITTED / 1.1.0
rm tx.baseline、history/*drill*、RESCUE.md（演练残留）
```

最终板端状态：`weaknet-server` / `weaknet-ota-gate` / `weaknet-ota-recover` 三个
单元全部 `enabled`，服务 `active`，`readlink current` = `releases/1.1.0`，
`weaknet-cli update status` = `installed_version 1.1.0, current_committed true`，
`/var/lib/weaknet-ota/` 只剩 `lock` 与 `transaction.json`（无 flag、无 RESCUE.md、
无演练历史），`/tmp` 无残留探针 unit。

## 结果

### 已完成

- 门禁本体 + systemd 单元 + 业务单元接线 + 部署链路 + 单测 + 流程兼容演练 + 文档
- 三项 P0-C1 判据都在真机上拿到可复现证据（见上），其中"删掉 flag 仍然拦住"
  直接证明了权威状态而非标记在起作用
- P0-C2 验收矩阵中不依赖真实断电的 10 行已在真机逐行跑过（见上表）
- 设计依据是先做 systemd 247 行为实验再定，四种候选写法的实测结果已写进
  代码注释、门禁 docstring、指南 §1.5 和单测断言，避免未来回退
- 修掉一个会毁掉整个 OTA 的设计错误（活跃阶段拒绝启动），并加了"必须重放
  真实流程"的演练作为防回归，演练本身做过变异验证

### 遗留（**明确未做，不得宣称完成**）

1. **断电场景仍未验证**：全部"故障"都是用状态构造 + `kill -9` / `systemctl
   stop` 制造的，**不等于掉电**。P0-B 遗留的 fsync/写缓存边界依然成立。
   用户已明确暂缓真实硬断电（P0-C3）。
2. **未做真实物理重启**：矩阵里"换 boot_id"是构造的，不是真按电源/`reboot`。
   真实重启还牵涉 systemd 开机顺序、`network-online.target` 时序等未覆盖因素。
3. **未跑完整 `./tools/ci.sh`**（用户明确暂缓）：本次部署是分别 scp + `install`
   到位，`ci.sh` 里新增的"门禁先于业务单元落盘"顺序**未在真实部署流程中跑过**，
   只做了 `bash -n` 语法校验。这是 P0-C4 的内容。
4. **未做完整功能回归**：救援 → 退出救援之后只验证了 `is-active` 与版本基线，
   没跑 `weaknet-test-full.sh`。
5. **门禁的"状态文件存在但事务为 null"分支放行**：按"上次升级已完全结束"
   的语义设计，但如果有人手工把 `transaction` 洗成 null，门禁会放行。当前
   依赖"控制器是唯一写者"这一约定，未做防篡改。
6. **活跃阶段的收敛依赖计数与恢复器**：门禁不再兜底"TRIAL 卡死"这类情况。
   这是用户确认的取舍——避免门禁猜错把设备变成砖。

### 后续方向

- **P0-C2 剩余**：真实物理重启（`reboot`）后的状态一致性验证。
- **P0-C3**：真实断电测试台（用户已明确暂缓）。
- **P0-C4**：完整 `./tools/ci.sh` 部署准入。

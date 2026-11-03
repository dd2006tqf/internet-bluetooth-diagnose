# P0-C2 真实重启收敛验证

> 日期：2026-11-03
> 分类：工作流外实施（真机验证，无代码改动）
> 结果：已完成

## 构思

### 要解决什么问题

P0-C2 验收矩阵中唯一剩下的一行，离线与 `systemd-run` 都无法证明的事：

> **真实物理重启（`reboot`）后，TRIAL 事务的尝试计数与收敛判定是否仍然正确。**

此前 `openspec/tasks/2026-11-01-ota-convergence-and-orphan-interface-fixes.md`
的「后续方向」明确写着：本轮用的是 `systemd-run` 起独立单元，**不是开机自启链路**。
也就是说，「恢复器在真实开机时被 systemd 拉起」「`boot_attempts` 真的跨
boot_id 持久累加」这两件事一直没有证据。

### 为什么不做进 OpenSpec change

本次不修改任何产品代码、不改变任何行为契约，只是对既有实现做真机取证。
OpenSpec change 的前提是「先规划、后实现、再记录证据」，且 fingerprint 机制
要求 evidence 对应真实的代码改动。纯验证任务套用 change 流程会得到 footprint
为 0 的退化情况（见 CLAUDE.md「代码已实现时不套用 OpenSpec change 流程追认」）。
因此按规则记入 `openspec/tasks/`。

### 备选方案与取舍

| 方案 | 取舍 |
|---|---|
| 生产路径直接构造 TRIAL 后重启 | 覆盖最真实，但中途失败会让生产板停在 TRIAL/救援态，需人工介入。**否决** |
| 沙盒 + `systemd-run` 一次性单元 | 这是 11-01 已经做过的，**证明不了开机自启链路**。否决 |
| 沙盒 + 真实开机自启单元 + 真实 `reboot` | 隔离生产、又走真实开机链路。**采纳** |

沙盒隔离边界：独立 root（`/var/lib/p0c2-sb/root`）、独立 state-dir
（`/var/lib/p0c2-sb/state`）、`--service` 指向一个**不存在的服务名**
（`weaknet-p0c2-absent`），因此健康判定恒为 `False`，事务稳定停在 TRIAL，
不会提前 commit。

## 实现方式

### 前两版沙盒为何作废（记录以免重踩）

1. **v1 建在 `/tmp/p0c2-sb`** —— 板端 `/usr/lib/tmpfiles.d/tmp.conf` 有
   `D /tmp` 规则，开机时整个目录被清空。重启后沙盒连同事务一起消失。
2. **v2 只装了 `weaknet-p0c2-probe`（`sleep infinity`）** —— 没有任何开机
   自启的 gate/recover 单元；生产的 gate/recover 指向 `/home/radxa/weaknet`，
   所以真重启后沙盒根本不会被执行。它验证的只是「探针服务开机起来了」。
3. **v1 试过复用生产 D-Bus 做健康探测** —— 生产 `GetUpdateStatus` 报
   `installed_version=1.1.0`，与沙盒 `to_version` 相同，恢复器一启动就判
   「健康」并 `commit`，TRIAL 在重启前就结束了。

### 最终方案（v3）

落到持久目录 `/var/lib/p0c2-sb`，只装一个开机自启单元：

```
weaknet-p0c2-recover.service
  ExecStart=/usr/bin/python3 /usr/local/lib/p0c2-sb/recover \
      --root /var/lib/p0c2-sb/root \
      --state-dir /var/lib/p0c2-sb/state \
      --ctl /usr/local/lib/p0c2-sb/ctl \
      --service weaknet-p0c2-absent
  Restart=on-failure
  RestartForceExitStatus=75      # 与生产同语义
```

只装恢复器、不装 gate/biz：本次要验的是「开机自启 + 计数跨 boot 持久化 +
TRIAL 收敛」，门禁接线已由 P0-C1 真机覆盖，少一个变量少一份风险。

### 执行步骤

1. 造 TRIAL 事务：`begin --from 1.0.0 --to 1.1.0 --rollback-target 1.0.0
   --policy '{"health_timeout": 600, "health_poll_s": 10}'`
   （600s 长窗口，确保重启时事务仍是 TRIAL）
2. 启动恢复器，记录重启前基线
3. `sudo systemd-run --on-active=2 --unit=p0c2-reboot2 /bin/systemctl reboot`
4. 等待隧道重建，核对重启后状态

### 关键参数选择

`health_timeout` 必须**长于**「启动恢复器 → 执行重启 → 板子回来」的耗时。
第一次尝试用了 60s，结果恢复器在重启前就耗尽了窗口，连续 `bump-trial-round`
把 `boot_attempts` 推到 3 并转入 `RECOVERY_REQUIRED`，重启发生时事务已不是
TRIAL —— 那次只验证了「重启后恢复器被拉起并对终态执行正确动作」，
**没有**验证计数跨 boot 累加。第二次改为 600s 才命中目标场景。

## 结果

### 验证证据

boot 序列（`journalctl --list-boots`）：

```
-2 e78f6b9a… Tue 07:27 — 09:57    （首次尝试）
-1 befde079… Tue 09:57 — 10:04    （第二次尝试的重启前）
 0 a5819368… Tue 10:04 —          （第二次尝试的重启后）
```

**重启前**（boot `befde079`）：

```
boot_attempts = 1
phase         = TRIAL
current       -> releases/1.1.0
恢复器日志：boot_attempts 已加一（boot_id=befde079-14cf-4aa2-9cb5-616fe0631031）
            判定：action=wait_trial … attempts=1
```

**重启后**（boot `a5819368`，由开机自启拉起）：

```
恢复器 systemctl is-active = active
恢复器日志：Started P0-C2 sandbox OTA recovery
            boot_attempts 已加一（boot_id=a5819368-4ac2-4fc1-ac74-673c20bff8bb）
            判定：action=wait_trial … attempts=2
boot_attempts = 2
phase         = TRIAL        ← 仍在 TRIAL，未提前 commit/rollback
current       -> releases/1.1.0
```

逐条对照：

| 断言 | 结果 |
|---|---|
| 恢复器在真实开机时被 systemd 自动拉起 | ✅ `active`，`-b` 本 boot 有完整日志 |
| boot_id 真的变了（证明是物理重启而非进程重启） | ✅ `befde079…` → `a5819368…` |
| `boot_attempts` 跨 boot_id 持久累加 | ✅ `1 → 2`，日志记录新 boot_id |
| 重启后事务仍处于 TRIAL 并继续正常判定 | ✅ `action=wait_trial`，未误判 |
| 沙盒跨重启存活（`/var/lib` 不被清理） | ✅ 目录与 `transaction.json` 均存活 |
| 生产零影响 | ✅ 见下 |

**生产状态复核**（两次重启前后均核对）：

```
weaknet-server / weaknet-tunnel / ssh / dbus   全部 active
weaknet-server / weaknet-ota-gate / weaknet-ota-recover / weaknet-tunnel  全部 enabled
OTA 事务：phase=COMMITTED, current=1.1.0, stable_version=1.1.0
/var/lib/weaknet-ota/：仅 history、lock、transaction.json（无 rescue.flag）
```

### 清理

沙盒已完全移除：`weaknet-p0c2-recover.service` 已 stop + disable + 删除，
`/var/lib/p0c2-sb`、`/usr/local/lib/p0c2-sb`、`/tmp/p0c2-*.sh` 均已删除，
`systemctl list-unit-files | grep p0c2` 无输出。

### 首次尝试暴露的方法论问题

第一次跑（60s 窗口）有一个**必须记录的教训**：沙盒恢复器的
`enter_rescue()` 内部会调用真实 `systemctl`：

- `service_stop_and_mask()` 对 `--service` 指定的名字执行 `stop` + `mask`
- `ensure_rescue_access()` 检查/启动**真实的** `ssh` 与 `weaknet-tunnel`

因为 `--service` 指向不存在的 `weaknet-p0c2-absent`，stop/mask 落在空处，
没有实际危害；但那一次清理后发现 `weaknet-p0c2-absent.service` 处于
`masked` 状态，需要显式 `unmask` 才能彻底清干净。**结论：沙盒恢复器
并非完全隔离 —— 它的救援路径会接触真实 systemd 单元名。** 后续若再做类似
沙盒验证，应在收尾清单里加上 `systemctl unmask` 与残留单元检查。

### 遗留问题

- **真实硬断电仍未验证**（P0-C3，用户已明确暂缓）。本次验证的是
  `reboot`（有序关机 + 正常开机），不是拔电。`fsync` 与根分区
  `/dev/sda3` 写缓存的边界依然成立，**不得宣称断电自愈已完成**。
- 本次未对沙盒跑完整 `weaknet-test-full.sh`（沙盒内没有真实业务进程，
  该测试无对象可跑）。

### 后续

P0-C2 剩余项至此关闭。P0 系列只剩 P0-C3（真实断电测试台，用户暂缓）。

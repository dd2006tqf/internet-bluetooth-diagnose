# `ssh -N` 成功时零日志，「日志静默」无法区分健康与死亡

> 发现于：2026-11-18
> 位置：`tools/weaknet-tunnel.service` 的 `ExecStart`；板端 `/etc/systemd/system/weaknet-tunnel.service`
> 影响：把 `Restart=always` 当成「隧道会自己恢复」的保证，实际漏掉了半死状态；排查时又把「日志静默」误读成「systemd 放弃了重启」

## 现象

板端的 `weaknet-tunnel.service` 长期用 `journalctl` 看不到任何输出：

```
$ sudo journalctl -u weaknet-tunnel -n 20
-- No entries --            # 或者只有十几小时前的一批连接失败
```

同期云端 `ssh board` 连不上，持续数小时，只能人工重启。

自然的推论是「systemd 放弃重启了」。我按这个方向查了很久，
甚至算出了 `StartLimitBurst=5 / StartLimitIntervalSec=10s` 配 `RestartSec=3s`
的组合**永远凑不满 5 次**（10s 窗口里只会有 3s、6s、9s 三次），
所以「start-limit 触发」在算术上就不可能——全历史 grep
`start-limit` / `Start request repeated` / `repeated too quickly` 也确实 0 条。

推翻了 start-limit 之后，真正的原因才显出来。

## 成因

`ssh -N` 的意思是「**不执行远程命令**」。它建立完连接、申请完
`-R` 转发之后，就只在后台维持这条连接，**没有任何 stdout/stderr 输出**。

于是：

| 状态 | journal 输出 | 单元状态 |
| --- | --- | --- |
| 连接建立、转发正常 | **无** | active (running) |
| 进程在、TCP ESTAB、转发通道已死 | **无** | active (running) |
| 正在 `activating`（等内核 TCP 超时） | **无** | activating |
| 进程退出 | 有（下一轮错误） | activating → active |
| 永久失败（真的限流/配置错） | 有 | failed |

**前三行完全无法区分。** 而第一行和第三行恰恰是最需要区分的两种。

更糟的是第三行的时间尺度：WiFi「关联上了但上行黑洞」时，`ssh`
会挂在内核 TCP 建连超时上（`tcp_retries2=15`，实测数分钟）。
这段时间单元一直是 `activating`，`Restart=always` 帮不上忙
（进程没退出，没有「重启」可言），journal 也一行都不写。

所以「日志静默」不是证据，它是**空集**——它同时兼容「健康」「半死」「卡在建连」。
拿它当「systemd 放弃了」的依据，是把缺席当成了证据。

## 修法

观测面不能靠 ssh 自己产生，必须**由外部主动探测**。

板端新增 `tools/tunnel_probe.sh` + `.timer`（每 2 分钟），主动走完整条
链路读回**板端自己**的 sshd banner。这条判据在健康时打出

```
[tunnel-probe] 健康（banner: SSH-2.0-OpenSSH_8.4p1 Debian-5+deb11u6）
```

不健康时打出具体故障（`stdio forwarding failed` / `refused` / 超时），
**两种状态输出不同**。同时 `weaknet-tunnel.service` 加了
`ConnectTimeout=10`，把「卡在建连」从数分钟压到 10 秒，
让 `Restart=always` 真正有机会介入。

云端侧 `tools/tunnel_guard.sh` 早在 2026-11-11 就这样做了（读 banner 判定），
但**只做了云端视角的那一半**。7 天数据证明那一半不够：
45 次判定半死、45 次清理，只有 8 次在 30s 内恢复，
**37 次落到「需人工介入」**——云端杀完会话，板端没在 43s 内重连，
云端就再也做不了什么了。

## 边界

- `ConnectTimeout` 只管 SSH **建连**阶段。建连之后 `-W` 通道上的读
  由别的超时管辖，所以探针外层还套了一个 `timeout`。
- 探针本身每 2 分钟才跑一次，最坏情况下隧道有约 2 分钟不可用窗口。
  对运维通道可接受；要求更高就得把周期缩短，代价是板端每轮多一次
  SSH 建连（约 2s、一次认证）。
- 若 `systemctl restart` 因为权限失败，探针只能打日志，不能自愈。
  unit 以 root 跑，实际上不会遇到；手工 `--check` 时可能。

## 教训

**「没有日志」是一个观测结果，不是一条判据。**
在用「日志里没有 X」下结论之前，先问：这个程序**在健康时**会打 X 吗？
如果健康时也是静默的，那这条日志就没有区分能力，静默既证明不了健康，
也证明不了死亡。

这条和 `openspec/pitfalls/README.md` 的元教训是同一族，但方向反过来：
前面几篇是「判据在成功和失败时输出相同」，这一篇是「**根本没有判据，
我把缺席读成了结论**」。

## 关联

- [[tunnel-probe-gatewayports-no]] — 同一个修复里并发的第二个假判据
- [[journal-log-date-attribution]] — 同族：把日志的缺失/存在过度解读
- `openspec/tasks/2026-11-18-tunnel-process-disappear-diagnosis.md` — 7 天数据与完整推导

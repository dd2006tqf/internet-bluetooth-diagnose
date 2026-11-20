# 反向隧道「进程消失」根因诊断与修复方案

> 日期：2026-11-18
> 分类：运维诊断（云端 + 板端只读取证，未改任何生产配置）
> 结果：诊断完成，修复方案待批准后实施

## 构思

**要解决什么问题**：`weaknet-tunnel.service` 会「消失」——板子还活着、
`ssh board` 却连不上，有时持续十几个小时，只能人工重启。

**为什么不做进 OpenSpec change**：不改产品代码，只做只读取证 + 配置层修复，
零 footprint。按仓库约定走 `openspec/tasks/`。

**取证手段与取舍**：

| 手段 | 取舍 |
| --- | --- |
| 云端 `journalctl -u weaknet-tunnel-guard` | ✅ **主力**。该守护自 2026-11-11 起常驻，7 天记录完整、未 vacuum |
| 板端 `journalctl` | ⚠️ **部分失效**：journal 已涨到 960MB 上限，早期 boot 被 vacuum，`-b -13`/`-b -34` 只有 2 行 `-- No entries --` |
| 板端 `systemctl show` | ✅ 实时属性可信 |
| 综合 `ss`/`ps`/`/proc` | ✅ 用于判定云端会话是否真占端口 |

## 实现方式

### 关键取证

**1. 云端守护 7 天判定分布**（口径：每 60s 一轮）

| 判定 | 次数 | 含义 |
| --- | --- | --- |
| `端口 22222 未监听` | **7702** | 隧道没建立（含板子关机时段） |
| `健康（banner: OpenSSH_8.4p1 Debian…）` | **2284** | 真正可用（≈22.7%） |
| `转发层半死` | 45 | 端口在听但等不到 banner |
| `清理承载转发的 sshd` | 45 | 守护杀了半死会话 |
| `清理后 30s 内仍未恢复，需人工` | **37** | **杀完之后没能自愈** |
| `已自愈` | 8 | 杀完恢复了 |

⇒ **最刺眼的一行是 37**：云端守护把会话杀掉了，但板端 43 秒内没有重新建起
隧道，于是守护只能报「需人工介入」，然后继续每 60s 报「未监听」直到有人去重启。

**2. 守护的日志是二进制 blob，早期分析漏读了「健康」**

`健康（banner: …OpenSSH_8.4p1 Debian-5+deb11u6\r）` 里带一个 **CR**，
journald 因此把 MESSAGE 存成字节数组，`journalctl` 默认渲染成
`[74B blob data]`。直接 `grep 健康` 会得到 **0 条**，从而误判
「隧道从来没健康过」。必须 `-o json` 解码。这是本次差点又一次误判的地方。

**3. 板端「12 次失败后永久静默」不是 start-limit 触发的**

`2026-11-16 15:05:40 → 15:06:16` 连续 12 次
`ssh: connect to host 81.71.76.133 port 22: Network is unreachable`，
之后 13h47m 无任何 ssh 日志。

- `StartLimitBurst=5` / `StartLimitIntervalUSec=10s` 配 `RestartSec=3s`：
  任意 10s 窗口内最多只有 3 次重启（3s、6s、9s），**永远凑不满 5 次**；
- 全历史 grep `start-limit` / `Start request repeated` / `repeated too quickly`
  **全部 0 条**；`Failed with result` 只有 2026-11-11 那 1 条。

⇒ **不是 systemd 放弃重启**。真正的解释是：**ssh 连接成功后 `-N` 不产生
任何日志**，所以「日志静默」既可能是「正常在跑」也可能是「已死」，板端日志
无法区分这两者。这本身就是可观测性缺陷。

**4. 云端旧会话并没有占着 22222**

早先怀疑「云端僵尸 sshd 占死 22222」。实测否定：

```
ss -tlnp | grep 22222     → 只有 pid 1970210（当前活跃隧道）
旧会话 998686             → 不持有任何 LISTEN socket
```

`998686` 其实是 **Trae IDE 的远程终端会话**（子进程是
`.trae-cn-server/.../node` + `bash --login`），一直在传数据
（`lastsnd` 只有 9s，`busy:6037098ms`），跟隧道无关。

⇒ 2026-11-10 那一批「僵尸占端口」问题**已经被 2026-11-11 落地的
`ClientAliveInterval 30 / ClientAliveCountMax 3` 根治**，不能拿它解释现在的问题。

### 真正的结构性缺陷

**板端对隧道自己没有健康判据，也没有兜底。**

- 单元只有 `Restart=always`，它保证「进程退出就重启」，
  **不保证「进程还在但转发已死」能被发现**；
- `ServerAliveInterval=10 / ServerAliveCountMax=3` 只能发现 **SSH 层**断开；
- 板端 **没有任何 timer / 守护脚本**去验证 22222 是否真的通；
- 一旦出现「进程在、连接在、转发死」这种半死状态，板端会**永远认为自己健康**。

这正是 `tools/weaknet-tunnel-guard.service` 注释里写的那个已知盲区，
只是当时只在云端做了兜底，**板端一侧从未补齐**。

### 修复方案（待批准，未实施）

| # | 位置 | 改动 | 解决什么 |
| --- | --- | --- | --- |
| 1 | 板端 unit `[Unit]` | 加 `StartLimitIntervalSec=0` | 让 `Restart=always` 名副其实「永远重试」，不依赖「当前窗口凑不满 5 次」这种脆弱推论 |
| 2 | 板端 unit `ExecStart` | 加 `-o ConnectTimeout=10` | WiFi 黑洞时 10s 内失败重试，而不是挂在内核 TCP 超时（`tcp_retries2=15`，可达数分钟） |
| 3 | 板端 **新增** timer + 探针脚本 | 每 2 分钟从**板端**主动验证「本机 → 云端 22222 → 本机 22」链路，失败则 `systemctl restart weaknet-tunnel` | **补上板端唯一的兜底**：进程在但转发死，板端自己能发现并自救 |
| 4 | 云端 `tunnel_guard.sh` | 杀掉会话后，恢复等待从 30s 放宽到 60s，并在失败时**再杀一次**而非直接报「需人工」 | 37 次「杀完没恢复」大多是因为 30s 不够板端重连（`RestartSec=3` + TCP 握手 + WiFi） |
| 5 | 板端 unit `ExecStart` 日志 | 加 `-v` 或改用 `-E` 无关；改为在探针脚本里落一行「隧道已建立」 | 让「连接成功」也留下证据，消除本次遇到的「成功与死亡都是静默」 |

**不建议的改法**：把 `ExitOnForwardFailure=yes` 改成 `no`。
那会让端口绑定失败时 ssh **不退出**，于是「隧道进程在跑但 22222 根本没监听」，
比现在更难发现。该单元文件里 2026-11-10 的注释已经踩过这个坑。

## 结果

### 已实施并验证（2026-11-18）

| # | 位置 | 实际改动 | 状态 |
| --- | --- | --- | --- |
| 1 | `tools/weaknet-tunnel.service` `[Unit]` | 加 `StartLimitIntervalSec=0` | ✅ 已部署，`systemctl show` 确认 `StartLimitIntervalUSec=0` |
| 2 | `tools/weaknet-tunnel.service` `ExecStart` | 加 `-o ConnectTimeout=10` | ✅ 已部署 |
| 3 | **新增** `tools/tunnel_probe.sh` + `.service` + `.timer` | 板端每 2 分钟走完整链路读回自己的 banner，失败则 restart | ✅ 已部署，timer active/enabled |
| 4 | `tools/tunnel_guard.sh` | 恢复等待 30s → 60s，中途再清一次残留会话 | ✅ 已改（云端需重新 install 才生效） |
| 5 | 可观测性 | 由第 3 项取代（探针每轮落一行判定） | ✅ |

### 验证记录：判据的双向对照

按仓库元教训，「判据在成功与失败时输出相同就不是判据」，实施后做了正反两向实测：

| 状态 | 探针输出 | rc |
| --- | --- | --- |
| 隧道正常 | `健康（banner: SSH-2.0-OpenSSH_8.4p1 Debian-5+deb11u6）` | 0 |
| `systemctl stop weaknet-tunnel` | `ssh 失败：stdio forwarding failed` | 1 |
| 重新拉起 | `健康（…Debian…）`，**7s 内恢复** | 0 |

三种状态输出互不相同。

### 部署一致性

板端与仓库四个文件 md5 逐一相同：

| 文件 | md5 |
| --- | --- |
| `tools/tunnel_probe.sh` = `/usr/local/bin/weaknet-tunnel-probe` | `c90f3aab3906f8099645dd28d829141b` |
| `tools/weaknet-tunnel.service` = `/etc/systemd/system/weaknet-tunnel.service` | `2221ce36b25dca84561d36a5999a0800` |
| `tools/weaknet-tunnel-probe.service` | `9c8339d44501f439bf0806803e25bef0` |
| `tools/weaknet-tunnel-probe.timer` | `fade8e23295e9d9ac069cf36ec7b3f99` |

`systemd-analyze verify weaknet-tunnel.service` 无告警。

### 实施中发现的第二个假判据（已单独立篇）

探针第一版写成直连 `81.71.76.133:22222`。因为云端 sshd `GatewayPorts` 默认 `no`，
`-R` 只绑**回环**，公网网卡上没有监听者——**隧道完全健康时这条判据也必然超时**。
实测 `rc=124`，而同一时刻我自己那条 `ssh board` 正跑在这条隧道上。

改为 `ssh -W 127.0.0.1:22222`（让云端 sshd 代连自己的回环），2.1s 读回板端 banner。
详见 `openspec/pitfalls/tunnel-probe-gatewayports-no.md`。

第二处：第一版用 `... | head -1` 取 banner，`$?` 变成 `head` 的状态而不是
`timeout`/`ssh` 的状态——一条永远返回 0 的判据。改为先落临时文件再取首行。

### 遗留边界

- 云端 `tunnel_guard.sh` 的 60s 改动**需要重新 install 到云端**才生效
  （`sudo install -m 0755 tools/tunnel_guard.sh /usr/local/bin/weaknet-tunnel-guard`
  + `systemctl restart weaknet-tunnel-guard`）。本次只改了仓库里的文件。
- 板端 journal 已达 960MB 上限且无 `SystemMaxUse` 配置，早期故障现场已被 vacuum。
  **该容量策略是否符合预期仍需确认**（当前 `#Storage=auto` + `/var/log/journal`
  存在 ⇒ 持久化生效，但会持续增长）。
- 探针周期 2 分钟 ⇒ 最坏情况下隧道有约 2 分钟不可用窗口。对运维通道可接受。
- 板子断电期间 timer 不跑；重新上电且网络不通时会每 2 分钟 restart 一次隧道，
  该动作本身无害（隧道本来也连不上），网络恢复后自动转绿。

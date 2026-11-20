# 隧道健康探测直连公网地址，在隧道完全健康时也必然超时

> 发现于：2026-11-18
> 位置：`tools/tunnel_probe.sh` 第一版 `read_banner()`，板端 `/usr/local/bin/weaknet-tunnel-probe`
> 影响：把一条**完全健康**的隧道判成不健康，探针一旦上线就会每 2 分钟重启一次隧道

## 现象

新写的板端探针在隧道正常工作时输出：

```
[tunnel-probe] 探测整体超时
[tunnel-probe] 不健康
rc=1            real 0m12.017s
```

同一时刻，我自己那条交互式 `ssh board` 连接正跑在这条隧道上，`ssh board` 秒回。
也就是说**判据说隧道坏了，但隧道正在承载它自己的运维流量**。

同一台板子上另取三条事实：

```
# A. 板端连公网 81.71.76.133:22222
timeout 8 bash -c 'exec 3<>/dev/tcp/81.71.76.133/22222'
→ rc=124（超时）

# B. 云端 22222 监听在哪
ss -ltnH 'sport = :22222'
→ LISTEN 0 128  127.0.0.1:22222  0.0.0.0:*
→ LISTEN 0 128      [::1]:22222     [::]:*

# C. ssh -W 让云端代连自己的回环
ssh ubuntu@cloud -W 127.0.0.1:22222
→ SSH-2.0-OpenSSH_8.4p1 Debian-5+deb11u6      real 0m2.131s
```

## 成因

`sshd_config` 的 `GatewayPorts` 默认是 **`no`**。`-R 22222:127.0.0.1:22`
在这种配置下，云端 sshd 只把监听 socket 绑在**回环地址**上，公网网卡上
**根本没有监听者**（事实 B）。

于是板端连 `81.71.76.133:22222` 发出的 SYN：

- 到了云端主机的公网网卡；
- 内核找不到该地址的监听 socket，正常应该回 RST；
- 但云厂商安全组/防火墙通常对未开放端口直接 **DROP**，于是变成黑洞；
- 板端只能等内核 TCP 超时（`tcp_retries2=15`，可达数分钟；探测外层
  8s `timeout` 先到，返回 124）。

所以这条判据在两个状态下输出**完全相同**：

| 隧道真实状态 | `exec 3<>/dev/tcp/81.71.76.133/22222` |
| --- | --- |
| 健康 | 超时（端口根本没在公网听） |
| 半死 | 超时 |
| 隧道完全不存在 | 超时 |

它分辨不出任何东西。这正是 `openspec/pitfalls/README.md` 里那条元教训
的又一例：「判据在已知正确和已知错误的状态下输出相同，它就不是判据」。
区别在于这次的成因不在判据逻辑，而在**我根本没验证过目标地址上有没有监听者**。

## 修法

用 `ssh -W 127.0.0.1:22222`：让**云端 sshd 代连它自己的回环端口**。
那条连接正好落在 `-R` 创建的那个监听 socket 上，一路转发回板端 22：

```
板端 ssh 客户端 → 云端 :22 → cloud sshd 的 direct-tcpip 通道
  → 云端 127.0.0.1:22222（-R 的监听点）→ 反向通道
  → 板端 127.0.0.1:22 → 板端 sshd 的 banner
```

`tools/tunnel_probe.sh` 的 `read_banner()` 最终形态：

```bash
timeout "$((BANNER_WAIT + 6))" \
    ssh -i "${CLOUD_KEY}" \
        -o "ConnectTimeout=${BANNER_WAIT}" \
        -o BatchMode=yes \
        -o StrictHostKeyChecking=accept-new \
        -o LogLevel=ERROR \
        "${CLOUD_USER}@${CLOUD_HOST}" \
        -W "127.0.0.1:${CLOUD_PORT}" < /dev/null >"${out_file}" 2>"${err_file}"
```

半死状态在这一步的表现是 `ssh: connect failed: Connection refused`
（云端回环端口听起来还在，反向通道已断），或通道开了但读不到 banner。
两种情况都被超时与 banner 校验拦住。

## 边界

- 这条判据把「链路是否通」与「云端密钥是否可用」耦合了。可以接受：
  隧道本身用同一把钥匙建连，钥匙坏了隧道必然建不起来，它确实是健康度的
  真实因素。若密钥被拒，探针会明确打「这不是隧道故障，重启隧道无意义」，
  而不是无脑重启。
- 若将来有人把云端 sshd 改成 `GatewayPorts yes`，公网直连就能通了，
  但那时 `-W` 写法**依然正确**，不需要改回去。所以不要因为「公网能连了」
  就把判据改回 `/dev/tcp`。
- 云端回环地址显式写 `127.0.0.1`，与 `-R` 的绑定目标一致。
  写成 `localhost` 在双栈机器上可能先解析到 `::1`，虽然云端确实同时
  监听了 `[::1]`，但多一层不确定性没有收益。

## 教训

**在写「连不上就是坏了」这条判据之前，先确认「连得上」长什么样。**
尤其跨主机时：监听地址、`GatewayPorts`、安全组三层里任何一层都能让
一条健康的链路在客户端表现为超时，而这三层都不会报错。
本次的破局点不是读代码，而是去云端 `ss -ltn` 看了一眼端口绑在哪个地址上。

**附带的第二次错误**：第一版还用 `... | head -1` 取 banner，
导致 `$?` 是 `head` 的状态而不是 `timeout`/`ssh` 的状态 —— 一条永远返回 0
的判据。改为输出先落临时文件再取首行。同一族错误的两个变体在同一个
函数里并发了。

## 关联

- [[proc-device-tree-find-symlink]] — 同族：判据无区分能力
- [[oracle-exit-code-as-truth]] — 同族：拿错量当判据
- [[dmesg-error-vs-function]] — 同族：线索被当成结论
- `openspec/tasks/2026-11-18-tunnel-process-disappear-diagnosis.md` — 本次修复的完整背景

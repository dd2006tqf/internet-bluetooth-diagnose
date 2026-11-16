# `lsof /dev/video*` 会让整板失联，且第二次复现

> 发现于：2026-11-15（首次）/ 2026-11-16（复现）
> 位置：板端 `lsof` 遍历 V4L2 设备节点
> 影响：一条看起来完全无害的排查命令，直接导致开发板 SSH 断开、
> 隧道失联，必须物理重启才能恢复。

## 现象

排查 IMX219 零帧时，想知道「谁占着 `/dev/video0`」，于是：

```bash
ssh board 'sudo lsof /dev/video0'
```

**SSH 连接立刻断开**（`Connection to 127.0.0.1 closed by remote host.`），
随后云端 `127.0.0.1:22222` 的监听器消失，板端 `weaknet-tunnel`
也没有重连上来，整板只能物理重启。

第一次发生时（2026-11-15）我把它记为「原因未定」，因为同时期还做过
别的事情，无法排除。**2026-11-16 第二次同因同果，可以判定。**

| 次数 | 时间 | 命令 | 结果 |
| --- | --- | --- | --- |
| 1 | 2026-11-15 | `sudo lsof /dev/video*` | SSH 立刻断开 |
| 2 | 2026-11-16 | `sudo lsof /dev/video0` | SSH 立刻断开，22222 监听器消失 |

## 成因

**未查清具体内核路径。** 以下是排除了可能性之后的推断，标注为推断。

`lsof` 的工作方式是遍历 `/proc/*/fd/*`，对每个 fd 做 `readlink`
拿到指向的路径，再 stat。对**普通文件**这完全安全。

但 V4L2 字符设备在 sunxi-vin 驱动里有一类特殊行为：部分 `ioctl`
会在驱动内部持有 spinlock 的情况下等待硬件，或者访问尚未初始化的
硬件寄存器。`lsof` 本身只做 `readlink` 和 `stat`，理论上不触发 ioctl。

已知的相邻事实：

- `/sys/kernel/debug/mpp/mipi` **一读整板冻结**（本项目早已记为硬规则，
  即使 `timeout` 也救不回来）——说明 sunxi-vin/MPP 这条链路上存在
  「读一下就死」的 debug 路径，且不是孤例。
  那一条的机制已于 2026-11-16 从源码定位清楚：`open()` 里直接 `readl()`
  已 gated 的 PHY 寄存器，配合 `PANIC_ON_OOPS=y` + `PANIC_TIMEOUT=0`
  永不重启。详见 [mpp-mipi-debugfs-panics.md](mpp-mipi-debugfs-panics.md)。
  （`lsof` 这条本身仍然成因未定，不要把两者的机制混为一谈。）
- 同一天用 `fuser -v /dev/video0`（2026-11-15 09:07）**没有**导致失联。
  两者都遍历 `/proc`，区别在于 `lsof` 会解析更多信息。

**结论：这是 sunxi-vin/V4L2 驱动的一个已知危险区，具体触发点未定位。**
不猜机制，如实标注为「未查清」。

## 修法

**不要对 V4L2 设备节点用 `lsof` 或 `fuser`。**

要判断「谁占着 video0」，用这些替代手段（都不会碰设备节点本身）：

```bash
# 1. 看 systemd 单元状态（服务持有设备时必然是它）
systemctl is-active weaknet-server

# 2. 看进程自己打开了哪些 fd（不经过 lsof 的设备解析）
sudo ls -l /proc/<pid>/fd/ | grep video

# 3. 需要确认占用者时，直接看服务日志里有没有 open 成功
sudo journalctl -u weaknet-server | grep "camera capture started"
```

第 2 条只读 `/proc/<pid>/fd/` 这个目录，不做设备层 stat，风险显著更低
（但**也不是零风险**，见边界）。

## 边界

- **成因未定位**，所以「哪些命令安全」也只能给经验判断，不能给保证。
  已有证据只到「`lsof` 两次致死、`fuser` 一次未致死」。
- 失联后**板端隧道不会自愈**。云端 `weaknet-tunnel-guard` 会正确判定
  「转发层半死」并清理占端口的僵死会话（12:24:07 的日志证实它工作正常），
  但板端 `weaknet-tunnel` 在板子本身出问题时无法重连，仍需物理重启。
- 这条规则与 `/sys/kernel/debug/mpp/mipi` 是**同一类**：
  sunxi-vin 链路上「看起来只读、实际会锁死」的路径。遇到 V4L2 debug
  相关操作时，默认应该假设它可能致死，先用最低风险手段。

## 教训

**「只读」不等于「安全」。** `lsof` 不写任何东西，`cat` 一个 debugfs
文件也不写任何东西，两者都让整板失联。判断一条命令能不能跑，不能只看
它「改不改状态」，还要看它**触碰的是哪条驱动路径**。

在嵌入式真机排查中，一条命令的代价可能是「物理重启一次整板」。
所以对任何涉及 V4L2 / MPP / sunxi-vin 的探针，先问：
**最坏情况下它会把板子弄成什么样？我能不能接受？**

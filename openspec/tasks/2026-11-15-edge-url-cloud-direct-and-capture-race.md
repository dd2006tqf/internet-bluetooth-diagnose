# 上行出口修复（edge.url 直连云端）+ 采集启动竞态取证

> 日期：2026-11-15
> 分类：工作流外实施
> 结果：**已完成** —— 板端三条上行（遥测 / 无线事件 / 视频留证）从「必然失败」
> 变为云端 `200 OK`；顺带取证并澄清了一个会被误判为「硬件坏了」的采集竞态。

## 构思

### 这一步要解决什么问题

`2026-11-14-video-clip-e2e-verification.md` 的遗留第 1 条写着：

> `edge.url` 指向板子自己的回环，而 8000 在板端没有任何监听。所以**生产路径的
> 留证上行在当前配置下必然失败**——本次 E2E 是靠 `--url` 覆盖绕过去的，
> 生产配置本身没修。

这条意味着：留证能力只在探针里存在过。生产进程每次想上传都会失败，而失败的
原因（连不上自己的回环）与「云端拒收」「签名错」完全不同——它连**网络这一层
都没走到**。不去修它，后面所有关于留证的验证都只是在验证探针。

### 这一次尝试把「必然失败」判断成了「误报」

中途我一度向用户汇报「缺口 #1 其实不存在」，理由是板端
`curl -X POST http://127.0.0.1:8000/...` 返回了真实云端 uvicorn 的响应。

**这个判断是错的。** 那个 8000 监听来自一条**临时的 SSH 会话**：

```
sudo ss -ltnp | grep :8000
LISTEN ... users:(("sshd",pid=792,fd=11))     ← 是 sshd，不是板端服务
TYPE  ... users:(("sshd",pid=792,fd=10))
```

而 `weaknet-tunnel.service` 的 `ExecStart` 里**只有 `-R 22222:127.0.0.1:22`**，
它的注释明确写着「不要再加 -L 8000 / -L 8090」（2026-11-10 事故后移除）。

所以正确的推理是：**板端 8000 平时根本没有监听**，我测到的可用状态是某条
入站 SSH 会话的副产品。用「刚好有人在」的瞬时状态去否定一个配置缺陷，是
典型的假阳性——它比直接测失败更危险，因为它让缺陷被划掉了。

### 为什么不走 OpenSpec change

沿用用户对 v4l2/video 这条线的一贯指示：不进 change。本次是**配置修复 +
运行态取证**，没有新的产品表面或 requirement。

## 实现方式

### 决策：用户选 A（板端直连云端公网）

| 方案 | 做法 | 代价 |
| --- | --- | --- |
| **A（选定）** | `edge.url` 改公网 `http://81.71.76.133:8000/...` + `allow_insecure_http: true` | 明文过公网；`allow_insecure_http` 本身是显式安全降级 |
| B | 另开独立 unit 做板端→云端 8000 转发，与 `-R 22222` 隔离 | 复用 SSH 通道不暴露公网；多一个 unit，`ExitOnForwardFailure` 仍有风险 |
| C | 板端起本地反向代理转到公网 | 配置不动；多一个进程和一个故障点，收益最差 |

选 A 的理由是成本最低且立即可验证（云控制台已放行 8000）。

### 改了两处，不是一处

这是本次最容易漏的点：`/etc/weaknet/config.yaml` **不是**板端独立维护的，
它是仓库根目录 `config.yaml` 经 `tools/ci.sh:335` 渲染后安装的：

```bash
sed "s|^  token: \"\"$|  token: \"${EDGE_TOKEN}\"|" config.yaml > "${RENDERED_CONFIG}"
ssh "${BOARD}" "sudo install -m 0600 /dev/stdin /etc/weaknet/config.yaml" < "${RENDERED_CONFIG}"
```

所以只改板端，**下次部署就会被覆盖回去**。必须同时改仓库源文件。

| 文件 | 改动 |
| --- | --- |
| `config.yaml`（仓库根） | `edge.url` 改公网；新增 `allow_insecure_http: true`，附完整理由注释 |
| `/etc/weaknet/config.yaml`（板端） | 只改 `url` 一行 + 插入 `allow_insecure_http`，**不整份覆盖**（板端那份含 `ci.sh` 注入的真实 token，仓库那份是空占位） |

板端改写用 python3 就地打补丁而非整文件替换，并断言旧 url 恰好出现一次、
`allow_insecure_http` 尚不存在——避免在已被别人改过的文件上盲写。
改前备份到 `/etc/weaknet/config.yaml.bak.20261115`。

### 为什么必须开 `allow_insecure_http`

`weaknet_config.cpp:538-563` 的 `validateEdgeUrlPolicy()` 在**全文件解析完成后**
调用一次，非回环 + 明文 `http://` 会被直接拒绝加载。这是刻意的策略
（`isLoopbackHost()` 只放行 `127.0.0.0/8`、`localhost`、`::1`）。

改成公网地址后必须显式开这个键。它的价值不是「让它能跑」，而是**在配置里
留下"我知道我在用明文过公网"的痕迹**——将来要收回这条降级，应该改 https +
反代，而不是悄悄删掉这个键。

## 结果

### 云端独立证据（不是看板端日志）

板端日志只能证明「没报错」，不能证明「送达」。真正的证据在云端 uvicorn 的
access log，且源地址从隧道内的 `127.0.0.1` 变成了板端公网出口：

```
Nov 15 14:34:11 python[2558689]: INFO: 36.141.24.90:49681 - "POST /api/v1/network/edge/telemetry HTTP/1.1" 200 OK
Nov 15 14:34:14 python[2558689]: INFO: 36.141.24.90:49682 - "POST /api/v1/network/edge/wireless-events HTTP/1.1" 200 OK
Nov 15 14:34:23 python[2558689]: INFO: 36.141.24.90:49683 - "POST /api/v1/network/edge/telemetry HTTP/1.1" 200 OK
Nov 15 14:34:35 python[2558689]: INFO: 36.141.24.90:49684 - "POST /api/v1/network/edge/wireless-events HTTP/1.1" 200 OK
```

修复前后对比：

| 指标 | 修复前 | 修复后 |
| --- | --- | --- |
| 板端 `Couldn't connect / http=0` 计数（2 分钟窗口） | 每轮都失败 | **0** |
| 云端收到 edge POST | 无 | 每 10 秒一轮，持续 200 |
| 源地址 | （走隧道时 127.0.0.1） | 板端公网 36.141.24.90 |
| `clips_url` | `http://127.0.0.1:8000/...`（无对端） | `http://81.71.76.133:8000/...` |

服务启动日志确认配置被接受且三条上行都指向新地址：

```
Config loaded from: /etc/weaknet/config.yaml
视频留证上行已启动: clips_url=http://81.71.76.133:8000/api/v1/network/edge/video-clips
边缘遥测上报已启动: device=radxa-cubie-a7a interval_ms=10000 buffer=10
无线事实上行已启动: events_ms=... state=/home/radxa/weaknet/data/wireless-uplink-state
```

### 顺带取证：采集「首次 open 零帧」是可复现的启动竞态

排查过程中板子重启了三次，得到一个一致的模式：

| 时刻 | 事件 | 采集 |
| --- | --- | --- |
| 06:00 重启后服务自动启动 | sensor 首次 open | ❌ 零帧，`tdm0 rx0 frame error! width is 0` |
| 06:02~06:03 停服务跑 gst | sensor 关闭再打开 | ❌ 零帧（gst 路径同样失败） |
| **06:04:49 服务 start** | **sensor 再次 open** | ✅ `hub configured`，TDM 错误 0 |
| 06:18:12 别的会话 start 服务 | sensor 再次 open | ✅ 出帧 |
| 06:33:22 我 restart 服务 | sensor 再次 open | ✅ 出帧 |

结论：**开机后第一次打开 sensor 会失败，之后再打开就正常**，可复现。
生产上表现为「重启板子后摄像头要等一次服务重启才出画面」。

失败的形态值得记：`width is 0` 且 `height` 在 2255~2420 之间随机跳动。宽度恒 0
说明一行有效像素都没收到，而高度跳动说明 TDM 数到的是噪声而非帧——
这不是「丢帧」，是 CSI-2 上根本没有有效数据。

**因此「板子重启」不是这个问题的解法，「重开一次设备」才是。**
此前把它归因为「内核模块引用计数坏了」（确实观察到 `module_put` WARNING 与
`imx219 is not used, video0 cannot be close!`）并不完整：重启确实清掉了 WARNING
（计数归 0），但第一次 open 仍然失败。真正的判据是 open 次数，不是重启。

### 一次操作事故（必须记）

读 `/sys/kernel/debug/mpp/mipi` **把板子卡死了**，SSH 立即断开、板子自己重启
（事后 `uptime` 归零）。

`openspec`/`memory` 里早有这条：*在流式期间反复读该文件会把板子卡死，必须
单次读取 + 硬超时*。我用了 `timeout 5`，仍然卡死——说明**超时保护不足以救它**，
这个文件应当被当作**完全不可读**对待，而不是"小心地读一次"。

### 遗留问题

1. **明文过公网**。方案 A 的直接代价。签名（Ed25519）保证完整性，但不保证
   机密性——遥测内容与设备标识可被链路上任何人看到。若要收紧，应上
   Nginx + TLS（即当时未选的 B/C 混合方案），而不是继续加开关。
2. **采集首次 open 失败未修**。当前靠「重开一次」绕过，产品侧没有自动重试。
   建议在 `V4l2FrameSource::start()` 里对「STREAMON 成功但首窗口零帧」做一次
   有界重试，但那需要先确认重试是否稳定有效（本次样本 3/3 成功）。
3. **`/sys/kernel/debug/mpp/mipi` 应加入禁用清单**，避免再有人（或我）去读。

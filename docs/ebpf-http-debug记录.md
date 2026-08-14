# eBPF HTTP 延迟监控问题排查记录与下一步方向

> 文档日期：2026-08-12（第三版，含 dev_queue_xmit 探索结论）
> 更新日期：2026-08-13（第四版，dev_queue_xmit 实测定性为不可行，转向 tcp_sendmsg msghdr 校正）
> 关联模块：`server/src/http_latency.bpf.c` + `server/src/http_latency_monitor.cpp`
> 涉及特性：`GetHttpLatencyStats` D-Bus 接口（`totalTxns` / TTFB）

---

## 一、问题现象

在 ARM64 开发板（Radxa Cubie A7A）真机验证时，`GetHttpLatencyStats` 始终返回 `totalTxns:0`，即使：
- 服务端 BPF 加载成功（`HttpLatencyMonitor: initialized successfully`）
- 明文 HTTP 流量（`curl http://`）返回 200
- 同机 `ProcessNetProfiler` 能抓到 curl 流量（证明同一 attach 机制可 work）

---

## 二、排查历程（按挂点方案演进）

### 方案 1：`tcp_sendmsg` + msghdr

| 结论 | 证据（debug map） |
|------|------|
| 探针 attach 正常 | `tcp_sendmsg` 触发（count>0） |
| `bpf_probe_read_user` 能读到数据 | iov 非空、data 读成功（count=2>0） |
| **但读到内容非明文 HTTP 首部** | `check_http_request` 失败（count=3=0） |

结论：`tcp_sendmsg` 的 `msg->msg_iter.iov[0].iov_base` 读到的应用原始缓冲区，不是从 HTTP 请求首字节开始（可能是分片/后续段）。

### 方案 2：`ip_queue_xmit` + skb->data

| 结论 | 证据 |
|------|------|
| `read_http_from_skb` 系统性失败 | 11 次触发全部 return 0 |
| **`skb->data` 不是 IPv4 头起点** | 细分到 `ihl<5` 或 `tcp_hdr_len<20` 失败 |

结论：`skb->data` 在 ip_queue_xmit 上下文指向非 IP 头位置，偏移假设错。

### 方案 3：`ip_queue_xmit` + skb->network_header

| 结论 | 证据 |
|------|------|
| **`skb->network_header == 0`** | 全部在 `nh_off==0` 处失败（debug key 22） |

结论：`ip_queue_xmit` 时 skb 头偏移字段尚未初始化，挂点不适配。

### 方案 4（当前最接近）：`dev_queue_xmit` + skb->network_header

**这是正确挂点**，`network_header` 有效。关键计数：

| debug key | 含义 | 计数 |
|-----------|------|------|
| 0 | 探针触发 | 2525 |
| 24 | head/nh_off/len OK | 2525 |
| 26 | 非 IPv4 跳过 | 2490 |
| 28 | IPv4 ihl OK | 35 |
| 31 | TCP 头长度 OK | 13 |
| **34** | **payload 读成功** | **3** |
| **40** | **payload 前 4 字节** | **0x00000000（全零）** |
| 4/5 | check_http_request | 失败(4)、通过(0) |

**结论**：
- `dev_queue_xmit` 挂点正确（network_header 有效）
- `read_http_from_skb` 已能走到 payload 读取（key 34 非零）
- **但读到 `0x00000000` 全零**（key 40）→ 读到的不是 HTTP 明文
- 根因仍是 payload 偏移读取到了非 HTTP 数据区域（或读到 UDP 包/TCP 头部等，偏移需细化）

### 方案 5（2026-08-13 实测）：dev_queue_xmit —— 定性为「payload 在非线性 fragment，取数不可行」

新增键盘偏移/协议/线性区取样后板端实测（`http_debug` array map）：

| debug key | 值 | 含义 |
|-----------|-----|------|
| 41 | 8 | `skb->protocol` = 0x0008（IPv4，大端 0x0800） |
| 42 | 6 | **IPv4 protocol = 6（TCP）** → 不是 UDP |
| 43 | 138 | `skb->len` = 138（纯小段） |
| 44 | 84 | **`skb->data_len` = 84 → 非线性 fragment** |
| 45 | 54 | `linear_len` = 138 − 84 = 54（线性区仅含 IP/TCP 头） |
| 47/48 | 280 / 300 | network_header / transport_header |
| 54 | 73 | 线性守卫：`nh(280)+payload(40)+64=384 > linear(54)` → 全部被线性边界拦下 |
| 34 / 50-53 | 0 | payload 读取成功 / 各偏移样本全部为 0 |

**决定性结论**：
- 这些 dev_queue_xmit 包都是 TCP 小段，`skb->len=138` 但 `data_len=84`，**HTTP 明文 payload 位于非线性 fragment**，不在线性区 `head + network_header + offset`。
- `head+offset` 方案在此挂点天然取不到明文 → **不是偏移差一点，而是内存布局根本不包含 payload**。
- 文档 3.1「payload 偏移细化」路线被实测推翻：无论怎么细化偏移，线性区里就没有 HTTP 明文。

**处置**：放弃 `dev_queue_xmit + network_header` 路线，转向文档 3.2（`tcp_sendmsg` msghdr 校正）。dev_queue_xmit 相关的偏移/协议/线性区取样代码已从 `http_latency.bpf.c` 移除，探针改回 `kprobe/tcp_sendmsg`。

---

## 三、未来路线（专门开新会话时参考）

### 3.1 ~~`dev_queue_xmit` 的 payload 偏移细化~~（已废弃）

> 方案 5 实测证明：本路线不可行。`dev_queue_xmit` 的 skb `data_len>0`，明文 payload 在非线性 fragment，
> `head+offset` 线性区无 payload。2026-08-13 起不再沿此方向调试。

### 3.2 当前路线：`tcp_sendmsg` msghdr 校正（实施中）

回到 `tcp_sendmsg`，重点校正 `msg_iter.iov` 的**多 iov 段拼接**，逐段 `bpf_probe_read_user` 读用户态缓冲的前 N 字节，组合成准确的 HTTP 首部。当前 `http_latency.bpf.c` 已改为 `kprobe/tcp_sendmsg`（探针名 `probe_http_req`），并在容器内编译通过。

### 3.3 备选：tc egress / fentry

`BPF_PROG_TYPE_SCHED_CLS` attach 到 tc（此时 skb 完整含各层头）或 fentry 挂 `dev_queue_xmit`。

### 3.4 本质局限（绕不开）

`check_http_request`/`check_http_response` 只识别明文 `GET`/`POST`/`HTTP/1.`，**`https://` 是 TLS 密文永远认不出**。即使取数打通，只支持明文 `http://` 场景。

---

## 四、已稳固定成果（非 HTTP 部分）

| commit | 内容 |
|--------|------|
| `e762c0d` | 5 个监控器补 attach（ProcessNetProfiler 真机抓取成功） |
| `4ff878b` | DNS 请求路径累计计数（GetDnsStats 非 0） |

**代码基线**：`http_latency.bpf.c` 已由 dev_queue_xmit 方案改为 `kprobe/tcp_sendmsg`（msghdr 多 iov 拼接，**未提交**）；`http_latency_monitor.cpp` attach 名已是 `probe_http_req`。dev_queue_xmit 的偏移/协议/线性区取样已移除。

---

## 五、开发板环境备忘（供新会话快速进入）

| 事项 | 说明 |
|------|------|
| SSH | `radxa@192.168.2.77`，当前可用（曾因 `/home/radxa` 权限损坏失效，已修） |
| sudo 免密 | 已配 `/etc/sudoers.d/99-radxa-nopasswd` |
| ARM64 容器 | 反复 `exec format error`（QEMU binfmt），每次需 `docker run --rm --privileged multiarch/qemu-user-static --reset -p yes` 后重建容器 |
| 真机验证流程 | 服务端框架启动 + `curl http://` 发明文流量 + `bpftool map dump` 观察 `http_debug`/`http_txn_stats` |
| 编译 | 容器内 `clang -target bpf` 编译 `.bpf.o`，scp 到板 `build/`；服务端二进制改动需 `make` 重链 |

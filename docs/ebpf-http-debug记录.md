# eBPF HTTP 延迟监控问题排查记录与下一步方向

> 文档日期：2026-08-12（第三版，含 dev_queue_xmit 探索结论）
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

---

## 三、未来路线（专门开新会话时参考）

### 3.1 下一步切入点：`dev_queue_xmit` 的 payload 偏移细化

方案 4 已验证挂点正确、能读到数据（虽为全零）。下一步：
1. **区分 TCP/UDP**：`dev_queue_xmit` 也会过 UDP 包，只处理 TCP（`skb->protocol` 或从 IP 头 next_protocol 判断）
2. **多偏移取样**：除 `ip_hdr_len + tcp_hdr_len` 外，尝试多个偏移读 payload，找出 HTTP 明文实际起点
3. **排除空数据包**：过滤掉 `skb->len` 不含 payload 的纯 ACK/握手包，只在 payload 非空时取样

### 3.2 备选：`tcp_sendmsg` msghdr 校正

若 dev_queue_xmit 偏移仍难解，回到 `tcp_sendmsg`，重点校正 `msg_iter.iov` 的多 iov / iov_offset，读用户态缓冲的准确 HTTP 首部。

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

**代码基线**：`http_latency.bpf.c` 当前含 dev_queue_xmit 探索改动（**未提交**）；`http_latency_monitor.cpp` attach 名已改为 `probe_http_req`（**未提交**）。

---

## 五、开发板环境备忘（供新会话快速进入）

| 事项 | 说明 |
|------|------|
| SSH | `radxa@192.168.2.77`，当前可用（曾因 `/home/radxa` 权限损坏失效，已修） |
| sudo 免密 | 已配 `/etc/sudoers.d/99-radxa-nopasswd` |
| ARM64 容器 | 反复 `exec format error`（QEMU binfmt），每次需 `docker run --rm --privileged multiarch/qemu-user-static --reset -p yes` 后重建容器 |
| 真机验证流程 | 服务端框架启动 + `curl http://` 发明文流量 + `bpftool map dump` 观察 `http_debug`/`http_txn_stats` |
| 编译 | 容器内 `clang -target bpf` 编译 `.bpf.o`，scp 到板 `build/`；服务端二进制改动需 `make` 重链 |

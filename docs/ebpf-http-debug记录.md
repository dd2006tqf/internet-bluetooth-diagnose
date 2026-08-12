# eBPF HTTP 延迟监控问题排查记录与下一步方向

> 文档日期：2026-08-12（第二版，含最新 debug 结论）
> 关联模块：`server/src/http_latency.bpf.c` + `server/src/http_latency_monitor.cpp`
> 涉及特性：`GetHttpLatencyStats` D-Bus 接口（`totalTxns` / TTFB）

---

## 一、问题现象

在 ARM64 开发板（Radxa Cubie A7A）真机验证时，`GetHttpLatencyStats` 始终返回 `totalTxns:0`，即使：
- 服务端 BPF 加载成功（`HttpLatencyMonitor: initialized successfully`）
- 明文 HTTP 流量（`curl http://`）返回 200
- 同机 `ProcessNetProfiler` 能抓到 curl 流量（证明同一 attach 机制可 work）

---

## 二、排查历程（多轮 debug 定位）

### 2.1 已排除 / 已确认

| 结论 | 证据 |
|------|------|
| attach 机制本身正常 | `ProcessNetProfiler`（同款 `bpf_program__attach`）能抓到流量 |
| 探针起初"未触发"是 bpftool CLI attach 语法问题 | `bpftool prog attach id N kprobe` 报 `invalid attach/detach type`；服务端用 libbpf API 无此问题 |
| **`tcp_sendmsg` 方案：数据能读到，但内容非明文 HTTP 首部** | debug map：`iov` 非空、`bpf_probe_read_user` 成功(count=2>0)，但 `check_http_request` 失败(count=3=0) |
| **`ip_queue_xmit` skb 方案：`read_http_from_skb` 系统性失败** | debug map：11 次触发中 `read_http_from_skb` 全部 return 0 |

### 2.2 `ip_queue_xmit` skb 方案的细分定位（决定性证据）

通过 64 槽 debug map 细分，`read_http_from_skb` 的失败点：

| index | 检查 | 计数 | 结论 |
|-------|------|------|------|
| 0/20/21 | 探针触发、data 非空 | 5 | ✅ |
| 22/23 | `total_len >= 40` | 5 | ✅ |
| **25** | **`ihl < 5`（1字节读出的 version_ihl 低4位 < 5）** | **2** | ⚠️ |
| **26** | ihl 正常 | 3 | ✅ |
| **28** | **`tcp_hdr_len < 20`（TCP data_off>>4*4 < 20）** | **3** | ❌ |

**根因**：`skb->data` **不是 IPv4 头起点**。在 `ip_queue_xmit` 的 kprobe 上下文，`skb->data` 可能已指向 IP payload / TCP 头 / 或含 MAC 头，导致从 `data[0]` 解析 `version_ihl`、`data+ihdr+12` 解析 `TCP data_off` 全错 → 读不到 HTTP payload。

---

## 三、下一步方向

### 3.1 正确取数方案（建议探索）

不再假设 `skb->data` 是 IP 头，改用 **`skb->network_header`** 定位网络头起点：
```c
// skb->network_header 是相对 head 的偏移，从 head 偏移取 IP 头
struct sk_buff *skb;
__u16 nh_off = BPF_CORE_READ(skb, network_header);
unsigned char *ip_start = BPF_CORE_READ(skb, head) + nh_off;
```
即：`IP 头 = skb->head + skb->network_header`，然后解析 IPv4 IHL + TCP data_off，再从 payload 读明文 HTTP。

### 3.2 或使用 tc / tcpdump 式挂点

若 kprobe 方式持续不稳定，改用 `tracepoint/net/netif_receive_skb`（已用于 wifi_packet_loss）读入站，或 `fentry` 挂 `tcp_sendmsg`，在这些点 skb/data 布局更确定。

### 3.3 本质上 HTTPS 局限仍然存在

`check_http_request`/`check_http_response` 识别明文 `GET`/`POST`/`HTTP/1.`，对 `https://`（TLS 密文）永远失效。即使取数解决，也只支持明文 HTTP 场景。

---

## 四、已固化成果（与 HTTP 无关的稳定部分）

| commit | 内容 |
|--------|------|
| `e762c0d` | 为 5 个 eBPF 监控器补齐探针 attach 逻辑（DNS/WiFi/HTTP/Process/TCP重传）——其中 ProcessNetProfiler 真机验证抓取成功 |
| `4ff878b` | DNS 请求路径累计查询计数（`GetDnsStats` 从 0 → totalQueries 非 0） |

**当前代码基线**：`http_latency.bpf.c` / `http_latency_monitor.cpp` 已还原到 HEAD（原始 msghdr `msg+16` 版），HTTP 探索性改动已移除。HTTP 取数作为**长期专题**单独跟踪。

---

## 五、开发板环境问题汇总

| 问题 | 状态 |
|------|------|
| SSH 免密失效 | 已恢复（重新 authorized_keys + `/home/radxa` 权限修复） |
| sudo 免密 | 已重建 `/etc/sudoers.d/99-radxa-nopasswd` |
| ARM64 容器反复 `exec format error` | QEMU binfmt 需反复 `multiarch/qemu-user-static --reset`；容器重建后可用 |
| 真机验证 | 通过服务端框架 + bpftool dump map 完成（绕开部分文件权限问题） |

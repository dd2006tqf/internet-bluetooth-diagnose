# 跨探针拼一次观测，query 与 response 会数成两个世界

> 发现于：2026-09-12
> 位置：`server/src/dns_monitor.bpf.c:792`（`SEC("kprobe/ip_finish_output2")`）
> 与 `server/src/dns_monitor.bpf.c:977`（`SEC("kprobe/udp_queue_rcv_skb")`）
> 影响：DNS 事务配对严重失衡，SLE 判定建立在错误的样本上

## 现象

真机长稳跑下来，Tracker 里的 query 与 response 数量差了一个数量级：

```text
tracker_q=45    tracker_r=324
```

45 次查询配 324 次响应。这两个数字**不可能是同一次采样的产物**——
没有 query 哪来的 response。但系统不报错，只是安静地把 ratio 算成
一个荒唐的值，然后拿它去参与 DNS SLE 的判定。

比起报错，这种「数字自洽地错着」更难发现：没有异常、没有丢事件计数飙升，
`fail=0`、`perf_lost=0`，一切指标健康，只有配对比例不对。

## 成因

第一版把一次 DNS 事务拆成两个独立探针，各自取一半信息：

```
syscall tracepoint  → payload（用户缓冲区里的 DNS 报文）
kprobe              → endpoint（sock 结构里的地址与端口）
```

然后按 `pid_tgid` 把两半 join 起来。这条路径有三个独立的失效点：

1. **触发时机不同。** 两个 hook 不是在同一次函数调用里被触发，
   中间隔着内核的调度与协议栈处理，时序上没有必然顺序。
2. **没有稳定的关联键。** `pid_tgid` 只能说明「是同一个进程」，
   同一进程并发发两个 DNS 查询时就无法区分谁配谁。
3. **陈旧条目污染后续。** join 失败留下的 pending 条目不会自己消失，
   它们会与后续的无关事件误配，把错误扩散到时间轴的后面。

三件事叠加的结果就是：有些 query 根本找不到自己的 response（少算），
有些 response 匹配到了别的 query（多算）。方向还是不对称的，
所以数量会朝一个方向偏。

**根本问题不是「join 写得不够好」，而是「join 这个动作本身不该存在」。**

## 修法

确立单一来源原则（代码里记为 IR-DNS-5，见 `:299`）：

> 一个 observation 的 endpoint 与 payload 必须来自同一个 hook、
> 同一语义源，不允许依赖跨独立 probe 的时序 join。

落到 DNS 上，query 与 response 各自找一个「两半都齐」的挂点：

```text
query    : kprobe/ip_finish_output2   skb → UDP/IP 头 + DNS 载荷
response : kprobe/udp_queue_rcv_skb   skb → UDP/IP 头 + DNS 载荷
```

`ip_finish_output2` 收的是 `sk_buff`，此时 **UDP 与 IP 头都已构造完毕、
地址已定型**，endpoint 从 `transport_header` / `network_header` 指向的
报文里读，payload 从同一份报文里读。两半同源，没有任何 pending pair。

选点前逐个证伪过两个看似更自然的挂点：

| 候选挂点 | 为什么不行 |
| --- | --- |
| `udp_sendmsg` **入口** | 未 connect 的 socket 尚未 autobind，`skc_num` / `skc_rcv_saddr` 仍为 0，取不到真实 local endpoint |
| `udp_send_skb` **入口** | UDP 头尚未写入——这个函数自己才写，`transport_header` 不可用 |
| `ip_finish_output2` | UDP/IP 头均已就绪、地址已定型 ✓ |

响应侧同理：`udp_recvmsg` 在 entry 时用户缓冲区还没写入数据，
要取 payload 就得 entry/return 配对保存指针——那又是另一种跨阶段脆弱状态。
队列态的 `skb` 已经持有完整载荷，不需要配对。

旧的 syscall tracepoint 路径没有删干净，但已明确改为**不投递**
（`:617`、`:649`），只保留计数器与 `pending_recv` 的清理，
避免删代码时连带弄坏统计口径。这是有意的过渡残留，不是遗漏。

### 两个让这件事哑火很久的实现细节

代码里两处注释值得单列，因为它们是「方向对了但数据还是空的」的真因：

**一、`iovec` 描述符在内核内存，`iov_base` 指向用户内存。**
内核把用户传入的 iovec 数组拷贝到了自己这边（小数组用栈上的 `iovstack`，
大数组用 kmalloc），所以数组本身要用 `bpf_probe_read_kernel` 读，
只有 `iov_base` 指向的才是用户数据，要用 `bpf_probe_read_user` 读。
`server/src/dns_monitor.bpf.c:338` 与 `:345` 两行分别对应这两次读。
一开始全用 user 读，结果 `emitted` 恒为 0——不是没命中，是读失败了。

**二、端口与地址必须在同一数值域。**
端口用大端组装成主机序数值（`(ports[0]<<8)|ports[1]`），
IP 则按 4 字节原样读出（网络序字节的小端加载）。两处都是刻意选的，
目的是让 query 侧与 response 侧产出的 canonical key 逐字节相等。
`server/src/dns_monitor.bpf.c:857` 与 `:1066` 的注释写着「同域」，
这个词不是修辞——换一种读法，两边的 key 就永远对不上，
表现为「请求响应各自都采到了，但配对率是 0」。

修完的真机结果是：

```text
dir=0 cli=...:59361 rs=... txid=17699
dir=1 cli=...:59361 rs=... txid=17699   ← canonical 键完全一致
fail=0/18  ok=18  unmatched=0  ambiguous=0  late=0
```

## 边界

单一来源只保证「同一次报文的两半不会配错」，它**不保证配对本身能完成**。

`unmatched` 依然会存在：应答在观测开始前就发了、LRU 淘汰了、
或者报文在 perf 通道里丢了。区别在于这些现在是**可解释的残余**，
而不是结构性的错配——残余有计数器（`unmatched` / `ambiguous` / `late`），
错配没有。

另一个边界：`udp_queue_rcv_skb` 挂在 softirq 上下文，`pid` 不可信。
凡是需要「这条流量属于哪个进程」的判断，在这里都做不了。
代码里的规避方式是 BPF 侧端口打标（`dns_self_endpoints`，
按 `(local_ip, local_port)` + 30s TTL 匹配），用端点而不是 pid
来识别「这是我们自己的主动探测」。这是个绕路，不是通用解。

## 教训

**当一次观测需要两处信息时，先找那个「两处都齐」的地点，
而不是先写两个探针再想办法拼起来。**

join 的代价不是多写几行代码，而是引入了一整类不可消除的失效模式：
时序不确定性、关联键不唯一、失败状态的清理。这些在并发下全部放大，
而且失败方式和成功方式长得很像——数据还是流出来了，只是错的。

判断标准可以简化成一个问题：**这两半信息，内核有没有在任何一刻
同时持有过？** 如果有，就去那里取。如果没有（比如需要用户态缓冲区内容，
而内核只在内核态短暂持有指针），那才是真的需要配对——此时也必须接受
「配对率不是 100%」并在指标里显式暴露残余，而不是假装它没有。

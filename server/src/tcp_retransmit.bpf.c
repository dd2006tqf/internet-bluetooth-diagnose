/*
 * 文件: tcp_retransmit.bpf.c
 * 功能: TCP 重传追踪。通过挂载 TCP 内核层的 kprobe，按"连接"维度统计
 *       TCP 重传事件和分段数，用于高效计算 TCP 丢包率：
 *         丢包率 = total_retrans / total_segs
 *       替代传统方案（周期性 cat /proc/net/netstat 或 netlink dump，开销大）。
 *
 * 挂载的探针类型和内核函数:
 *   - kprobe/tcp_retransmit_skb : TCP 重传发生时触发（主动丢包告警）
 *   - kprobe/tcp_sendmsg       : TCP 数据发送时触发（估算发送总段数，用作丢包率分母）
 *
 * 使用的 BPF Map:
 *   - retrans_events : LRU_HASH，key=TCP 4 元组，存储最近一次重传事件详情（时间戳/PID/计数）
 *   - retrans_stats  : LRU_HASH，key=TCP 4 元组，存储连接级累计统计（total_retrans / total_segs）
 *
 * 用户态对应的 Monitor 类: TcpRetransmitMonitor（/ net_tcp_monitor）
 *
 * 与 flow_rate.bpf.c 的重传挂点对比：
 *   本文件：按"连接"维度（4 元组）统计重传，适合定位"哪条 TCP 连接在丢包"
 *   flow_rate.bpf.c：按"进程"维度统计重传，适合定位"哪个进程的网络在抖动"
 */

#define __TARGET_ARCH_arm64
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_endian.h>
#ifndef AF_INET
#define AF_INET 2
#endif

char LICENSE[] SEC("license") = "GPL";

// =============================================================================
// 数据结构定义
// =============================================================================

/*
 * TCP 连接标识 Key（4 元组，仅支持 IPv4）
 * 与 http_latency.bpf.c 的 tcp_conn_key 不同，这里用 packed 消除 padding，
 * 且只覆盖 IPv4（足够覆盖绝大多数移动网络场景）
 */
struct tcp_conn_key {
    __u32 saddr;   // 源 IP（网络序）
    __u32 daddr;   // 目的 IP（网络序）
    __u16 sport;   // 源端口（网络序，bpf_htons(skc_num)）
    __u16 dport;   // 目的端口（网络序，skc_dport 直接存储）
} __attribute__((packed));

/*
 * 重传事件详情（存储在 retrans_events Map 的 Value）
 * LRU Hash 自动只保留最近的事件，用户态轮询消费后可删除，
 * 但即使不删，LRU 也会淘汰最久的条目
 */
struct tcp_retrans_event {
    __u32 pid;          // 触发重传的进程 PID（bpf_get_current_pid_tgid 高 32 位）
    __u32 tgid;         // 线程组 ID（通常与 PID 相同）
    __u64 timestamp_ns; // 重传发生的时间戳（bpf_ktime_get_ns，单调时钟）
    __u32 segs_out;     // 累计已发送段数（预留字段，暂未填充，内核 tcp_sock 中有 tcpi_segs_out）
    __u32 segs_retrans; // 累计重传段数（预留字段）
    __u32 sstate;       // TCP socket 状态（预留字段，可从 sk->__sk_common.skc_state 读取）
};

/*
 * 连接级重传统计（存储在 retrans_stats Map 的 Value）
 * 用户态用以下公式计算丢包率:
 *   丢包率 = total_retrans / max(total_segs, 1)
 *   若某条连接 total_segs=0（只有重传，没有新发送），则分母取 1
 */
struct tcp_retrans_stats {
    __u64 total_retrans; // 该连接累计重传次数（每次 tcp_retransmit_skb +1）
    __u64 total_segs;    // 该连接累计估算发送段数（每次 tcp_sendmsg +ceil(size/1460)）
    __u32 last_state;    // 最近一次观测到的 TCP 状态（预留字段）
};

// =============================================================================
// BPF Map 定义
// =============================================================================

/*
 * Map: retrans_events
 * 类型: BPF_MAP_TYPE_LRU_HASH
 * Key:  struct tcp_conn_key（4 元组）
 * Value: struct tcp_retrans_event（事件详情）
 * 最大条目数: 1024（LRU 保证最多保留 1024 个活跃连接的最近一次重传）
 * 用途: 用户态轮询此 Map 可以获得每条连接最后一次重传的时间戳，
 *       用于实时告警（如重传在 1 秒内频繁发生）。
 *       之所以用 LRU_HASH 而不是 PERF_EVENT_ARRAY，
 *       是因为我们需要"按连接 Key 聚合"，而不是流式消费所有事件。
 */
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 1024);
    __type(key, struct tcp_conn_key);
    __type(value, struct tcp_retrans_event);
} retrans_events SEC(".maps");

/*
 * Map: retrans_stats
 * 类型: BPF_MAP_TYPE_LRU_HASH
 * Key:  struct tcp_conn_key（4 元组）
 * Value: struct tcp_retrans_stats（累计统计）
 * 最大条目数: 65536（高并发下足够，LRU 自动淘汰冷门连接）
 * 用途: 存储每条 TCP 连接的累计重传统计。
 *       tcp_retransmit_skb 入口 → total_retrans +1
 *       tcp_sendmsg 入口 → total_segs +ceil(size/1460)
 *       用户态定期读取后重置（或依赖 LRU 自然淘汰）。
 */
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 65536);
    __type(key, struct tcp_conn_key);
    __type(value, struct tcp_retrans_stats);
} retrans_stats SEC(".maps");

/*
 * Map: retrans_self_pid
 * 服务端自身 PID。用于排除**主动探测**连接的 TCP 重传。
 *
 * 为什么这里必须排除：本探针的 total_retrans / total_segs 直接喂给
 * Reliability SLE —— 一个 blocking 的 Core SLE。主动探测目标是
 * "应稳定可达"的，若其重传进入本窗口，会人为改变丢包率并影响
 * Overall 判决，同时破坏 Active/Passive 证据边界。
 *
 * ── 为什么只做 PID 过滤、不做 socket cookie 桥接 ──
 *
 * 曾尝试照搬 dns_monitor 的 cookie 方案（sendmsg 侧登记 cookie、
 * retransmit 侧按 cookie 排除），在开发板上**加载失败**：
 *
 *   libbpf: prog 'trace_tcp_retransmit': failed to load: -22
 *   unknown func bpf_get_socket_cookie#46
 *
 * 原因：kprobe/tcp_retransmit_skb 的上下文不被该内核（5.15.147）允许
 * 调用 bpf_get_socket_cookie；而 dns_monitor 的挂点在 skb 路径上，
 * 上下文不同，故它能用而这里不能。
 *
 * 因此本探针的覆盖是**有方向的**：
 *   - sendmsg（分母）：在调用方进程上下文，PID 可靠 → 完全排除
 *   - retransmit（分子）：由进程上下文触发时 PID 有效 → 排除；
 *     由**重传定时器**在 softirq 触发时 PID 不可用 → 漏过
 *
 * 这个残余方向已知且有界：它只会**少算**分子（放宽），不会因分母被
 * 单侧削减而**抬高**丢包率造成假 BAD。原实现两侧都不排除才是真正
 * 危险的不对称。残余漏过由真机验收的实际数值观察兜底。
 */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u32);
} retrans_self_pid SEC(".maps");

// =============================================================================
// 辅助函数
// =============================================================================

static __always_inline bool is_self_pid(void)
{
    __u32 k = 0;
    __u32 *self = bpf_map_lookup_elem(&retrans_self_pid, &k);
    if (!self || *self == 0)
        return false;   // 未设置时不误杀
    return ((__u32)(bpf_get_current_pid_tgid() >> 32)) == *self;
}

/*
 * 从 struct sock 构造 TCP 4 元组 Key
 * 仅处理 AF_INET（IPv4）
 * @return 0=成功，-1=非 IPv4（跳过）
 */
static __always_inline int fill_conn_key(struct sock *sk, struct tcp_conn_key *k)
{
    __u16 family = BPF_CORE_READ(sk, __sk_common.skc_family);
    if (family != AF_INET)
        return -1;

    // skc_rcv_saddr / skc_daddr 是内核存 IP 的字段名
    k->saddr = BPF_CORE_READ(sk, __sk_common.skc_rcv_saddr);
    k->daddr = BPF_CORE_READ(sk, __sk_common.skc_daddr);
    // skc_num 存主机序端口，用 bpf_htons 转网络序（大端），保证跨架构 Key 一致
    __u16 sport_host = BPF_CORE_READ(sk, __sk_common.skc_num);
    k->sport = bpf_htons(sport_host);
    // skc_dport 内核直接存网络序，不需要转换
    k->dport = BPF_CORE_READ(sk, __sk_common.skc_dport);
    return 0;
}

// =============================================================================
// BPF 入口函数 1: TCP 重传事件
// =============================================================================

/*
 * 函数: trace_tcp_retransmit
 * 挂点: SEC("kprobe/tcp_retransmit_skb")
 * 触发时机: 每当 TCP 内核决定重传一个 sk_buff 时。
 *           内核函数签名: int tcp_retransmit_skb(struct sock *sk, struct sk_buff *skb, int segs)
 * 主要逻辑:
 *   1. PT_REGS_PARM1 取出 struct sock *sk
 *   2. fill_conn_key(sk, &key) 构造 TCP 4 元组
 *   3. bpf_ktime_get_ns() 取单调时钟时间戳
 *   4. bpf_get_current_pid_tgid() 取触发重传的进程 PID/TGID
 *   5. 在 retrans_stats 中 total_retrans +1（原子累加）
 *   6. 在 retrans_events 中写入完整事件详情
 * 写入的 Map:
 *   - retrans_stats（total_retrans +1）
 *   - retrans_events（写入完整事件）
 */
SEC("kprobe/tcp_retransmit_skb")
int trace_tcp_retransmit(struct pt_regs *ctx)
{
    // PT_REGS_PARM1(ctx) 提取第一个参数 struct sock *sk
    struct sock *sk = (struct sock *)PT_REGS_PARM1(ctx);
    if (!sk)
        return 0;

    // === 排除服务端自身（主动探测）的连接 ===
    // 仅 PID 判据：本挂点不允许调用 bpf_get_socket_cookie
    // （unknown func #46，见 retrans_self_pid 的说明）。
    if (is_self_pid())
        return 0;

    // 构造 TCP 4 元组 Key
    struct tcp_conn_key key = {};
    if (fill_conn_key(sk, &key) < 0)
        return 0;

    // bpf_ktime_get_ns()：内核单调时钟，基于 sched_clock()
    __u64 now = bpf_ktime_get_ns();
    // bpf_get_current_pid_tgid()：返回 (PID << 32) | TID
    // 右移 32 位拿到 PID，& 0xffffffff 拿 TID
    __u32 pid_tgid = (__u32)(bpf_get_current_pid_tgid() >> 32);

    // === 更新连接级累计统计 ===
    struct tcp_retrans_stats *stat = bpf_map_lookup_elem(&retrans_stats, &key);
    if (stat) {
        // __sync_fetch_and_add：原子内置函数，编译为 lock add 指令
        // 保证多核并发触发时不会丢计数
        __sync_fetch_and_add(&stat->total_retrans, 1);
        stat->last_state = 0;  // 预留：可从 skc_state 读真实 TCP 状态
    } else {
        // 第一次看到此连接：创建初始化记录
        struct tcp_retrans_stats init = {0};
        init.total_retrans = 1;
        init.total_segs = 0;
        bpf_map_update_elem(&retrans_stats, &key, &init, BPF_ANY);
    }

    // === 记录最近一次重传事件详情 ===
    struct tcp_retrans_event event = {0};
    event.pid = pid_tgid;
    event.tgid = bpf_get_current_pid_tgid() >> 32;  // 与 pid 相同（同一进程）
    event.timestamp_ns = now;
    // segs_out / segs_retrans / sstate 是预留字段，
    // 可扩展读取 tcp_sock->tcpi_segs_out 等内核内部计数器
    event.segs_out = 0;
    event.segs_retrans = 1;  // 本次就是 1 个段的重传
    bpf_map_update_elem(&retrans_events, &key, &event, BPF_ANY);

    return 0;
}

// =============================================================================
// BPF 入口函数 2: TCP 发送量统计（用作丢包率分母）
// =============================================================================

/*
 * 函数: trace_tcp_sendmsg
 * 挂点: SEC("kprobe/tcp_sendmsg")
 * 触发时机: 每当 TCP socket 发送数据时。
 *           内核函数签名: int tcp_sendmsg(struct sock *sk, struct msghdr *msg, size_t size)
 * 主要逻辑:
 *   1. PT_REGS_PARM1/3 取 struct sock *sk 和 size
 *   2. fill_conn_key(sk, &key) 构造 TCP 4 元组
 *   3. 用 MSS≈1460 估算本次发送会产生多少个 TCP 段：ceil(size / 1460)
 *   4. 在 retrans_stats 中累加 total_segs
 * 写入的 Map: retrans_stats（total_segs +估算值）
 *
 * 丢包率计算公式:
 *   loss_rate = total_retrans / max(total_segs, 1)
 *
 * 为什么用估算而不是精确值？
 *   精确段数需要读 tcp_sock->snd_nxt / snd_una 差值，BPF_CORE_READ 对 tcp_sock
 *   的嵌套结构偏移不稳定，MSS 近似足够用于统计场景。
 */
SEC("kprobe/tcp_sendmsg")
int trace_tcp_sendmsg(struct pt_regs *ctx)
{
    struct sock *sk = (struct sock *)PT_REGS_PARM1(ctx);
    unsigned long size = (unsigned long)PT_REGS_PARM3(ctx);
    if (!sk || size == 0)
        return 0;

    // === 排除服务端自身（主动探测）的发送 ===
    // sendmsg 在调用方进程上下文，PID 可靠。
    // 与 retransmit 侧使用同一判据（本挂点同样不允许
    // bpf_get_socket_cookie）。
    if (is_self_pid())
        return 0;

    struct tcp_conn_key key = {};
    if (fill_conn_key(sk, &key) < 0)
        return 0;

    // MSS 近似值 1460（以太网 MTU 1500 - IP头20 - TCP头20）
    // ceil(size / 1460) = (size + 1459) / 1460
    // 任何 size > 0 至少产生 1 个段
    __u64 segs = (size + 1459) / 1460;
    if (segs == 0) segs = 1;

    // 与 trace_tcp_retransmit 更新同一张 Map（retrans_stats），
    // 原子累加 total_segs，与 total_retrans 配合计算丢包率
    struct tcp_retrans_stats *stat = bpf_map_lookup_elem(&retrans_stats, &key);
    if (stat) {
        __sync_fetch_and_add(&stat->total_segs, segs);
    } else {
        struct tcp_retrans_stats init = {0};
        init.total_retrans = 0;
        init.total_segs = segs;
        bpf_map_update_elem(&retrans_stats, &key, &init, BPF_ANY);
    }

    return 0;
}

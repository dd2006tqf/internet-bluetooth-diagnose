// tcp_retransmit.bpf.c
// TCP 重传追踪 eBPF 程序
// 挂载 kprobe/tcp_retransmit_skb，记录每次 TCP 重传事件
// 用于替代 net_tcp.cpp 的低效 netlink dump 方案
//
// 数据结构：
//   retrans_events: LRU_HASH，存储最近的重传事件，用户态轮询消费
//   retrans_stats: LRU_HASH，按连接聚合重传统计，高效计算丢包率

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

// ---- 数据结构 ----

// TCP 连接标识（5-tuple）
struct tcp_conn_key {
    __u32 saddr;
    __u32 daddr;
    __u16 sport;
    __u16 dport;
} __attribute__((packed));

// 重传事件（LRU，用户态轮询消费）
struct tcp_retrans_event {
    __u32 pid;
    __u32 tgid;
    __u64 timestamp_ns;
    __u32 segs_out;
    __u32 segs_retrans;
    __u32 sstate;     // TCP 状态
};

// 连接级重传统计（PERCPU，高效聚合）
struct tcp_retrans_stats {
    __u64 total_retrans;
    __u64 total_segs;
    __u32 last_state;
};

// ---- BPF Map ----

// 重传事件环形缓冲区（LRU，最近 1024 条）
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 1024);
    __type(key, struct tcp_conn_key);
    __type(value, struct tcp_retrans_event);
} retrans_events SEC(".maps");

// 连接级统计（PERCPU，高效读取）
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 65536);
    __type(key, struct tcp_conn_key);
    __type(value, struct tcp_retrans_stats);
} retrans_stats SEC(".maps");

// ---- 辅助函数 ----

static __always_inline int fill_conn_key(struct sock *sk, struct tcp_conn_key *k)
{
    __u16 family = BPF_CORE_READ(sk, __sk_common.skc_family);
    if (family != AF_INET)
        return -1;

    k->saddr = BPF_CORE_READ(sk, __sk_common.skc_rcv_saddr);
    k->daddr = BPF_CORE_READ(sk, __sk_common.skc_daddr);
    __u16 sport_host = BPF_CORE_READ(sk, __sk_common.skc_num);
    k->sport = bpf_htons(sport_host);
    k->dport = BPF_CORE_READ(sk, __sk_common.skc_dport);
    return 0;
}

// ---- 挂点 ----

// kprobe: int tcp_retransmit_skb(struct sock *sk, struct sk_buff *skb, int segs)
// 每次 TCP 重传时内核调用此函数
SEC("kprobe/tcp_retransmit_skb")
int trace_tcp_retransmit(struct pt_regs *ctx)
{
    struct sock *sk = (struct sock *)PT_REGS_PARM1(ctx);
    if (!sk)
        return 0;

    struct tcp_conn_key key = {};
    if (fill_conn_key(sk, &key) < 0)
        return 0;

    __u64 now = bpf_ktime_get_ns();
    __u32 pid_tgid = (__u32)(bpf_get_current_pid_tgid() >> 32);

    // 更新统计
    struct tcp_retrans_stats *stat = bpf_map_lookup_elem(&retrans_stats, &key);
    if (stat) {
        __sync_fetch_and_add(&stat->total_retrans, 1);
        stat->last_state = 0;
    } else {
        struct tcp_retrans_stats init = {0};
        init.total_retrans = 1;
        init.total_segs = 0;
        bpf_map_update_elem(&retrans_stats, &key, &init, BPF_ANY);
    }

    // 记录事件
    struct tcp_retrans_event event = {0};
    event.pid = pid_tgid;
    event.tgid = bpf_get_current_pid_tgid() >> 32;
    event.timestamp_ns = now;
    event.segs_out = 0;
    event.segs_retrans = 1;
    bpf_map_update_elem(&retrans_events, &key, &event, BPF_ANY);

    return 0;
}

// kprobe: int tcp_sendmsg(struct sock *sk, struct msghdr *msg, size_t size)
// 统计发送段数，用于计算丢包率分母
SEC("kprobe/tcp_sendmsg")
int trace_tcp_sendmsg(struct pt_regs *ctx)
{
    struct sock *sk = (struct sock *)PT_REGS_PARM1(ctx);
    unsigned long size = (unsigned long)PT_REGS_PARM3(ctx);
    if (!sk || size == 0)
        return 0;

    struct tcp_conn_key key = {};
    if (fill_conn_key(sk, &key) < 0)
        return 0;

    // 估算段数（MSS ≈ 1460）
    __u64 segs = (size + 1459) / 1460;
    if (segs == 0) segs = 1;

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

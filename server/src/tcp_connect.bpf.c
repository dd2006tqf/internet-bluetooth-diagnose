/*
 * 文件: tcp_connect.bpf.c
 * 功能: TCP 建连（Connect）生命周期捕获。
 *
 * 挂点选择（单一来源原则，与 DNS 捕获一致）：
 *   tracepoint/sock/inet_sock_set_state
 *   该 tracepoint 在**同一次事件**中同时提供：
 *     - 连接四元组 saddr/sport/daddr/dport（endpoint）
 *     - oldstate/newstate（状态迁移，用于判定成功/失败）
 *   因此不需要任何跨 hook 关联或 pending 状态。
 *
 * 判定的状态迁移：
 *   SYN_SENT → ESTABLISHED   建连成功
 *   SYN_SENT → CLOSE         建连失败（拒绝/超时/不可达）
 *
 * 为什么不用 kprobe/tcp_connect 或 kretprobe/tcp_v4_connect：
 *   tcp_connect 只给出 attempt，不给结果；kretprobe 只给返回值，
 *   拿不到后续的握手结果。状态迁移 tracepoint 才是既有 endpoint
 *   又有结果的单一语义源。
 *
 * 用户态消费者: TcpConnectMonitor（经 perf buffer）
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

/* Linux TCP 状态（include/net/tcp_states.h） */
#define TCP_ESTABLISHED 1
#define TCP_SYN_SENT    2
#define TCP_CLOSE       7

#define TCP_CONNECT_MAX_TRACKED 4096

/*
 * TCP 建连事件（userspace ABI）。
 * 只上报建连相关的状态迁移，避免把整条 TCP 生命周期灌进 perf buffer。
 */
struct tcp_connect_event {
    __u32 saddr;        /* 本机地址（原始网络序字节） */
    __u32 daddr;        /* 对端地址 */
    __u16 sport;        /* 本机端口（主机序数值） */
    __u16 dport;        /* 对端端口 */
    __u8  old_state;
    __u8  new_state;
    __u8  is_ipv4;
    __u8  reserved;
    __u64 timestamp_ns;
};

enum tcp_connect_stat {
    TCP_CONN_STAT_ENTER = 0,
    TCP_CONN_STAT_NON_IPV4,
    TCP_CONN_STAT_SYN_SENT,
    TCP_CONN_STAT_ESTABLISHED,
    TCP_CONN_STAT_CLOSED_FROM_SYN_SENT,
    TCP_CONN_STAT_EMITTED,
    TCP_CONN_STAT_EMIT_FAIL,
    TCP_CONN_STAT_OTHER_TRANSITION,
    TCP_CONN_STAT_MAX
};

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, TCP_CONN_STAT_MAX);
    __type(key, __u32);
    __type(value, __u64);
} tcp_connect_counters SEC(".maps");

static __always_inline void tcp_stat_inc(__u32 key)
{
    __u64 *v = bpf_map_lookup_elem(&tcp_connect_counters, &key);
    if (v)
        (*v)++;
}

struct {
    __uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
    __type(key, __u32);
    __type(value, __u32);
} tcp_connect_events SEC(".maps");

char LICENSE[] SEC("license") = "GPL";

SEC("tracepoint/sock/inet_sock_set_state")
int trace_tcp_connect(struct trace_event_raw_inet_sock_set_state *ctx)
{
    tcp_stat_inc(TCP_CONN_STAT_ENTER);

    if (ctx->family != AF_INET) {
        tcp_stat_inc(TCP_CONN_STAT_NON_IPV4);
        return 0;
    }
    if (ctx->protocol != IPPROTO_TCP)
        return 0;

    __u8 old_state = (__u8)ctx->oldstate;
    __u8 new_state = (__u8)ctx->newstate;

    // 只关心建连相关的迁移
    bool is_syn_sent = (new_state == TCP_SYN_SENT);
    bool is_established_from_syn_sent = (old_state == TCP_SYN_SENT) && (new_state == TCP_ESTABLISHED);
    bool is_closed_from_syn_sent = (old_state == TCP_SYN_SENT) && (new_state == TCP_CLOSE);

    if (!is_syn_sent && !is_established_from_syn_sent && !is_closed_from_syn_sent) {
        tcp_stat_inc(TCP_CONN_STAT_OTHER_TRANSITION);
        return 0;
    }

    if (is_syn_sent) tcp_stat_inc(TCP_CONN_STAT_SYN_SENT);
    else if (is_established_from_syn_sent) tcp_stat_inc(TCP_CONN_STAT_ESTABLISHED);
    else tcp_stat_inc(TCP_CONN_STAT_CLOSED_FROM_SYN_SENT);

    struct tcp_connect_event ev = {};
    // 四元组与状态迁移同源（本次 tracepoint），无跨 hook 关联
    __builtin_memcpy(&ev.saddr, ctx->saddr, 4);
    __builtin_memcpy(&ev.daddr, ctx->daddr, 4);
    ev.sport = ctx->sport;   // tracepoint 已提供主机序
    ev.dport = ctx->dport;
    ev.old_state = old_state;
    ev.new_state = new_state;
    ev.is_ipv4 = 1;
    ev.timestamp_ns = bpf_ktime_get_ns();

    long ret = bpf_perf_event_output(ctx, &tcp_connect_events, BPF_F_CURRENT_CPU,
                                     &ev, sizeof(ev));
    if (ret < 0)
        tcp_stat_inc(TCP_CONN_STAT_EMIT_FAIL);
    else
        tcp_stat_inc(TCP_CONN_STAT_EMITTED);
    return 0;
}

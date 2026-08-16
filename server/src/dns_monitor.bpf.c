// dns_monitor.bpf.c
// DNS 解析监控 eBPF 程序
// 挂载 kprobe/udp_sendmsg + kprobe/udp_recvmsg，过滤端口 53 的 UDP 包
// 计算 DNS 解析延迟，检测超时和失败
//
// 数据结构：
//   dns_queries: LRU_HASH，存储进行中的 DNS 查询，收到响应时更新
//   dns_stats: PERCPU_HASH，聚合 DNS 延迟统计

#define __TARGET_ARCH_arm64
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_endian.h>
#ifndef AF_INET
#define AF_INET 2
#endif

#define DNS_PORT 53
#define DNS_TIMEOUT_NS 5000000000ULL  // 5 秒

char LICENSE[] SEC("license") = "GPL";

// ---- 数据结构 ----

// DNS 查询标识
struct dns_query_key {
    __u32 saddr;
    __u32 daddr;
    __u16 sport;
} __attribute__((packed));

// DNS 查询记录
struct dns_query_record {
    __u64 send_time_ns;
    __u64 recv_time_ns;   // 0 = 未收到响应（超时）
    __u32 reply_len;
    __u8  rcode;          // DNS 响应码
    __u8  is_response;    // 0=请求 1=响应
};

// DNS 聚合统计（PERCPU）
struct dns_stats_record {
    __u64 total_queries;
    __u64 total_responses;
    __u64 total_timeouts;
    __u64 total_errors;     // rcode != 0
    __u64 total_latency_ns; // 延迟总和（用于算平均）
    __u64 max_latency_ns;
};

// ---- BPF Map ----

// 进行中的 DNS 查询
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 256);
    __type(key, struct dns_query_key);
    __type(value, struct dns_query_record);
} dns_queries SEC(".maps");

// 聚合统计
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1);
    __type(key, __u32); // 固定 key=0
    __type(value, struct dns_stats_record);
} dns_stats SEC(".maps");

// ---- 辅助函数 ----

static __always_inline bool is_dns_port(__u16 port)
{
    return port == DNS_PORT || port == bpf_htons(DNS_PORT);
}

static __always_inline void update_dns_stats(__u64 latency_ns, bool is_timeout, bool is_error)
{
    __u32 key = 0;
    struct dns_stats_record *stat = bpf_map_lookup_elem(&dns_stats, &key);
    if (!stat) {
        struct dns_stats_record init = {0};
        init.total_queries = 1;
        init.total_responses = is_timeout ? 0 : 1;
        init.total_timeouts = is_timeout ? 1 : 0;
        init.total_errors = is_error ? 1 : 0;
        init.total_latency_ns = is_timeout ? 0 : latency_ns;
        init.max_latency_ns = is_timeout ? 0 : latency_ns;
        bpf_map_update_elem(&dns_stats, &key, &init, BPF_ANY);
    } else {
        __sync_fetch_and_add(&stat->total_queries, 1);
        if (!is_timeout) {
            __sync_fetch_and_add(&stat->total_responses, 1);
            __sync_fetch_and_add(&stat->total_latency_ns, latency_ns);
            if (latency_ns > stat->max_latency_ns)
                stat->max_latency_ns = latency_ns;
        } else {
            __sync_fetch_and_add(&stat->total_timeouts, 1);
        }
        if (is_error)
            __sync_fetch_and_add(&stat->total_errors, 1);
    }
}

// ---- 挂点 1: DNS 请求发送 ----

SEC("kprobe/udp_sendmsg")
int trace_dns_send(struct pt_regs *ctx)
{
    struct sock *sk = (struct sock *)PT_REGS_PARM1(ctx);
    if (!sk)
        return 0;

    __u16 family = BPF_CORE_READ(sk, __sk_common.skc_family);
    if (family != AF_INET)
        return 0;

    __u16 sport = BPF_CORE_READ(sk, __sk_common.skc_num);
    __u16 dport = BPF_CORE_READ(sk, __sk_common.skc_dport);

    // 过滤目的端口 53
    if (!is_dns_port(dport))
        return 0;

    __u32 saddr = BPF_CORE_READ(sk, __sk_common.skc_rcv_saddr);
    __u32 daddr = BPF_CORE_READ(sk, __sk_common.skc_daddr);

    struct dns_query_key key = {0};
    key.saddr = saddr;
    key.daddr = daddr;
    key.sport = bpf_htons(sport);

    // 记录发送时间
    struct dns_query_record rec = {0};
    rec.send_time_ns = bpf_ktime_get_ns();
    rec.is_response = 0;
    bpf_map_update_elem(&dns_queries, &key, &rec, BPF_ANY);

    // 发送请求即累计 total_queries，不依赖响应路径
    __u32 stats_key = 0;
    struct dns_stats_record *stat = bpf_map_lookup_elem(&dns_stats, &stats_key);
    if (stat) {
        __sync_fetch_and_add(&stat->total_queries, 1);
    } else {
        struct dns_stats_record init = {0};
        init.total_queries = 1;
        bpf_map_update_elem(&dns_stats, &stats_key, &init, BPF_ANY);
    }

    return 0;
}

// ---- 挂点 2: DNS 响应接收 ----

SEC("kprobe/udp_recvmsg")
int trace_dns_recv(struct pt_regs *ctx)
{
    struct sock *sk = (struct sock *)PT_REGS_PARM1(ctx);
    if (!sk)
        return 0;

    __u16 family = BPF_CORE_READ(sk, __sk_common.skc_family);
    if (family != AF_INET)
        return 0;

    __u16 sport = BPF_CORE_READ(sk, __sk_common.skc_num);
    __u16 dport = BPF_CORE_READ(sk, __sk_common.skc_dport);

    // 响应对端是 DNS 服务器 (端口53)，本机是随机端口
    // 需同时检查 dport==53（服务器端 socket）或 sport==53（客户端 socket收到响应）
    bool is_server_side = is_dns_port(dport);
    if (!is_server_side && !is_dns_port(sport))
        return 0;

    __u32 saddr = BPF_CORE_READ(sk, __sk_common.skc_rcv_saddr);
    __u32 daddr = BPF_CORE_READ(sk, __sk_common.skc_daddr);

    // 反转方向：响应的 sport=53, dport=客户端端口
    struct dns_query_key key = {0};
    // 服务器端 socket：saddr=服务器, daddr=客户端
    // 客户端 socket：saddr=客户端, daddr=服务器
    if (is_server_side) {
        // 客户端 socket 接收 DNS 响应：skc_rcv_saddr=本机, skc_daddr=DNS服务器
        // 与发送路径对齐：saddr=本机IP, daddr=DNS服务器IP
        key.saddr = saddr;
        key.daddr = daddr;
        key.sport = bpf_htons(sport);
    } else {
        // DNS 服务器 socket 接收请求：不参与客户端 DNS 请求追踪
        key.saddr = daddr;
        key.daddr = saddr;
        key.sport = bpf_htons(sport);
    }

    struct dns_query_record *rec = bpf_map_lookup_elem(&dns_queries, &key);
    if (!rec || rec->send_time_ns == 0)
        return 0;

    // 计算延迟
    __u64 now = bpf_ktime_get_ns();
    __u64 latency = now - rec->send_time_ns;

    // 更新记录
    rec->recv_time_ns = now;
    rec->is_response = 1;
    rec->reply_len = 0;

    // 更新统计
    update_dns_stats(latency, false, false);

    // 清理查询记录
    bpf_map_delete_elem(&dns_queries, &key);

    return 0;
}

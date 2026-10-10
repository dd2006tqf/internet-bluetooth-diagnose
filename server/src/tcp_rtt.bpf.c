/*
 * 文件: tcp_rtt.bpf.c
 * 功能: 被动 TCP 往返时延（RTT）与传输拥塞观测。
 *       通过挂载内核 tracepoint/tcp/tcp_probe，从真实业务 TCP 连接的 ACK
 *       处理路径中直接读取内核维护的平滑往返时间（srtt）与拥塞窗口（snd_cwnd），
 *       无需额外发起任何 ICMP/TCP 探测数据包即可评估端到端传输响应延迟。
 *
 * 挂载点:
 *   - tracepoint/tcp/tcp_probe (struct trace_event_raw_tcp_probe)
 *
 * 聚合策略（关键设计）:
 *   tcp_probe 在高吞吐业务下触发极其密集。本程序严格在内核 BPF Map 侧按
 *   4 元组连接维度（saddr, daddr, sport, dport）做指标累加与快照更新，
 *   避免将海量高频事件推送到用户态造成 perf_buffer/ringbuf 溢出。
 *
 * 使用的 BPF Map:
 *   - tcp_rtt_stats : LRU_HASH，key=struct tcp_conn_key, value=struct tcp_rtt_stat
 *   - tcp_rtt_global: ARRAY(1)，全局聚合（总样本数、延迟累加和、极值）
 *   - tcp_rtt_self_pid: ARRAY(1)，自身进程 PID 过滤
 *
 * 用户态对应的 Monitor 类: TcpRttMonitor
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

#ifndef AF_INET6
#define AF_INET6 10
#endif

char LICENSE[] SEC("license") = "GPL";

/// 连接标识四元组
struct tcp_conn_key {
    __u32 saddr;
    __u32 daddr;
    __u16 sport;
    __u16 dport;
    __u16 family;
    __u16 pad;
};

/// 单连接 RTT 统计状态
struct tcp_rtt_stat {
    __u32 srtt_us;           ///< 最近一次平滑 RTT（微秒）
    __u32 min_rtt_us;        ///< 该连接观测到的最小 RTT（微秒）
    __u32 max_rtt_us;        ///< 该连接观测到的最大 RTT（微秒）
    __u32 snd_cwnd;          ///< 拥塞窗口（段数）
    __u32 snd_wnd;           ///< 发送窗口字节数
    __u32 rcv_wnd;           ///< 接收窗口字节数
    __u64 sample_count;      ///< 该连接捕获的 probe 次数
    __u64 last_timestamp_ns; ///< 最近采样时间戳 (CLOCK_MONOTONIC)
    __u64 sock_cookie;       ///< 套接字唯一标识
};

/// 全局聚合统计
struct tcp_rtt_global_stat {
    __u64 total_samples;     ///< 全局样本总数
    __u64 total_conns;       ///< 累计跟踪连接数
    __u64 srtt_sum_us;       ///< RTT 累加和（用于计算平均值）
    __u32 min_srtt_us;       ///< 全局最小 RTT
    __u32 max_srtt_us;       ///< 全局最大 RTT
};

#define TCP_RTT_DEFAULT_ENTRIES 8192

struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, TCP_RTT_DEFAULT_ENTRIES);
    __type(key, struct tcp_conn_key);
    __type(value, struct tcp_rtt_stat);
} tcp_rtt_stats SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct tcp_rtt_global_stat);
} tcp_rtt_global SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u32);
} tcp_rtt_self_pid SEC(".maps");

SEC("tracepoint/tcp/tcp_probe")
int trace_tcp_probe(struct trace_event_raw_tcp_probe *ctx)
{
    // 自采样流量过滤：排除 WeakNet 服务端自身发起的连通性探测流量
    __u32 pid_key = 0;
    __u32 *self_pid = bpf_map_lookup_elem(&tcp_rtt_self_pid, &pid_key);
    if (self_pid && *self_pid != 0) {
        __u64 pid_tgid = bpf_get_current_pid_tgid();
        __u32 current_pid = (__u32)(pid_tgid >> 32);
        if (current_pid == *self_pid) {
            return 0;
        }
    }

    __u16 family = ctx->family;
    // 第一阶段优先支持 IPv4 连接统计
    if (family != AF_INET) {
        return 0;
    }

    // 从 trace_event_raw_tcp_probe 提取四元组
    //
    // 字段布局说明：内核 trace_tcp_probe() 通过 TP_fast_assign 把
    // sockaddr_storage(28B) 整块拷贝到 saddr/daddr，因此：
    //   +0..1  = ss_family (AF_INET=2)
    //   +2..3  = sin_port (big-endian)
    //   +4..7  = sin_addr (big-endian IPv4)
    // 端口号以 sockaddr_in.sin_port（偏移 2）为准，不用 ctx->sport/dport
    // ——这两个 tracepoint 字段在部分内核版本上语义不稳定。
    //
    // 注意：用 ctx->saddr[N] 数组下标访问会产生「ctx + 常量偏移」的访存，
    // 校验器与 CO-RE 都接受；绝不要写 (u8*)ctx + off 或 memcpy(ctx->saddr+N)
    // ——后者是修改后的 ctx 指针解引用，校验器报
    // "dereference of modified ctx ptr disallowed" 直接拒绝加载。
    struct tcp_conn_key key = {};
    key.family = family;

    __u16 sport_be = (((__u16)ctx->saddr[2]) << 8) | ctx->saddr[3];
    __u16 dport_be = (((__u16)ctx->daddr[2]) << 8) | ctx->daddr[3];
    key.sport = sport_be;
    key.dport = dport_be;

    // IPv4 地址 = sockaddr_in.sin_addr @ sockaddr offset 4，网络字节序原样保留
    key.saddr = (((__u32)ctx->saddr[4])       ) |
                (((__u32)ctx->saddr[5]) <<  8) |
                (((__u32)ctx->saddr[6]) << 16) |
                (((__u32)ctx->saddr[7]) << 24);
    key.daddr = (((__u32)ctx->daddr[4])       ) |
                (((__u32)ctx->daddr[5]) <<  8) |
                (((__u32)ctx->daddr[6]) << 16) |
                (((__u32)ctx->daddr[7]) << 24);

    // 内核计算得到的平滑 RTT：tp->srtt_us >> 3
    __u32 rtt_us = ctx->srtt;
    if (rtt_us == 0) {
        return 0; // 尚未完成一次有效 RTT 测算的连接暂不计入
    }

    __u64 now = bpf_ktime_get_ns();

    // 更新连接级状态
    struct tcp_rtt_stat *stat = bpf_map_lookup_elem(&tcp_rtt_stats, &key);
    if (!stat) {
        struct tcp_rtt_stat init_stat = {};
        init_stat.srtt_us = rtt_us;
        init_stat.min_rtt_us = rtt_us;
        init_stat.max_rtt_us = rtt_us;
        init_stat.snd_cwnd = ctx->snd_cwnd;
        init_stat.snd_wnd = ctx->snd_wnd;
        init_stat.rcv_wnd = ctx->rcv_wnd;
        init_stat.sample_count = 1;
        init_stat.last_timestamp_ns = now;
        init_stat.sock_cookie = ctx->sock_cookie;
        bpf_map_update_elem(&tcp_rtt_stats, &key, &init_stat, BPF_ANY);

        // 全局连接数递增
        __u32 g_key = 0;
        struct tcp_rtt_global_stat *g_stat = bpf_map_lookup_elem(&tcp_rtt_global, &g_key);
        if (g_stat) {
            __sync_fetch_and_add(&g_stat->total_conns, 1);
        }
    } else {
        stat->srtt_us = rtt_us;
        if (rtt_us < stat->min_rtt_us) stat->min_rtt_us = rtt_us;
        if (rtt_us > stat->max_rtt_us) stat->max_rtt_us = rtt_us;
        stat->snd_cwnd = ctx->snd_cwnd;
        stat->snd_wnd = ctx->snd_wnd;
        stat->rcv_wnd = ctx->rcv_wnd;
        stat->last_timestamp_ns = now;
        stat->sock_cookie = ctx->sock_cookie;
        __sync_fetch_and_add(&stat->sample_count, 1);
    }

    // 全局指标累加
    __u32 g_key = 0;
    struct tcp_rtt_global_stat *g_stat = bpf_map_lookup_elem(&tcp_rtt_global, &g_key);
    if (g_stat) {
        __sync_fetch_and_add(&g_stat->total_samples, 1);
        __sync_fetch_and_add(&g_stat->srtt_sum_us, (__u64)rtt_us);
        if (g_stat->min_srtt_us == 0 || rtt_us < g_stat->min_srtt_us) {
            g_stat->min_srtt_us = rtt_us;
        }
        if (rtt_us > g_stat->max_srtt_us) {
            g_stat->max_srtt_us = rtt_us;
        }
    }

    return 0;
}

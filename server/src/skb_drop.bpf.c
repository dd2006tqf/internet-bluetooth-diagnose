/*
 * 文件: skb_drop.bpf.c
 * 功能: 内核 Socket / 协议栈丢包原因精确归因。
 *       挂载 tracepoint/skb/kfree_skb，捕获带有 enum skb_drop_reason 的数据包释放事件，
 *       按协议类型与丢包原因码在内核中聚合统计，帮助定位丢包根因（防火墙、无套接字、校验和错误等）。
 *
 * 挂载点:
 *   - tracepoint/skb/kfree_skb (trace_event_raw_kfree_skb)
 *
 * 使用的 BPF Map:
 *   - drop_stats_map: HASH 表，key=struct drop_key, value=struct drop_stat
 *
 * 用户态对应 Monitor 类: SkbDropMonitor
 */

#define __TARGET_ARCH_arm64
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_endian.h>

char LICENSE[] SEC("license") = "GPL";

struct drop_key {
    __u32 protocol; // ETH_P_IP, ETH_P_IPV6 等，或协议号
    __u32 reason;   // enum skb_drop_reason
};

struct drop_stat {
    __u64 count;
    __u64 last_timestamp_ns;
};

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 256);
    __type(key, struct drop_key);
    __type(value, struct drop_stat);
} drop_stats_map SEC(".maps");

SEC("tracepoint/skb/kfree_skb")
int trace_kfree_skb(struct trace_event_raw_kfree_skb *ctx)
{
    __u32 reason = (__u32)ctx->reason;
    if (reason <= SKB_DROP_REASON_NOT_SPECIFIED) {
        return 0; // 忽略未指定原因或非丢包释放
    }

    struct drop_key key = {};
    key.protocol = (__u32)ctx->protocol;
    key.reason = reason;

    struct drop_stat *stat = bpf_map_lookup_elem(&drop_stats_map, &key);
    if (!stat) {
        struct drop_stat init_stat = {};
        init_stat.count = 1;
        init_stat.last_timestamp_ns = bpf_ktime_get_ns();
        bpf_map_update_elem(&drop_stats_map, &key, &init_stat, BPF_ANY);
    } else {
        __sync_fetch_and_add(&stat->count, 1);
        stat->last_timestamp_ns = bpf_ktime_get_ns();
    }

    return 0;
}

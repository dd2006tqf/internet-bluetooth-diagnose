// wifi_packet_loss.bpf.c
// Wi-Fi/网卡收发丢包归因追踪 eBPF 程序
// 通过挂载收发路径 tracepoint，统计每个接口的发送/接收/丢弃/重试包数，
// 区分发送丢包与接收丢包，定位网络故障点
//
// 挂点：
//   tracepoint/net/netif_receive_skb  — 接收路径，统计每接口 rx 包/字节
//   tracepoint/net/net_dev_queue      — 发送入队，统计每接口 tx 包/字节
//   tracepoint/net/net_dev_xmit       — 发送完成/重试/丢弃，区分发送丢包

#define __TARGET_ARCH_arm64
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_endian.h>

char LICENSE[] SEC("license") = "GPL";

// ---- 数据结构 ----

// 接口收发统计（key = ifindex）
struct iface_packet_stats {
    __u64 rx_pkts;
    __u64 rx_bytes;
    __u64 tx_pkts;
    __u64 tx_bytes;
    __u64 tx_drops;      // 发送丢弃（NETDEV_TX_BUSY 或失败）
    __u64 tx_retries;    // 发送重试
};

// ---- BPF Map ----

// 每接口收发统计（PERCPU，避免锁竞争）
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_HASH);
    __uint(max_entries, 256);   // 最多监控 256 个接口
    __type(key, __u32);          // ifindex
    __type(value, struct iface_packet_stats);
} packet_stats SEC(".maps");

// ---- 辅助函数 ----

static __always_inline struct iface_packet_stats *get_stats(__u32 ifindex)
{
    struct iface_packet_stats *stats = bpf_map_lookup_elem(&packet_stats, &ifindex);
    if (!stats) {
        struct iface_packet_stats init = {};
        if (bpf_map_update_elem(&packet_stats, &ifindex, &init, BPF_ANY) != 0)
            return NULL;
        stats = bpf_map_lookup_elem(&packet_stats, &ifindex);
    }
    return stats;
}

// 从 sk_buff 提取网卡 ifindex
static __always_inline __u32 skb_ifindex(struct sk_buff *skb)
{
    if (!skb) return 0;
    struct net_device *dev = BPF_CORE_READ(skb, dev);
    if (!dev) return 0;
    return BPF_CORE_READ(dev, ifindex);
}

// ---- 挂点 1: 接收路径 ----

// tracepoint: void netif_receive_skb(struct sk_buff *skb)
// 用 tracepoint/net/... 普通挂法；事件第一个数据字段 skbaddr(指针)在通用头(8B)之后
// 偏移 8。libbpf 对 tracepoint 不做自动解参，需从事件结构按偏移读。
SEC("tracepoint/net/netif_receive_skb")
int trace_net_rx(struct trace_event_raw_netif_receive_skb *ctx)
{
    struct sk_buff *skb = NULL;
    bpf_probe_read_kernel(&skb, sizeof(skb), (void*)ctx + 8);  // skbaddr
    if (!skb) return 0;
    __u32 ifindex = skb_ifindex(skb);
    if (ifindex == 0) return 0;

    struct iface_packet_stats *stats = get_stats(ifindex);
    if (!stats) return 0;
    __sync_fetch_and_add(&stats->rx_pkts, 1);
    __u32 len = BPF_CORE_READ(skb, len);
    __sync_fetch_and_add(&stats->rx_bytes, len);
    return 0;
}

// ---- 挂点 2: 发送入队 ----

// tracepoint: void net_dev_queue(struct sk_buff *skb)
SEC("tracepoint/net/net_dev_queue")
int trace_net_tx_queue(struct trace_event_raw_net_dev_queue *ctx)
{
    struct sk_buff *skb = NULL;
    bpf_probe_read_kernel(&skb, sizeof(skb), (void*)ctx + 8);  // skbaddr
    if (!skb) return 0;
    __u32 ifindex = skb_ifindex(skb);
    if (ifindex == 0) return 0;

    struct iface_packet_stats *stats = get_stats(ifindex);
    if (!stats) return 0;
    __sync_fetch_and_add(&stats->tx_pkts, 1);
    __u32 len = BPF_CORE_READ(skb, len);
    __sync_fetch_and_add(&stats->tx_bytes, len);
    return 0;
}

// ---- 挂点 3: 发送完成/重试/丢弃 ----

// tracepoint: net_dev_xmit(struct sk_buff *skb, int rc, struct net_device *dev, unsigned int skb_len)
SEC("tracepoint/net/net_dev_xmit")
int trace_net_tx_xmit(struct trace_event_raw_net_dev_xmit *ctx)
{
    // 事件结构: ent(8B) + skbaddr(8B) + len(4B) + rc(4B)
    struct sk_buff *skb = NULL;
    int rc = 0;
    bpf_probe_read_kernel(&skb, sizeof(skb), (void*)ctx + 8);   // skbaddr
    bpf_probe_read_kernel(&rc, sizeof(rc), (void*)ctx + 20);    // rc
    __u32 ifindex = 0;
    if (skb) ifindex = skb_ifindex(skb);
    if (ifindex == 0) return 0;

    struct iface_packet_stats *stats = get_stats(ifindex);
    if (!stats) return 0;

    // rc == NETDEV_TX_OK(0) → 发送成功
    // rc == NETDEV_TX_BUSY(16) → 设备忙，发送重试
    // rc < 0 → 发送失败，计入丢弃
    if (rc == 16) {  // NETDEV_TX_BUSY
        __sync_fetch_and_add(&stats->tx_retries, 1);
    } else if (rc < 0) {
        __sync_fetch_and_add(&stats->tx_drops, 1);
    }
    return 0;
}


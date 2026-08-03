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

struct conn_key {
    __u32 saddr;
    __u32 daddr;
    __u16 sport;
    __u16 dport;
    __u8  protocol; // 6 TCP, 17 UDP
} __attribute__((packed));

struct flow_data {
    __u64 bytes;
    __u64 packets;
    __u32 pid;
};

// 进程级网络统计
struct process_net_stats {
    char  comm[16];        // 进程名
    __u64 tx_bytes;        // 发送字节数
    __u64 tx_packets;      // 发送包数
    __u64 retrans_count;   // TCP 重传次数
};

// 统计当前窗口数据
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 65536);
    __type(key, struct conn_key);
    __type(value, struct flow_data);
} current_sec SEC(".maps");

// 进程级统计（新增，用于进程网络画像）
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 65536);
    __type(key, __u32);            // pid
    __type(value, struct process_net_stats);
} process_stats SEC(".maps");

// 可选：绑定的网卡 ifindex（0 表示不过滤）
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1);
    __type(key, __u32); // 固定 key=0
    __type(value, __u32); // ifindex
} cfg_iface SEC(".maps");

static __always_inline bool iface_allowed(__u32 skb_ifindex)
{
    __u32 key = 0;
    __u32 *want = bpf_map_lookup_elem(&cfg_iface, &key);
    if (!want) return true; // 未配置则放行
    if (*want == 0) return true;
    return skb_ifindex == *want;
}

static __always_inline int fill_key_from_sock(struct sock *sk, __u8 proto, struct conn_key *k)
{
    __u16 family = BPF_CORE_READ(sk, __sk_common.skc_family);
    if (family != AF_INET)
        return -1; // 仅统计 IPv4

    k->saddr = BPF_CORE_READ(sk, __sk_common.skc_rcv_saddr);
    k->daddr = BPF_CORE_READ(sk, __sk_common.skc_daddr);
    __u16 sport_host = BPF_CORE_READ(sk, __sk_common.skc_num);
    k->sport = bpf_htons(sport_host);
    k->dport = BPF_CORE_READ(sk, __sk_common.skc_dport);
    k->protocol = proto;
    return 0;
}

static __always_inline void account_flow(struct conn_key *k, __u64 add_bytes)
{
    struct flow_data *v = bpf_map_lookup_elem(&current_sec, k);
    if (!v) {
        struct flow_data init = {0};
        init.bytes = add_bytes;
        init.packets = 1;
        init.pid = (__u32)(bpf_get_current_pid_tgid() & 0xffffffff);
        bpf_map_update_elem(&current_sec, k, &init, 0);
    } else {
        __sync_fetch_and_add(&v->bytes, add_bytes);
        __sync_fetch_and_add(&v->packets, 1);
        v->pid = (__u32)(bpf_get_current_pid_tgid() & 0xffffffff);
    }

    // 更新进程级统计
    __u32 pid = (__u32)(bpf_get_current_pid_tgid() & 0xffffffff);
    struct process_net_stats *ps = bpf_map_lookup_elem(&process_stats, &pid);
    if (!ps) {
        struct process_net_stats init = {};
        bpf_get_current_comm(init.comm, sizeof(init.comm));
        init.tx_bytes = add_bytes;
        init.tx_packets = 1;
        bpf_map_update_elem(&process_stats, &pid, &init, 0);
    } else {
        __sync_fetch_and_add(&ps->tx_bytes, add_bytes);
        __sync_fetch_and_add(&ps->tx_packets, 1);
    }
}

// kprobe: int tcp_retransmit_skb(struct sock *sk, struct sk_buff *skb, int segs)
// 每次 TCP 重传时累计到发起进程
SEC("kprobe/tcp_retransmit_skb")
int trace_tcp_retransmit(struct pt_regs *ctx)
{
    __u32 pid = (__u32)(bpf_get_current_pid_tgid() & 0xffffffff);
    struct process_net_stats *ps = bpf_map_lookup_elem(&process_stats, &pid);
    if (!ps) {
        struct process_net_stats init = {};
        bpf_get_current_comm(init.comm, sizeof(init.comm));
        init.retrans_count = 1;
        bpf_map_update_elem(&process_stats, &pid, &init, 0);
    } else {
        __sync_fetch_and_add(&ps->retrans_count, 1);
    }
    return 0;
}

// kprobe: __ip_queue_xmit(struct sock *sk, struct sk_buff *skb, ...)
SEC("kprobe/ip_queue_xmit")
int tcp_transmit_entry(struct pt_regs *ctx)
{
    struct sock *sk = (struct sock *)PT_REGS_PARM1(ctx);
    struct sk_buff *skb = (struct sk_buff *)PT_REGS_PARM2(ctx);
    if (!sk || !skb) return 0;

    // 网卡过滤：skb->dev->ifindex
    struct net_device *dev = BPF_CORE_READ(skb, dev);
    __u32 ifindex = dev ? BPF_CORE_READ(dev, ifindex) : 0;
    if (!iface_allowed(ifindex)) return 0;

    struct conn_key k = {};
    if (fill_key_from_sock(sk, 6 /*TCP*/, &k) < 0)
        return 0;

    __u32 len = BPF_CORE_READ(skb, len);
    account_flow(&k, len);
    return 0;
}

// kprobe: udp_sendmsg(struct sock *sk, struct msghdr *msg, size_t len)
SEC("kprobe/udp_sendmsg")
int udp_send_entry(struct pt_regs *ctx)
{
    struct sock *sk = (struct sock *)PT_REGS_PARM1(ctx);
    unsigned long len = (unsigned long)PT_REGS_PARM3(ctx);
    if (!sk) return 0;

    // UDP 无 skb，这里不做接口过滤（旧内核字段兼容性差），仍统计总量

    struct conn_key k = {};
    if (fill_key_from_sock(sk, 17 /*UDP*/, &k) < 0)
        return 0;

    account_flow(&k, (__u64)len);
    return 0;
}



#define __TARGET_ARCH_arm64
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>

char LICENSE[] SEC("license") = "GPL";

#define HCI_ACLDATA_PKT 0x02

struct acl_stat {
    __u64 tx_pkts;
    __u64 tx_bytes;
    __u64 rx_pkts;
    __u64 rx_bytes;
    __u64 non_acl_dropped;
    __u64 short_pkt_dropped;
};

// 以 handle (低 12 位) 为 key 聚合统计
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 64);
    __type(key, __u32);
    __type(value, struct acl_stat);
} hci_acl_stats SEC(".maps");

// 全局底噪与非 ACL 计数（Key 0: TX non-acl, Key 1: RX non-acl）
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 4);
    __type(key, __u32);
    __type(value, __u64);
} global_drop_stats SEC(".maps");

// 辅助递增全局 drop
static __always_inline void bump_global_drop(__u32 idx) {
    __u64 *val = bpf_map_lookup_elem(&global_drop_stats, &idx);
    if (val) {
        __sync_fetch_and_add(val, 1);
    }
}

// ----------------------------------------------------------------------------
// TX 挂点: kprobe/hci_send_frame(struct hci_dev *hdev, struct sk_buff *skb)
// ----------------------------------------------------------------------------
SEC("kprobe/hci_send_frame")
int BPF_KPROBE(probe_hci_send_frame, void *hdev, struct sk_buff *skb) {
    if (!skb) return 0;

    // 1. 检查报文类型: 蓝牙子系统将 pkt_type 存放在 skb->cb 中 (bt_cb(skb)->pkt_type)
    // 根据内核定义，bt_skb_cb.pkt_type 位于 skb->cb 的第 0 字节
    __u8 pkt_type = 0;
    if (bpf_probe_read_kernel(&pkt_type, sizeof(pkt_type), (const char *)skb + offsetof(struct sk_buff, cb)) < 0) {
        return 0;
    }

    if (pkt_type != HCI_ACLDATA_PKT) {
        bump_global_drop(0); // TX 非 ACL 丢弃计数
        return 0;
    }

    // 2. 检查 skb 长度与数据指针
    __u32 len = BPF_CORE_READ(skb, len);
    if (len < 4) {
        bump_global_drop(2); // TX 过短报文
        return 0;
    }

    const unsigned char *data = BPF_CORE_READ(skb, data);
    if (!data) return 0;

    // 3. 读取 HCI ACL Header
    __u16 raw_handle = 0;
    __u16 dlen = 0;
    if (bpf_probe_read_kernel(&raw_handle, sizeof(raw_handle), data) < 0) return 0;
    if (bpf_probe_read_kernel(&dlen, sizeof(dlen), data + 2) < 0) return 0;

    __u32 handle = raw_handle & 0x0FFF;

    // 4. 统计更新
    struct acl_stat *stat = bpf_map_lookup_elem(&hci_acl_stats, &handle);
    if (!stat) {
        struct acl_stat zero = {};
        bpf_map_update_elem(&hci_acl_stats, &handle, &zero, BPF_NOEXIST);
        stat = bpf_map_lookup_elem(&hci_acl_stats, &handle);
        if (!stat) return 0;
    }

    __sync_fetch_and_add(&stat->tx_pkts, 1);
    __sync_fetch_and_add(&stat->tx_bytes, dlen);
    return 0;
}

// ----------------------------------------------------------------------------
// RX 挂点: kprobe/hci_recv_frame(struct hci_dev *hdev, struct sk_buff *skb)
// ----------------------------------------------------------------------------
SEC("kprobe/hci_recv_frame")
int BPF_KPROBE(probe_hci_recv_frame, void *hdev, struct sk_buff *skb) {
    if (!skb) return 0;

    // 1. 检查报文类型
    __u8 pkt_type = 0;
    if (bpf_probe_read_kernel(&pkt_type, sizeof(pkt_type), (const char *)skb + offsetof(struct sk_buff, cb)) < 0) {
        return 0;
    }

    if (pkt_type != HCI_ACLDATA_PKT) {
        bump_global_drop(1); // RX 非 ACL 丢弃计数 (例如海量的 HCI Event)
        return 0;
    }

    // 2. 检查 skb 长度
    __u32 len = BPF_CORE_READ(skb, len);
    if (len < 4) {
        bump_global_drop(3); // RX 过短报文
        return 0;
    }

    const unsigned char *data = BPF_CORE_READ(skb, data);
    if (!data) return 0;

    // 3. 读取 HCI ACL Header
    __u16 raw_handle = 0;
    __u16 dlen = 0;
    if (bpf_probe_read_kernel(&raw_handle, sizeof(raw_handle), data) < 0) return 0;
    if (bpf_probe_read_kernel(&dlen, sizeof(dlen), data + 2) < 0) return 0;

    __u32 handle = raw_handle & 0x0FFF;

    // 4. 统计更新
    struct acl_stat *stat = bpf_map_lookup_elem(&hci_acl_stats, &handle);
    if (!stat) {
        struct acl_stat zero = {};
        bpf_map_update_elem(&hci_acl_stats, &handle, &zero, BPF_NOEXIST);
        stat = bpf_map_lookup_elem(&hci_acl_stats, &handle);
        if (!stat) return 0;
    }

    __sync_fetch_and_add(&stat->rx_pkts, 1);
    __sync_fetch_and_add(&stat->rx_bytes, dlen);
    return 0;
}

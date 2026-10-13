/*
 * 文件: server/src/bpf/bt_hci_traffic.bpf.c
 * 职责: 工业蓝牙 HCI ACL 数据通道观测（Phase 1 事实源）
 *
 * 核心设计:
 *   1. 双向对称采集:
 *      - TX: kprobe/hci_send_frame
 *      - RX: kprobe/hci_recv_frame
 *   2. 严格防御过滤:
 *      - 在解引用数据前，读取 skb->cb 的 pkt_type，仅放行 HCI_ACLDATA_PKT (0x02)
 *      - 严格校验 skb->len >= 4
 *   3. 精准事实提取:
 *      - handle: 提取低 12 位 (raw_handle & 0x0FFF)
 *      - bytes:  累加 dlen (ACL 净荷字节数，不计物理层/HCI 传输开销)
 *   4. 零音频语义、零硬编码结构体偏移，原生依赖内核网络核心 sk_buff (CO-RE 友好)
 */

#define __TARGET_ARCH_arm64
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>

char LICENSE[] SEC("license") = "GPL";

#define HCI_ACLDATA_PKT 0x02

struct bpf_acl_traffic_val {
    __u64 tx_packets;
    __u64 tx_bytes;
    __u64 rx_packets;
    __u64 rx_bytes;
    __u64 last_tx_ktime_ns;
    __u64 last_rx_ktime_ns;
    __u64 non_acl_dropped;
    __u64 short_pkt_dropped;
};

// 以 connection_handle (低 12 位，取值 0..4095) 为 Key 记录流量统计
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 64);
    __type(key, __u32);
    __type(value, struct bpf_acl_traffic_val);
} bt_acl_traffic SEC(".maps");

// 全局开关与未归类统计
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 4);
    __type(key, __u32);
    __type(value, __u64);
} bt_acl_global_stats SEC(".maps");

static __always_inline void bump_global_counter(__u32 idx, __u64 val) {
    __u64 *p = bpf_map_lookup_elem(&bt_acl_global_stats, &idx);
    if (p) {
        __sync_fetch_and_add(p, val);
    }
}

// ----------------------------------------------------------------------------
// TX 挂点: kprobe/hci_send_frame(struct hci_dev *hdev, struct sk_buff *skb)
// ----------------------------------------------------------------------------
SEC("kprobe/hci_send_frame")
int BPF_KPROBE(bt_hci_send_frame, void *hdev, struct sk_buff *skb) {
    if (!skb) return 0;

    // 1. 报文类型门控 (bt_cb(skb)->pkt_type 位于 skb->cb 第 0 字节)
    __u8 pkt_type = 0;
    if (bpf_probe_read_kernel(&pkt_type, sizeof(pkt_type),
                              (const char *)skb + offsetof(struct sk_buff, cb)) < 0) {
        return 0;
    }

    if (pkt_type != HCI_ACLDATA_PKT) {
        bump_global_counter(0, 1); // TX 非 ACL 过滤
        return 0;
    }

    // 2. 报文长度防御
    __u32 len = BPF_CORE_READ(skb, len);
    if (len < 4) {
        bump_global_counter(2, 1); // TX 短报文丢弃
        return 0;
    }

    const unsigned char *data = BPF_CORE_READ(skb, data);
    if (!data) return 0;

    // 3. 读取并解析 ACL 报头 (handle: 12bit, dlen: 16bit)
    __u16 raw_handle = 0;
    __u16 dlen = 0;
    if (bpf_probe_read_kernel(&raw_handle, sizeof(raw_handle), data) < 0) return 0;
    if (bpf_probe_read_kernel(&dlen, sizeof(dlen), data + 2) < 0) return 0;

    __u32 handle = raw_handle & 0x0FFF;

    // 提取控制器序号，构成复合 Key: (hci_id << 16) | handle
    __u16 hci_id = 0;
    if (hdev) {
        bpf_probe_read_kernel(&hci_id, sizeof(hci_id), (const char *)hdev + 64);
    }
    __u32 map_key = ((__u32)hci_id << 16) | handle;

    // 4. 更新复合 Key 统计
    struct bpf_acl_traffic_val *stat = bpf_map_lookup_elem(&bt_acl_traffic, &map_key);
    if (!stat) {
        struct bpf_acl_traffic_val init = {};
        bpf_map_update_elem(&bt_acl_traffic, &map_key, &init, BPF_NOEXIST);
        stat = bpf_map_lookup_elem(&bt_acl_traffic, &map_key);
        if (!stat) return 0;
    }

    __u64 now = bpf_ktime_get_ns();
    __sync_fetch_and_add(&stat->tx_packets, 1);
    __sync_fetch_and_add(&stat->tx_bytes, dlen);
    stat->last_tx_ktime_ns = now;

    return 0;
}

// ----------------------------------------------------------------------------
// RX 挂点: kprobe/hci_recv_frame(struct hci_dev *hdev, struct sk_buff *skb)
// ----------------------------------------------------------------------------
SEC("kprobe/hci_recv_frame")
int BPF_KPROBE(bt_hci_recv_frame, void *hdev, struct sk_buff *skb) {
    if (!skb) return 0;

    // 1. 报文类型门控
    __u8 pkt_type = 0;
    if (bpf_probe_read_kernel(&pkt_type, sizeof(pkt_type),
                              (const char *)skb + offsetof(struct sk_buff, cb)) < 0) {
        return 0;
    }

    if (pkt_type != HCI_ACLDATA_PKT) {
        bump_global_counter(1, 1); // RX 非 ACL 过滤 (如底噪 HCI Event)
        return 0;
    }

    // 2. 报文长度防御
    __u32 len = BPF_CORE_READ(skb, len);
    if (len < 4) {
        bump_global_counter(3, 1); // RX 短报文丢弃
        return 0;
    }

    const unsigned char *data = BPF_CORE_READ(skb, data);
    if (!data) return 0;

    // 3. 读取并解析 ACL 报头
    __u16 raw_handle = 0;
    __u16 dlen = 0;
    if (bpf_probe_read_kernel(&raw_handle, sizeof(raw_handle), data) < 0) return 0;
    if (bpf_probe_read_kernel(&dlen, sizeof(dlen), data + 2) < 0) return 0;

    __u32 handle = raw_handle & 0x0FFF;

    // 提取控制器序号，构成复合 Key: (hci_id << 16) | handle
    __u16 hci_id = 0;
    if (hdev) {
        bpf_probe_read_kernel(&hci_id, sizeof(hci_id), (const char *)hdev + 64);
    }
    __u32 map_key = ((__u32)hci_id << 16) | handle;

    // 4. 更新复合 Key 统计
    struct bpf_acl_traffic_val *stat = bpf_map_lookup_elem(&bt_acl_traffic, &map_key);
    if (!stat) {
        struct bpf_acl_traffic_val init = {};
        bpf_map_update_elem(&bt_acl_traffic, &map_key, &init, BPF_NOEXIST);
        stat = bpf_map_lookup_elem(&bt_acl_traffic, &map_key);
        if (!stat) return 0;
    }

    __u64 now = bpf_ktime_get_ns();
    __sync_fetch_and_add(&stat->rx_packets, 1);
    __sync_fetch_and_add(&stat->rx_bytes, dlen);
    stat->last_rx_ktime_ns = now;

    return 0;
}

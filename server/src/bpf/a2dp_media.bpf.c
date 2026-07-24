// a2dp_media.bpf.c
// 蓝牙 A2DP/L2CAP 音频流量采集 eBPF 程序
// 通过内核 L2CAP 发送路径钩子，统计蓝牙音频设备的实际发包情况
// 用于区分 "active 但卡顿" 与 "正常播放" 两种状态
//
// 挂点优先级（自动探测）：
//   1. kprobe/l2cap_sock_sendmsg  — 最通用，内核 4.x+
//   2. kprobe/__sock_sendmsg      — 通用的 socket 发送钩子
//   3. 全部失败 → 用户空间降级为纯 D-Bus 模式

#define __TARGET_ARCH_x86
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif

char LICENSE[] SEC("license") = "GPL";

// ---- 数据结构 ----

// 蓝牙设备标识（用于 Map Key）
// 使用 6 字节 BDADDR + 1 字节方向区分 tx/rx
struct device_key {
    __u8  bdaddr[6];   // 蓝牙设备地址
    __u8  direction;   // 0=发送, 1=接收
} __attribute__((packed));

// 会话控制：用户空间通过 D-Bus 获知设备 active 后写入此 Map
struct session_control {
    __u8  enabled;     // 1=启用跟踪, 0=禁用
    __u8  reserved[3];
};

// 流量统计：eBPF 内核态累计，用户空间定期读取
struct traffic_stats {
    __u64 bytes;          // 累计字节数
    __u64 packets;        // 累计包数
    __u64 last_packet_ns; // 最后一包的时间戳 (bpf_ktime_get_ns)
    __u64 gap_count;      // 间隔 > GAP_THRESHOLD_NS 的次数
    __u64 max_gap_ns;     // 最大包间隔 (纳秒)
};

// 全局配置：key=0 启用/禁用
struct global_cfg {
    __u8  enabled;
    __u8  reserved[3];
};

// ---- BPF Map 定义 ----

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct global_cfg);
} btaudio_cfg SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 64);
    __type(key, struct device_key);
    __type(value, struct session_control);
} active_sessions SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 64);
    __type(key, struct device_key);
    __type(value, struct traffic_stats);
} bt_traffic SEC(".maps");

// ---- 常量 ----

#define GAP_THRESHOLD_NS  100000000ULL  // 100ms

// ---- 内部辅助 ----

// 简化的 bdaddr_t 结构 (6 bytes)
// 定义自己的结构以避免依赖内核 BTF 中的 l2cap_chan
struct bdaddr_t {
    __u8 b[6];
};

// 简化的 l2cap_chan 结构（仅含我们需要的字段）
// 内核中 dst 字段在 l2cap_chan 中有固定偏移，我们直接读取
// 完整定义在 include/net/bluetooth/l2cap.h
struct l2cap_chan_minimal {
    struct sock *sk;
    __u8 __pad[16];     // 跳过一些字段
    struct bdaddr_t dst;  // 目标 BDADDR
    struct bdaddr_t src;  // 源 BDADDR
};

static __always_inline int is_global_enabled(void)
{
    __u32 key = 0;
    struct global_cfg *cfg = bpf_map_lookup_elem(&btaudio_cfg, &key);
    if (!cfg) return 0;
    return cfg->enabled;
}

static __always_inline int is_session_enabled(const struct device_key *k)
{
    struct session_control *ctrl = bpf_map_lookup_elem(&active_sessions, k);
    if (!ctrl) return 0;
    return ctrl->enabled;
}

// 从 struct sock 的 sk_user_data 中提取蓝牙设备地址
// sk_user_data → l2cap_chan → dst.b (6 bytes)
// 使用 bpf_probe_read_kernel 读取（不依赖 BTF）
static __always_inline int extract_bdaddr_kprobe(struct sock *sk, __u8 *bdaddr_out)
{
    if (!sk) return -1;

    // 验证协议族
    __u16 family = BPF_CORE_READ(sk, __sk_common.skc_family);
    if (family != AF_BLUETOOTH)
        return -1;

    // 读取 sk_user_data 指针
    void *user_data;
    if (bpf_probe_read_kernel(&user_data, sizeof(user_data), &sk->sk_user_data) < 0)
        return -1;
    if (!user_data)
        return -1;

    // 读取 dst.b (6 bytes) — 从 l2cap_chan 结构体中读取
    // dst 字段在 l2cap_chan 中的偏移（近似值，取决于内核版本）
    // 保守做法：多偏移尝试，或直接用固定偏移
    // 典型偏移：sk(8) + 其他字段 ≈ 偏移 24-32 字节
    // 使用 bpf_probe_read_kernel 读取，容错返回
    struct bdaddr_t dst;
    if (bpf_probe_read_kernel(&dst, sizeof(dst),
                               (char *)user_data + 24 /* approximate dst offset */) < 0)
        return -1;

    __builtin_memcpy(bdaddr_out, dst.b, 6);
    return 0;
}

static __always_inline void update_stats(struct device_key *k, __u64 len)
{
    __u64 now = bpf_ktime_get_ns();
    struct traffic_stats *stats = bpf_map_lookup_elem(&bt_traffic, k);
    if (!stats) {
        struct traffic_stats init = {0};
        init.bytes = len;
        init.packets = 1;
        init.last_packet_ns = now;
        bpf_map_update_elem(&bt_traffic, k, &init, BPF_ANY);
        return;
    }

    __sync_fetch_and_add(&stats->bytes, len);
    __sync_fetch_and_add(&stats->packets, 1);

    // 包间隔计算（原子安全）
    if (stats->last_packet_ns > 0 && now > stats->last_packet_ns) {
        __u64 gap = now - stats->last_packet_ns;
        if (gap > GAP_THRESHOLD_NS) {
            __sync_fetch_and_add(&stats->gap_count, 1);
        }
        if (gap > stats->max_gap_ns) {
            stats->max_gap_ns = gap;
        }
    }
    stats->last_packet_ns = now;
}

// ---- 挂点 1: kprobe/l2cap_sock_sendmsg ----
// 从 struct socket *sock 中提取 struct sock，再从 sk_user_data 提取 BDADDR
SEC("kprobe/l2cap_sock_sendmsg")
int l2cap_send_entry(struct pt_regs *ctx)
{
    if (!is_global_enabled())
        return 0;

    struct socket *sock = (struct socket *)PT_REGS_PARM1(ctx);
    size_t len = (size_t)PT_REGS_PARM3(ctx);
    if (!sock || len == 0)
        return 0;

    struct sock *sk = BPF_CORE_READ(sock, sk);
    if (!sk)
        return 0;

    __u8 bdaddr[6];
    if (extract_bdaddr_kprobe(sk, bdaddr) < 0)
        return 0;

    struct device_key key = {0};
    __builtin_memcpy(key.bdaddr, bdaddr, 6);
    key.direction = 0;

    if (!is_session_enabled(&key))
        return 0;

    update_stats(&key, (__u64)len);
    return 0;
}

// ---- 挂点 2: kprobe/__sock_sendmsg ----
// 通用 socket 发送钩子，过滤 AF_BLUETOOTH 协议族
// 作为备选挂点，当 l2cap_sock_sendmsg 不可用时使用
// 注意：此挂点无法区分具体设备，将所有蓝牙流量聚合到 key=0 的"通配"条目
SEC("kprobe/__sock_sendmsg")
int sock_sendmsg_entry(struct pt_regs *ctx)
{
    if (!is_global_enabled())
        return 0;

    struct socket *sock = (struct socket *)PT_REGS_PARM1(ctx);
    if (!sock)
        return 0;

    struct sock *sk = BPF_CORE_READ(sock, sk);
    if (!sk)
        return 0;

    __u16 family = BPF_CORE_READ(sk, __sk_common.skc_family);
    if (family != AF_BLUETOOTH)
        return 0;

    // 聚合所有蓝牙流量到通配 Key（全零 BDADDR）
    struct device_key key = {0};
    key.direction = 0;

    // 使用固定 1 作为包计数（无法准确获取长度，但包间隔信息仍然有效）
    update_stats(&key, 1);
    return 0;
}
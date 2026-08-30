/*
 * 文件: flow_rate.bpf.c
 * 功能: 网络流量速率统计。通过挂载内核 IP/UDP 发送路径，统计每条 TCP/UDP 流
 *       的字节数和包数，并聚合到进程级别，可实时计算各进程的网络带宽消耗。
 *       同时复用 kprobe/tcp_retransmit_skb 挂点统计进程重传次数。
 *
 * 挂载的探针类型和内核函数:
 *   - kprobe/ip_queue_xmit      : IP 层 TCP 发包入口，从 sk_buff 取 len 统计流量
 *   - kprobe/udp_sendmsg       : UDP 发包入口，参数直接带 size
 *   - kprobe/tcp_retransmit_skb: TCP 重传入口，用于进程级重传计数
 *
 * 使用的 BPF Map:
 *   - current_sec  : LRU_HASH，按 5 元组 (saddr,daddr,sport,dport,protocol) 统计每条流的字节/包数
 *   - process_stats: LRU_HASH，按 PID 聚合进程级网络画像（comm、tx_bytes、tx_packets、retrans_count）
 *   - cfg_iface    : HASH，key=0，用户态可预配置网卡 ifindex 过滤（0 表示不过滤）
 *
 * 用户态对应的 Monitor 类: FlowRateMonitor / NetworkQualityAssessor
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

char LICENSE[] SEC("license") = "GPL";

// =============================================================================
// 数据结构定义
// =============================================================================

/*
 * 流标识 Key（5 元组）
 * 用 __attribute__((packed)) 消除编译器可能插入的 padding，
 * 保证作为 Map Key 时的唯一性和可哈希性
 */
struct conn_key {
    __u32 saddr;      // 源 IP（网络序，来自 skc_rcv_saddr）
    __u32 daddr;      // 目的 IP（网络序，来自 skc_daddr）
    __u16 sport;      // 源端口（网络序，bpf_htons(skc_num)）
    __u16 dport;      // 目的端口（网络序，skc_dport 直接存网络序）
    __u8  protocol;   // IP 协议号：6=TCP, 17=UDP
} __attribute__((packed));

/*
 * 单条流的数据统计（存储在 current_sec Map 的 Value）
 */
struct flow_data {
    __u64 bytes;      // 累计发送字节数
    __u64 packets;    // 累计发送包数
    __u32 pid;        // 最近一次发送此流数据的进程 PID
};

/*
 * 进程级网络画像（存储在 process_stats Map 中）
 * 用户态可通过此 Map 识别哪些进程正在消耗带宽
 */
struct process_net_stats {
    char  comm[16];        // 进程名（Linux 内核限制 16 字节，含结尾 \0）
    __u64 tx_bytes;        // 该进程所有 TCP+UDP 流的累计发送字节数
    __u64 tx_packets;      // 累计发送包数
    __u64 retrans_count;   // TCP 重传次数（由 trace_tcp_retransmit 挂点累计）
};

// =============================================================================
// BPF Map 定义
// =============================================================================

/*
 * Map: current_sec
 * 类型: BPF_MAP_TYPE_LRU_HASH（LRU 淘汰，65536 条目）
 * Key:  struct conn_key（5 元组）
 * Value: struct flow_data（字节数/包数/PID）
 * 最大条目数: 65536（高并发连接场景下足够容纳活跃流，LRU 自动淘汰冷门流）
 * 用途: 存储当前正在发送的每条流的累计统计。
 *       LRU_HASH 的好处：1) 自动内存管理，不会因为某条流发完就忘记删
 *                       2) 比 PERCPU_ARRAY 灵活，支持动态 5 元组
 * 命名 "current_sec" 暗示：用户态可以每秒轮询后清空（或直接替换为新窗口）
 */
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 65536);
    __type(key, struct conn_key);
    __type(value, struct flow_data);
} current_sec SEC(".maps");

/*
 * Map: process_stats
 * 类型: BPF_MAP_TYPE_LRU_HASH
 * Key:  __u32 = PID（来自 bpf_get_current_pid_tgid() 低 32 位清零后的值）
 * Value: struct process_net_stats（进程级画像）
 * 最大条目数: 65536
 * 用途: 按进程聚合所有网络流量。account_flow() 每次调用时同时更新
 *       current_sec（流级）和 process_stats（进程级）两张 Map。
 *       进程第一次出现时用 bpf_get_current_comm() 抓取 16 字节进程名。
 */
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 65536);
    __type(key, __u32);
    __type(value, struct process_net_stats);
} process_stats SEC(".maps");

/*
 * Map: cfg_iface
 * 类型: BPF_MAP_TYPE_HASH（单条目配置 Map）
 * Key:  __u32，固定值 0
 * Value: __u32，网卡 ifindex（0 表示不限制，所有接口都统计）
 * 最大条目数: 1
 * 用途: 用户态可预加载，实现"只统计指定网卡"的能力。
 *       例如只统计 Wi-Fi 接口（wlan0 的 ifindex 通常较小），
 *       排除 loopback (lo) 和 docker (docker0) 等虚拟接口。
 */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1);
    __type(key, __u32);     // 固定 key=0
    __type(value, __u32);   // ifindex
} cfg_iface SEC(".maps");

// =============================================================================
// 辅助函数
// =============================================================================

/*
 * 判断 sk_buff 所属网卡是否在用户态配置的白名单内
 * @skb_ifindex sk_buff->dev->ifindex，由调用方提取好传入
 * 返回: true=允许统计，false=跳过
 * 逻辑：cfg_iface 里没配置 → 全部放行；cfg_iface=0 → 全部放行；
 *       cfg_iface=N(非 0) → 只允许 ifindex==N 的包
 */
static __always_inline bool iface_allowed(__u32 skb_ifindex)
{
    __u32 key = 0;
    __u32 *want = bpf_map_lookup_elem(&cfg_iface, &key);
    if (!want) return true;  // 未配置则放行
    if (*want == 0) return true;
    return skb_ifindex == *want;
}

/*
 * 从 struct sock 构造 5 元组 conn_key
 * 仅处理 AF_INET（IPv4），IPv6 可扩展 AF_INET6 分支
 * @proto IP 协议号，由调用方传入（6=TCP，17=UDP）
 * 返回: 0=成功，-1=非 IPv4 家族
 */
static __always_inline int fill_key_from_sock(struct sock *sk, __u8 proto, struct conn_key *k)
{
    __u16 family = BPF_CORE_READ(sk, __sk_common.skc_family);
    if (family != AF_INET)
        return -1;

    // skc_rcv_saddr / skc_daddr：内核中存储的 IP 地址（网络字节序）
    k->saddr = BPF_CORE_READ(sk, __sk_common.skc_rcv_saddr);
    k->daddr = BPF_CORE_READ(sk, __sk_common.skc_daddr);
    // skc_num 是主机序（小端），端口号在网络传输时是网络序，故转一下
    __u16 sport_host = BPF_CORE_READ(sk, __sk_common.skc_num);
    k->sport = bpf_htons(sport_host);
    // skc_dport 内核已经存网络序，直接使用
    k->dport = BPF_CORE_READ(sk, __sk_common.skc_dport);
    k->protocol = proto;
    return 0;
}

/*
 * 核心统计函数：同时更新"流级 Map"和"进程级 Map"
 * @k         5 元组 Key（已填好）
 * @add_bytes 本次发送的字节数（来自 skb->len 或 msghdr->size）
 *
 * 流程：
 *   1. 在 current_sec (LRU_HASH) 中查找或创建 flow_data
 *   2. 原子累加 bytes 和 packets（同一 CPU 上 kprobe 可以并发，用 __sync_fetch_and_add）
 *   3. 更新 process_stats (LRU_HASH)：用 bpf_get_current_pid_tgid() 取 PID 作为 Key
 *   4. 进程首次出现时调用 bpf_get_current_comm() 抓取 16 字节进程名
 */
static __always_inline void account_flow(struct conn_key *k, __u64 add_bytes)
{
    // === 流级统计 ===
    struct flow_data *v = bpf_map_lookup_elem(&current_sec, k);
    if (!v) {
        // 首次出现此 5 元组：创建初始化记录
        struct flow_data init = {0};
        init.bytes = add_bytes;
        init.packets = 1;
        // bpf_get_current_pid_tgid() 高 32 位=PID，低 32 位=TID
        // 低 32 位清零的方式：& 0xffffffff 后取高 32 位移位其实等价
        // 这里直接 & 0xffffffff 是取整个 64 位的低 32 位，作为 PID
        init.pid = (__u32)(bpf_get_current_pid_tgid() & 0xffffffff);
        bpf_map_update_elem(&current_sec, k, &init, 0);  // BPF_NOEXIST：只创建不覆盖
    } else {
        // 已存在：原子累加
        __sync_fetch_and_add(&v->bytes, add_bytes);
        __sync_fetch_and_add(&v->packets, 1);
        v->pid = (__u32)(bpf_get_current_pid_tgid() & 0xffffffff);
    }

    // === 进程级统计 ===
    __u32 pid = (__u32)(bpf_get_current_pid_tgid() & 0xffffffff);
    struct process_net_stats *ps = bpf_map_lookup_elem(&process_stats, &pid);
    if (!ps) {
        struct process_net_stats init = {};
        // bpf_get_current_comm：BPF 辅助函数 #159，从 task_struct->comm 复制进程名
        // 大小固定 16 字节，与内核 TASK_COMM_LEN 对齐
        bpf_get_current_comm(init.comm, sizeof(init.comm));
        init.tx_bytes = add_bytes;
        init.tx_packets = 1;
        bpf_map_update_elem(&process_stats, &pid, &init, 0);
    } else {
        __sync_fetch_and_add(&ps->tx_bytes, add_bytes);
        __sync_fetch_and_add(&ps->tx_packets, 1);
    }
}

// =============================================================================
// BPF 入口函数 1: TCP 重传计数（进程级）
// =============================================================================

/*
 * 函数: trace_tcp_retransmit（重传专用）
 * 挂点: SEC("kprobe/tcp_retransmit_skb")
 * 触发时机: 每当 TCP 检测到丢包并重新发送 sk_buff 时。
 *           内核函数签名: int tcp_retransmit_skb(struct sock *sk, struct sk_buff *skb, int segs)
 * 主要逻辑: 以当前 PID 为 Key，在 process_stats 中累加 retrans_count。
 *           与 tcp_retransmit.bpf.c 的区别：那个按"连接"维度统计，
 *           这里按"进程"维度统计（一个进程可能有多个 TCP 连接）。
 * 写入的 Map: process_stats（累加 retrans_count）
 */
SEC("kprobe/tcp_retransmit_skb")
int trace_tcp_retransmit(struct pt_regs *ctx)
{
    // 取 PID（& 0xffffffff 保留低 32 位）
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

// =============================================================================
// BPF 入口函数 2: IP 层 TCP 发包入口
// =============================================================================

/*
 * 函数: tcp_transmit_entry
 * 挂点: SEC("kprobe/ip_queue_xmit")
 * 触发时机: 每当 IP 层准备发送一个 sk_buff 时。
 *           内核函数签名: int ip_queue_xmit(struct sock *sk, struct sk_buff *skb, ...)
 *           此函数是 TCP 发包的必经之路（UDP 走 ip_send_skb，但我们用 udp_sendmsg 挂点更精确）。
 * 主要逻辑:
 *   1. 从 PT_REGS_PARM1/2 取出 struct sock 和 struct sk_buff
 *   2. 从 skb->dev->ifindex 提取网卡号，调用 iface_allowed() 判断是否在白名单内
 *   3. fill_key_from_sock(sk, 6, &k) 构造 TCP 5 元组
 *   4. BPF_CORE_READ(skb, len) 获取 IP 包长度（不含链路层头）
 *   5. account_flow() 同时更新流级和进程级 Map
 * 写入的 Map: current_sec + process_stats
 *
 * sk_buff 结构说明:
 *   skb->dev    : 指向发送/接收网卡的 struct net_device*
 *   skb->dev->ifindex : 网卡在内核中的唯一编号（lo=1, eth0=2, wlan0=3...）
 *   skb->len    : skb 中线性数据区的长度（不含非线性 fragment 的 data_len）
 */
SEC("kprobe/ip_queue_xmit")
int tcp_transmit_entry(struct pt_regs *ctx)
{
    struct sock *sk = (struct sock *)PT_REGS_PARM1(ctx);
    struct sk_buff *skb = (struct sk_buff *)PT_REGS_PARM2(ctx);
    if (!sk || !skb) return 0;

    // === 网卡过滤 ===
    // BPF_CORE_READ(skb, dev) 读 sk_buff 的 dev 成员（struct net_device *）
    struct net_device *dev = BPF_CORE_READ(skb, dev);
    __u32 ifindex = dev ? BPF_CORE_READ(dev, ifindex) : 0;
    if (!iface_allowed(ifindex)) return 0;  // 不在白名单内则跳过

    // 构造 TCP 5 元组（6=IPPROTO_TCP）
    struct conn_key k = {};
    if (fill_key_from_sock(sk, 6 /*TCP*/, &k) < 0)
        return 0;

    // BPF_CORE_READ(skb, len)：sk_buff 的 len 成员，
    // 是 IP 层视角的包长度（从 IP 头开始算），不含以太网头
    __u32 len = BPF_CORE_READ(skb, len);
    account_flow(&k, len);
    return 0;
}

// =============================================================================
// BPF 入口函数 3: UDP 发包入口
// =============================================================================

/*
 * 函数: udp_send_entry
 * 挂点: SEC("kprobe/udp_sendmsg")
 * 触发时机: 每当 UDP socket 发送数据时。
 *           内核函数签名: int udp_sendmsg(struct sock *sk, struct msghdr *msg, size_t len)
 * 主要逻辑:
 *   1. 从 PT_REGS_PARM3 直接拿到 len（用户态本次 send 的字节数）
 *   2. fill_key_from_sock(sk, 17, &k) 构造 UDP 5 元组（17=IPPROTO_UDP）
 *   3. account_flow() 更新两张 Map
 * 写入的 Map: current_sec + process_stats
 *
 * 为什么 UDP 不做 ifindex 过滤？
 *   UDP 在 udp_sendmsg 时 skb 还没构造，无法提前知道会走哪块网卡。
 *   接口过滤对 UDP 兼容性差（旧内核字段名不同），此处简化为不做过滤。
 */
SEC("kprobe/udp_sendmsg")
int udp_send_entry(struct pt_regs *ctx)
{
    struct sock *sk = (struct sock *)PT_REGS_PARM1(ctx);
    // PT_REGS_PARM3 对应 msghdr 参数后面的 size_t len
    unsigned long len = (unsigned long)PT_REGS_PARM3(ctx);
    if (!sk) return 0;

    // UDP 无 skb，不做接口过滤，仍统计总量
    struct conn_key k = {};
    if (fill_key_from_sock(sk, 17 /*UDP*/, &k) < 0)
        return 0;

    account_flow(&k, (__u64)len);
    return 0;
}

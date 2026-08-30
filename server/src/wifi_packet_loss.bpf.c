/*
 * 文件: wifi_packet_loss.bpf.c
 * 功能: Wi-Fi / 网卡收发丢包归因追踪。通过挂载内核 tracepoint，统计每个网卡接口
 *       的发送/接收/丢弃/重试包数，区分"发送丢包"（TX drops）与"接收丢包"（RX 不计数），
 *       用于定位网络故障点：是 Wi-Fi 信号差导致的 TX retry/drop，
 *       还是中间路由器丢弃（我们只看到 RX 收不到）。
 *
 * 挂载的探针类型和内核函数（全部是 tracepoint）:
 *   - tracepoint/net/netif_receive_skb : 接收路径，每收到一个 sk_buff 就触发
 *   - tracepoint/net/net_dev_queue     : 发送入队，sk_buff 准备交给驱动时触发
 *   - tracepoint/net/net_dev_xmit      : 发送完成，驱动返回成功/BUSY/错误时触发
 *
 * Tracepoint 与 Kprobe 的区别:
 *   Kprobe: 动态插桩，内核符号存在才能挂，参数需手动 PT_REGS_PARM* 提取
 *   Tracepoint: 内核预定义的静态探针，函数签名固定，参数从 ctx 结构按偏移读取
 *              libbpf 会自动为 tracepoint 生成 trace_event_raw_* 结构体，
 *              事件结构前 8 字节是通用头（ent，含 type/flags/timestamp），
 *              实际数据字段从偏移 +8 开始。
 *
 * 使用的 BPF Map:
 *   - packet_stats : HASH，key=ifindex（网卡编号），Value=收发统计结构体
 *
 * 用户态对应的 Monitor 类: WifiPacketLossMonitor / NetInterfaceMonitor
 */

#define __TARGET_ARCH_arm64
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_endian.h>

char LICENSE[] SEC("license") = "GPL";

// =============================================================================
// 数据结构定义
// =============================================================================

/*
 * 每个网卡接口的收发统计
 * 存储在 packet_stats Map 中，key=网卡 ifindex
 * 用户态定期读取后可以计算：
 *   发送成功率 = tx_pkts / max(tx_pkts + tx_drops, 1)
 *   发送重试率 = tx_retries / max(tx_pkts, 1)
 *   接收带宽   = rx_bytes / 时间窗口
 */
struct iface_packet_stats {
    __u64 rx_pkts;      // 接收包数（netif_receive_skb 触发）
    __u64 rx_bytes;     // 接收字节数（sk_buff->len 累加）
    __u64 tx_pkts;      // 发送包数（net_dev_queue 入队时统计）
    __u64 tx_bytes;     // 发送字节数（sk_buff->len 累加）
    __u64 tx_drops;     // 发送丢弃数（net_dev_xmit 返回 rc<0 时）
    __u64 tx_retries;   // 发送重试数（net_dev_xmit 返回 rc=NETDEV_TX_BUSY(16) 时）
};

// =============================================================================
// BPF Map 定义
// =============================================================================

/*
 * Map: packet_stats
 * 类型: BPF_MAP_TYPE_HASH（普通哈希表）
 * Key:  __u32 = 网卡 ifindex（内核给每块网卡分配的唯一编号：lo=1, eth0=2, wlan0=3...）
 * Value: struct iface_packet_stats（收发统计）
 * 最大条目数: 256（足够覆盖一台机器上所有可能的网卡 + 虚拟接口）
 * 用途: 存储每块网卡的实时收发统计。tracepoint 触发时用 ifindex 查找/创建条目，
 *       原子累加各计数器。用户态定期遍历整个 Map 即可获得全机网络画像。
 *
 * 为什么不用 PERCPU_HASH？
 *   HASH 类型配合 __sync_fetch_and_add 原子操作已经足够，且单条目读取比 PERCPU
 *   多条目合并更简单（用户态不需要遍历每个 CPU 的槽位再求和）。
 *   网卡 tracepoint 触发频率虽然高，但单次只做 atomic add，锁开销可接受。
 */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 256);
    __type(key, __u32);
    __type(value, struct iface_packet_stats);
} packet_stats SEC(".maps");

// =============================================================================
// 辅助函数
// =============================================================================

/*
 * 获取某 ifindex 对应的统计条目，如果不存在则创建（zero-initialized）
 * 典型的"查找或创建"模式：BPF_MAP_TYPE_HASH 没有 BPF_NOEXIST 创建的原子性保证，
 * 所以先 lookup，不存在则 update(BPF_ANY) 再 lookup 一次。
 * 返回: 结构体指针（BPF Map Value 地址，直接写入即生效），失败返回 NULL
 */
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

/*
 * 从 sk_buff 提取网卡 ifindex
 * sk_buff 结构：skb->dev 是发送/接收网卡的 struct net_device*
 * net_device->ifindex 是内核分配的唯一网卡编号
 *
 * BPF_CORE_READ 访问 sk_buff.dev：
 *   sk_buff 是内核中最常见的结构体之一，BPF_CORE_READ 会根据 vmlinux BTF
 *   自动解析 dev 指针的偏移，保证跨内核版本兼容。
 */
static __always_inline __u32 skb_ifindex(struct sk_buff *skb)
{
    if (!skb) return 0;
    // BPF_CORE_READ(skb, dev)：读 skb 结构体的 dev 成员（struct net_device*）
    struct net_device *dev = BPF_CORE_READ(skb, dev);
    if (!dev) return 0;
    // BPF_CORE_READ(dev, ifindex)：读 net_device 的 ifindex 成员
    return BPF_CORE_READ(dev, ifindex);
}

// =============================================================================
// BPF 入口函数 1: 接收路径
// =============================================================================

/*
 * 函数: trace_net_rx
 * 挂点: SEC("tracepoint/net/netif_receive_skb")
 * 触发时机: 每当内核准备把一个 sk_buff 交给上层协议栈处理时（网卡驱动已收到包）。
 *           内核中 netif_receive_skb() 是所有入方向流量的必经之路。
 * 主要逻辑:
 *   1. 从 tracepoint 事件结构 ctx 按偏移 +8 读出 skbaddr（指向 sk_buff 的指针）
 *   2. bpf_probe_read_kernel(&skb, sizeof(skb), (void*)ctx + 8)：
 *      tracepoint 的原始事件结构以通用 8 字节 ent 头开头，实际字段从 +8 开始
 *   3. skb_ifindex(skb) 提取网卡号
 *   4. get_stats(ifindex) 获取/创建统计条目
 *   5. 原子累加 rx_pkts 和 rx_bytes
 * 写入的 Map: packet_stats（rx_pkts++, rx_bytes += skb->len）
 *
 * 为什么用 (void*)ctx + 8 而不是 BPF_CORE_READ(ctx, skbaddr)？
 *   tracepoint 的 ctx 结构（trace_event_raw_*）字段名是 libbpf 自动生成的，
 *   不同内核版本可能有差异（字段名/顺序），直接硬编码偏移 +8 更稳定。
 *   内核约定 trace_event_raw 结构前 8 字节永远是通用 ent 头。
 */
SEC("tracepoint/net/netif_receive_skb")
int trace_net_rx(struct trace_event_raw_netif_receive_skb *ctx)
{
    struct sk_buff *skb = NULL;
    // bpf_probe_read_kernel：从内核空间 ctx + 8 处读出 skbaddr（8 字节指针）
    // 注意这里不能直接写 skb = ctx->skbaddr，因为 trace_event_raw 结构
    // 的字段在编译时可能还没被 CO-RE reloc 正确处理，用 bpf_probe_read_kernel
    // 按固定偏移 +8 读取最稳妥
    bpf_probe_read_kernel(&skb, sizeof(skb), (void*)ctx + 8);
    if (!skb) return 0;

    // 从 sk_buff.dev.ifindex 拿到网卡编号
    __u32 ifindex = skb_ifindex(skb);
    if (ifindex == 0) return 0;

    struct iface_packet_stats *stats = get_stats(ifindex);
    if (!stats) return 0;

    // __sync_fetch_and_add：多核原子自增，避免不同 CPU 同时写同一条目时丢数据
    __sync_fetch_and_add(&stats->rx_pkts, 1);
    // BPF_CORE_READ(skb, len)：sk_buff 的 len 成员，线性数据区长度（不含 fragment）
    __u32 len = BPF_CORE_READ(skb, len);
    __sync_fetch_and_add(&stats->rx_bytes, len);
    return 0;
}

// =============================================================================
// BPF 入口函数 2: 发送入队
// =============================================================================

/*
 * 函数: trace_net_tx_queue
 * 挂点: SEC("tracepoint/net/net_dev_queue")
 * 触发时机: 每当 sk_buff 准备进入网卡驱动的发送队列时（还没真正发给硬件）。
 * 主要逻辑: 与 trace_net_rx 对称，统计 tx_pkts 和 tx_bytes
 * 写入的 Map: packet_stats（tx_pkts++, tx_bytes += skb->len）
 *
 * 与 trace_net_dev_xmit 的配合：
 *   net_dev_queue 统计"尝试发送"的包数和字节数（TX 总量）
 *   net_dev_xmit 统计"发送结果"（成功/BUSY/错误）
 *   tx_retries（BUSY） + tx_drops（错误）= 发送侧的异常包数
 */
SEC("tracepoint/net/net_dev_queue")
int trace_net_tx_queue(struct trace_event_raw_net_dev_queue *ctx)
{
    struct sk_buff *skb = NULL;
    // 同样按偏移 +8 读取 skbaddr
    bpf_probe_read_kernel(&skb, sizeof(skb), (void*)ctx + 8);
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

// =============================================================================
// BPF 入口函数 3: 发送完成（区分成功/重试/丢弃）
// =============================================================================

/*
 * 函数: trace_net_tx_xmit
 * 挂点: SEC("tracepoint/net/net_dev_xmit")
 * 触发时机: 每当网卡驱动尝试发送一个 sk_buff 并返回结果时。
 *           tracepoint 事件结构:
 *             +0  : ent (8B)    — 通用头
 *             +8  : skbaddr (8B) — sk_buff*
 *             +16 : len (4B)    — skb_len（注意这个 len 是 tracepoint 里额外存的副本）
 *             +20 : rc (4B)     — 驱动返回码
 * 主要逻辑:
 *   1. 从偏移 +8 读 skbaddr（8B 指针）
 *   2. 从偏移 +20 读 rc（4B 返回码）
 *   3. rc==NETDEV_TX_BUSY(16) → 驱动忙，计入 tx_retries（内核稍后会重试）
 *      rc<0 → 驱动报错，计入 tx_drops（包被丢弃）
 *      rc==0（NETDEV_TX_OK）→ 成功，不统计异常项
 * 写入的 Map: packet_stats（tx_retries++ 或 tx_drops++）
 *
 * 丢包归因:
 *   tx_drops 高 → Wi-Fi 信号差或驱动问题（我们主动重传/丢弃）
 *   rx_pkts 突然降为 0 但 tx_pkts 正常 → 中间链路丢包（路由器丢弃，我们看不到）
 */
SEC("tracepoint/net/net_dev_xmit")
int trace_net_tx_xmit(struct trace_event_raw_net_dev_xmit *ctx)
{
    // trace_event_raw_net_dev_xmit 的布局（从偏移 +0 开始）:
    //   +0  ent (8B)    ← tracepoint 通用头
    //   +8  skbaddr (8B) ← sk_buff*
    //   +16 len (4B)    ← skb_len（tracepoint 额外保存的长度）
    //   +20 rc (4B)     ← 驱动返回码（我们需要的就是这个）
    struct sk_buff *skb = NULL;
    int rc = 0;
    // 按偏移 +8 读 skbaddr
    bpf_probe_read_kernel(&skb, sizeof(skb), (void*)ctx + 8);
    // 按偏移 +20 读 rc（跳过 ent(8) + skbaddr(8) + len(4) = 20）
    bpf_probe_read_kernel(&rc, sizeof(rc), (void*)ctx + 20);

    __u32 ifindex = 0;
    if (skb) ifindex = skb_ifindex(skb);
    if (ifindex == 0) return 0;

    struct iface_packet_stats *stats = get_stats(ifindex);
    if (!stats) return 0;

    // 驱动返回码常量（定义在 include/linux/netdevice.h）:
    //   NETDEV_TX_OK = 0      → 发送成功
    //   NETDEV_TX_BUSY = 16   → 驱动忙，内核稍后自动重试
    //   负数                  → 驱动硬错误（ENETDOWN, EIO 等），包被丢弃
    if (rc == 16) {  // NETDEV_TX_BUSY
        // 设备忙，内核会重传，计入"重试"
        __sync_fetch_and_add(&stats->tx_retries, 1);
    } else if (rc < 0) {
        // 驱动返回错误码（负数 errno），包被丢弃
        __sync_fetch_and_add(&stats->tx_drops, 1);
    }
    // rc == 0 (NETDEV_TX_OK) → 发送成功，不需要额外统计
    return 0;
}

/*
 * 文件: bpf/a2dp_media.bpf.c
 * 功能: 蓝牙 A2DP/L2CAP 音频流量采集。通过挂载内核 L2CAP 发送路径，
 *       统计蓝牙音频设备的实际发包频率和包间隔，用于区分：
 *         - "active 但卡顿"：蓝牙设备已连接 (D-Bus 报告 active)，
 *           但 eBPF 检测到包间隔 > GAP_THRESHOLD_NS（100ms），说明链路中断
 *         - "正常播放"：活跃连接且包间隔均匀（通常 20ms 左右一个音频包）
 *
 * 挂载的探针类型和内核函数（两级自动降级）:
 *   1. kprobe/l2cap_sock_sendmsg  — 最精确，能提取 BDADDR 区分具体蓝牙设备
 *   2. kprobe/__sock_sendmsg     — 通用 socket 发送钩子，只能聚合所有蓝牙流量
 *   3. 全部失败 → 用户空间降级为纯 D-Bus 模式（仅知道连接状态，不知道实际流量）
 *
 * 使用的 BPF Map:
 *   - btaudio_cfg      : ARRAY (1 entry)，全局启用/禁用开关
 *   - active_sessions  : HASH，key=BDADDR+方向，用户态通过 D-Bus 发现新设备后写入启用
 *   - bt_traffic       : LRU_HASH，key=BDADDR+方向，累计统计字节数/包数/包间隔
 *
 * 用户态对应的 Monitor 类: BluetoothAudioMonitor / A2dpMediaMonitor
 */

#define __TARGET_ARCH_arm64
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>

// AF_BLUETOOTH 协议族常量（内核定义在 include/linux/socket.h）
// 值为 31，这里显式定义以避免依赖内核头文件
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif

char LICENSE[] SEC("license") = "GPL";

// =============================================================================
// 数据结构定义
// =============================================================================

/*
 * 蓝牙设备标识（作为 Map Key）
 *   bdaddr[6]   — 48 位蓝牙设备地址（格式 XX:XX:XX:XX:XX:XX）
 *   direction   — 0=发送（本机→耳机），1=接收（耳机→本机）
 * __attribute__((packed)) 消除 padding，保证作为 Map Key 时可哈希、无歧义
 */
struct device_key {
    __u8  bdaddr[6];
    __u8  direction;
} __attribute__((packed));

/*
 * 会话控制记录（存储在 active_sessions Map）
 * 用户态职责：通过 D-Bus 监听 org.bluez，发现新设备 active 后写入此 Map
 * eBPF 职责：每次 L2CAP 发包时检查此 Map，只有 enabled=1 的会话才统计
 * 这样可以避免 eBPF 每次都检查所有蓝牙包（性能优化）
 */
struct session_control {
    __u8  enabled;      // 1=启用此设备的流量追踪，0=禁用
    __u8  reserved[3];  // 对齐填充（不使用）
};

/*
 * 蓝牙流量统计记录（存储在 bt_traffic Map，Value）
 * 用户态定期读取后可计算：
 *   - 平均包速率 = packets / 时间窗口
 *   - 平均间隔   = total_gap_time / packets（需扩展字段）
 *   - 卡顿次数   = gap_count（包间隔 > 100ms 的次数）
 *   - 最大间隙   = max_gap_ns（最大包间隔，用于识别严重卡顿）
 */
struct traffic_stats {
    __u64 bytes;          // 累计发送字节数
    __u64 packets;        // 累计发送包数
    __u64 last_packet_ns; // 上一个包的时间戳（bpf_ktime_get_ns），用于计算 gap
    __u64 gap_count;      // 间隔 > GAP_THRESHOLD_NS 的次数
    __u64 max_gap_ns;     // 观测到的最大包间隔（纳秒）
};

/*
 * 全局配置（存储在 btaudio_cfg Map）
 * 用户态写入 enabled=1 后，所有挂点才开始工作
 * 用于优雅启停，而不是每次重新加载 BPF 程序
 */
struct global_cfg {
    __u8  enabled;
    __u8  reserved[3];
};

// =============================================================================
// BPF Map 定义
// =============================================================================

/*
 * Map: btaudio_cfg
 * 类型: BPF_MAP_TYPE_ARRAY（预分配固定大小数组，所有 CPU 共享）
 * Key:  __u32，固定 0（单条目）
 * Value: struct global_cfg（enabled 开关）
 * 最大条目数: 1
 * 用途: 全局启用/禁用开关。每个 BPF 入口函数最开头都调用 is_global_enabled()
 *       检查此 Map，disabled 时直接 return 0，零开销快速路径。
 *       用 ARRAY 而不是 HASH 的原因：单条目配置，ARRAY lookup 最快且无锁。
 */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct global_cfg);
} btaudio_cfg SEC(".maps");

/*
 * Map: active_sessions
 * 类型: BPF_MAP_TYPE_HASH
 * Key:  struct device_key（BDADDR + 方向）
 * Value: struct session_control（enabled 开关）
 * 最大条目数: 64（足够同时活跃的蓝牙音频设备数量）
 * 用途: 用户态通过 D-Bus 发现新蓝牙音频设备并标记为 active 后，
 *       在此 Map 中为该设备的 (BDADDR, direction) 条目设置 enabled=1。
 *       eBPF 每次 L2CAP 发包时调用 is_session_enabled() 检查。
 *       设备断开连接时，用户态应 delete 此条目（或置 enabled=0）。
 */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 64);
    __type(key, struct device_key);
    __type(value, struct session_control);
} active_sessions SEC(".maps");

/*
 * Map: bt_traffic
 * 类型: BPF_MAP_TYPE_LRU_HASH（带最近最少使用淘汰）
 * Key:  struct device_key（BDADDR + 方向）
 * Value: struct traffic_stats（累计统计）
 * 最大条目数: 64（与 active_sessions 数量匹配）
 * 用途: 存储每个蓝牙设备的实际发包流量统计。每次 L2CAP 发包时更新。
 *       LRU_HASH 保证即使某个设备的 session 被禁用，它的旧统计条目
 *       也会逐渐被淘汰而不撑爆内存。
 */
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 64);
    __type(key, struct device_key);
    __type(value, struct traffic_stats);
} bt_traffic SEC(".maps");

// =============================================================================
// 常量定义
// =============================================================================

/*
 * 包间隔阈值：100ms
 * 蓝牙 A2DP 正常播放时，音频包通常每 ~20ms 发一个（44.1kHz sample rate）。
 * 若两个相邻的 L2CAP 发包间隔 > 100ms，说明蓝牙链路存在卡顿或丢包。
 * 这个值是综合考虑音频缓冲大小和蓝牙重传机制后的经验值。
 */
#define GAP_THRESHOLD_NS  100000000ULL  // 100ms = 100,000,000 纳秒

// =============================================================================
// 自定义简化结构体（绕过内核 BTF 不完整的问题）
// =============================================================================

/*
 * 简化的蓝牙设备地址（BDADDR）结构
 * 内核中有完整定义，但 BTF 可能不完整。
 * 我们只需 6 字节的地址，自己定义就够了。
 */
struct bdaddr_t {
    __u8 b[6];
};

/*
 * 简化的 l2cap_chan 结构（仅含我们需要的字段）
 *
 * 完整的 l2cap_chan 定义在 include/net/bluetooth/l2cap.h，
 * 但内核 BTF 中可能没有完整的类型信息（蓝牙子系统不一定被编进 BTF）。
 * 我们定义一个"最小版"结构，通过硬编码偏移 +24 来读取 dst/src BDADDR。
 *
 * 典型的 l2cap_chan 内存布局（不同内核版本可能有差异）:
 *   offset 0-7   : struct sock *sk
 *   offset 8-23  : 其他内部字段（约 16 字节）
 *   offset 24-29 : bdaddr_t dst（目标蓝牙设备地址，6 字节） ← 我们读这个
 *   offset 30-35 : bdaddr_t src（源蓝牙设备地址，6 字节）
 *
 * 用 __pad[16] 跳过 offset 8-23 的字段，dst 在偏移 24 正好对齐。
 * 如果某内核版本字段偏移不同，extract_bdaddr_kprobe 会因
 * bpf_probe_read_kernel 返回错误而 gracefully 失败（该次统计跳过），
 * 不会导致整个程序崩溃。
 */
struct l2cap_chan_minimal {
    struct sock *sk;
    __u8 __pad[16];     // 跳过一些内部字段
    struct bdaddr_t dst;  // 目标 BDADDR（offset 24）
    struct bdaddr_t src;  // 源 BDADDR（offset 30）
};

// =============================================================================
// 辅助函数
// =============================================================================

/*
 * 检查全局启用开关
 * 从 btaudio_cfg Map 的 key=0 处读取 enabled 标志
 * 返回: 1=启用，0=禁用
 */
static __always_inline int is_global_enabled(void)
{
    __u32 key = 0;
    struct global_cfg *cfg = bpf_map_lookup_elem(&btaudio_cfg, &key);
    if (!cfg) return 0;  // Map 不存在或未初始化，默认禁用
    return cfg->enabled;
}

/*
 * 检查某设备会话是否已启用
 * 在 active_sessions Map 中查找指定 device_key 的 enabled 标志
 * 返回: 1=启用追踪，0=禁用或会话不存在
 */
static __always_inline int is_session_enabled(const struct device_key *k)
{
    struct session_control *ctrl = bpf_map_lookup_elem(&active_sessions, k);
    if (!ctrl) return 0;  // 用户态未配置此设备（可能还没被 D-Bus 发现）
    return ctrl->enabled;
}

/*
 * 从 struct sock 中提取蓝牙目标设备的 BDADDR
 *
 * 蓝牙 socket 的数据路径:
 *   struct socket → sock (sk) → sk->sk_user_data (void*)
 *                                  ↓
 *                          l2cap_chan *（内核蓝牙 L2CAP 通道结构）
 *                                  ↓
 *                          l2cap_chan.dst（bdaddr_t，目标设备 6 字节地址）
 *
 * CO-RE 不适用：
 *   sk_user_data 是 void* 类型指针，内核 BTF 中不会有完整类型信息，
 *   必须用 bpf_probe_read_kernel 硬编码偏移读取。
 *
 * 容错设计：
 *   - 先验证 sk 的 family 是 AF_BLUETOOTH(31)，跳过非蓝牙 socket
 *   - bpf_probe_read_kernel 对非法地址/错误偏移会返回 -EFAULT，不会 OOM
 *   - 任何步骤失败都返回 -1，调用方跳过该次统计
 *
 * 返回: 0=成功，bdaddr_out 已填充；-1=失败
 */
static __always_inline int extract_bdaddr_kprobe(struct sock *sk, __u8 *bdaddr_out)
{
    if (!sk) return -1;

    // 第一步：验证协议族是蓝牙（AF_BLUETOOTH=31）
    __u16 family = BPF_CORE_READ(sk, __sk_common.skc_family);
    if (family != AF_BLUETOOTH)
        return -1;

    // 第二步：从 sock 中取出 sk_user_data（内核 L2CAP 通道指针）
    // sk_user_data 在 sock 中的偏移是固定的（内核结构体 sock 的成员），
    // 用 &sk->sk_user_data 取它的地址，再用 bpf_probe_read_kernel 解引用
    void *user_data;
    if (bpf_probe_read_kernel(&user_data, sizeof(user_data), &sk->sk_user_data) < 0)
        return -1;
    if (!user_data)
        return -1;

    // 第三步：从 l2cap_chan 结构中读取 dst BDADDR
    // 使用硬编码偏移 +24（见 struct l2cap_chan_minimal 的注释）
    // bpf_probe_read_kernel 会验证该地址可读，失败则返回错误
    struct bdaddr_t dst;
    if (bpf_probe_read_kernel(&dst, sizeof(dst),
                               (char *)user_data + 24 /* approximate dst offset */) < 0)
        return -1;

    __builtin_memcpy(bdaddr_out, dst.b, 6);
    return 0;
}

/*
 * 更新蓝牙流量统计（bt_traffic Map）
 * @k   已构造好的 device_key
 * @len 本次发包的字节数（来自 PT_REGS_PARM3）
 *
 * 首次出现某设备时创建初始化记录；已存在则原子累加。
 * 核心逻辑：计算"包间隔"并判断是否超过 GAP_THRESHOLD_NS。
 * gap_count 用于报告卡顿次数，max_gap_ns 用于报告最大卡顿幅度。
 *
 * 原子性注意：
 *   last_packet_ns = now 这步不是原子的，在多核并发下可能有竞争。
 *   但蓝牙 L2CAP 发包通常从同一进程的同一个 CPU 发起，
 *   竞争概率极低，可接受。若要严格正确，可改用 PERCPU Map 或 atomic64。
 */
static __always_inline void update_stats(struct device_key *k, __u64 len)
{
    __u64 now = bpf_ktime_get_ns();  // 单调时钟，适合算间隔
    struct traffic_stats *stats = bpf_map_lookup_elem(&bt_traffic, k);
    if (!stats) {
        // 第一次看到此设备：创建记录
        struct traffic_stats init = {0};
        init.bytes = len;
        init.packets = 1;
        init.last_packet_ns = now;  // 首次直接记录，不算 gap
        bpf_map_update_elem(&bt_traffic, k, &init, BPF_ANY);
        return;
    }

    // 已存在：原子累加 bytes 和 packets
    __sync_fetch_and_add(&stats->bytes, len);
    __sync_fetch_and_add(&stats->packets, 1);

    // 计算包间隔：now - last_packet_ns
    if (stats->last_packet_ns > 0 && now > stats->last_packet_ns) {
        __u64 gap = now - stats->last_packet_ns;
        if (gap > GAP_THRESHOLD_NS) {
            // 间隔超过 100ms，计入卡顿次数
            __sync_fetch_and_add(&stats->gap_count, 1);
        }
        // 更新最大间隔观测值
        if (gap > stats->max_gap_ns) {
            stats->max_gap_ns = gap;
        }
    }
    stats->last_packet_ns = now;  // 更新"上一包时间戳"供下次计算
}

// =============================================================================
// BPF 入口函数 1: 首选 — l2cap_sock_sendmsg
// =============================================================================

/*
 * 函数: l2cap_send_entry
 * 挂点: SEC("kprobe/l2cap_sock_sendmsg")
 * 触发时机: 每当蓝牙 L2CAP socket 发送数据时（A2DP 音频数据走此路径）。
 *           内核函数签名: int l2cap_sock_sendmsg(struct socket *sock, struct msghdr *msg, size_t len)
 *           参数 1 是 struct socket*（不是 struct sock*！），
 *           需要用 BPF_CORE_READ(sock, sk) 拿到内部的 struct sock*。
 * 主要逻辑:
 *   1. is_global_enabled() 快速返回
 *   2. 从 struct socket* 提取 struct sock*（BPF_CORE_READ(sock, sk)）
 *   3. extract_bdaddr_kprobe(sk, bdaddr) 从 sk_user_data→l2cap_chan 读目标 BDADDR
 *   4. 构造 device_key（BDADDR + direction=0 表示发送）
 *   5. is_session_enabled(&key) 检查用户态是否为此设备启用追踪
 *   6. update_stats(&key, len) 更新 bt_traffic Map
 * 写入的 Map: bt_traffic
 *
 * 为什么这是首选挂点？
 *   l2cap_sock_sendmsg 是蓝牙 L2CAP 协议的专用入口，
 *   能精确区分目标设备 BDADDR，实现"每个蓝牙设备独立统计"。
 */
SEC("kprobe/l2cap_sock_sendmsg")
int l2cap_send_entry(struct pt_regs *ctx)
{
    // 全局快速禁用路径：用户态没启动蓝牙追踪时，所有请求秒回
    if (!is_global_enabled())
        return 0;

    // PT_REGS_PARM1: struct socket *sock（不是 struct sock *sk！）
    // PT_REGS_PARM3: size_t len（本次发送的字节数）
    struct socket *sock = (struct socket *)PT_REGS_PARM1(ctx);
    size_t len = (size_t)PT_REGS_PARM3(ctx);
    if (!sock || len == 0)
        return 0;

    // 从 struct socket* 提取内部的 struct sock*
    // BPF_CORE_READ(sock, sk)：socket 结构体有成员 sk，指向底层 sock
    struct sock *sk = BPF_CORE_READ(sock, sk);
    if (!sk)
        return 0;

    // 核心：从 sk → sk_user_data → l2cap_chan → dst BDADDR
    // 如果内核版本不同导致偏移不匹配，这一步会失败并返回 -1，
    // 此挂点跳过，用户态应检测并降级到 __sock_sendmsg 挂点
    __u8 bdaddr[6];
    if (extract_bdaddr_kprobe(sk, bdaddr) < 0)
        return 0;

    // 构造 device_key：方向=0 表示发送（本机→蓝牙设备）
    struct device_key key = {0};
    __builtin_memcpy(key.bdaddr, bdaddr, 6);
    key.direction = 0;

    // 用户态是否为此特定设备启用了追踪？
    // （D-Bus 发现新蓝牙音频设备后会写入此条目）
    if (!is_session_enabled(&key))
        return 0;

    // 更新统计：累加 bytes/packets，计算包间隔
    update_stats(&key, (__u64)len);
    return 0;
}

// =============================================================================
// BPF 入口函数 2: 备选 — __sock_sendmsg（通用降级路径）
// =============================================================================

/*
 * 函数: sock_sendmsg_entry
 * 挂点: SEC("kprobe/__sock_sendmsg")
 * 触发时机: 每当任何 AF_BLUETOOTH 协议族的 socket 发送数据时。
 *           __sock_sendmsg 是内核 socket 层通用的发送函数，
 *           所有协议族最终都会走到这里。
 * 主要逻辑:
 *   1. is_global_enabled() 快速返回
 *   2. 从 struct socket* 提取 struct sock*
 *   3. BPF_CORE_READ(sk, __sk_common.skc_family) 过滤 AF_BLUETOOTH
 *   4. 构造全零 BDADDR 的通配 key（无法区分具体设备）
 *   5. update_stats(&key, 1) 只能计数，无法区分设备
 * 写入的 Map: bt_traffic（通配条目）
 *
 * 什么时候走到这个挂点？
 *   当 l2cap_sock_sendmsg 在内核中不存在（旧内核）或挂点失败时，
 *   libbpf 尝试装载此备选。它无法提取 BDADDR，
 *   只能把所有蓝牙流量聚合到一个通配 key 里。
 *   对"检测蓝牙是否活跃"够用，但无法区分"哪台设备卡顿"。
 */
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

    // 过滤 AF_BLUETOOTH（31），跳过其他协议族（AF_INET/AF_UNIX 等）
    __u16 family = BPF_CORE_READ(sk, __sk_common.skc_family);
    if (family != AF_BLUETOOTH)
        return 0;

    // 无法提取 BDADDR（备选挂点没有设备上下文），用全零 BDADDR 作为通配 key
    // 所有蓝牙流量聚合到这一条目，direction=0（发送）
    struct device_key key = {0};
    key.direction = 0;

    // 无法准确获取长度，用 1 作为占位符
    // 但包间隔检测（gap_count/max_gap_ns）仍然有效，对卡顿检测至关重要
    update_stats(&key, 1);
    return 0;
}

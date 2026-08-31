/*
 * 文件: tcp_conn_stats.bpf.c
 * 功能: TCP 连接生命周期统计。通过 kretprobe 捕获服务端 accept 的入向连接，
 *       通过 kprobe/tcp_close 捕获其关闭，计算连接时长分布与每端口接受/关闭数。
 *
 * 挂载的探针类型和内核函数:
 *   - kretprobe/inet_csk_accept : 捕获入向 TCP 连接建立（返回值 = 新连接的 sock）
 *   - kprobe/tcp_close          : 捕获被跟踪连接的关闭（含正常关闭与进程退出时内核代为关闭）
 *
 * 使用的 BPF Map:
 *   - conn_start : LRU_HASH，key = sock 指针，value = accept 时间戳；
 *                  只有出现在此 map 中的连接才被跟踪（即本机 accept 的入向连接）
 *   - conn_stats : HASH，key=0 的单条目聚合统计（accept/close 计数、活跃连接数、时长分布）
 *   - conn_ports : LRU_HASH，key = 本地端口（主机序），value = 该端口的 accept/close 计数
 *
 * 与现有监控的关系: flow_rate / tcp_retransmit 统计流量与重传维度，
 * 本程序统计连接生命周期维度，互不重叠。
 *
 * 用户态对应的 Monitor 类: TcpConnMonitor
 */

#define __TARGET_ARCH_arm64
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>

#ifndef AF_INET
#define AF_INET 2
#endif
#ifndef AF_INET6
#define AF_INET6 10
#endif

char LICENSE[] SEC("license") = "GPL";

// =============================================================================
// 常量与数据结构定义
// =============================================================================

// conn_start 容量：入向并发连接数的兜底上限（LRU 自动淘汰最老条目，
// 被淘汰的连接关闭时将不再计入 close/时长统计，活跃计数可能出现小幅正漂移）
#define CONN_START_MAX_ENTRIES 8192
// 每端口统计表容量
#define CONN_PORTS_MAX_ENTRIES 128

// 连接时长分桶阈值（纳秒）：7 个桶
//   [0]<100ms [1]<1s [2]<10s [3]<1min [4]<5min [5]<1h [6]>=1h
#define HIST_BUCKET_COUNT 7

/*
 * 全局聚合统计（conn_stats 的 value，单条目 key=0）
 * 多 CPU 并发触发 kprobe，使用 __sync_fetch_and_add 原子累加。
 */
struct tcp_conn_global {
    __u64 total_accepts;         // 累计入向连接 accept 数
    __u64 total_accept_failures; // accept 返回错误指针的次数（客户端中止等）
    __u64 accepts_v4;            // IPv4 入向连接数
    __u64 accepts_v6;            // IPv6 入向连接数
    __u64 total_closes;          // 累计被跟踪入向连接关闭数
    __s64 active_inbound;        // 当前活跃入向连接数（accepts - tracked closes）
    __u64 total_duration_ns;     // 已关闭连接的时长总和（计算均值用）
    __u64 max_duration_ns;       // 已关闭连接的最大时长
    __u64 duration_count;        // 已完成时长统计的连接数
    __u64 hist[HIST_BUCKET_COUNT]; // 连接时长分桶计数
};

/*
 * 每端口 accept/close 计数（conn_ports 的 value）
 */
struct tcp_conn_port_value {
    __u64 accepts;
    __u64 closes;
};

// =============================================================================
// BPF Map 定义
// =============================================================================

/*
 * Map: conn_start
 * 类型: BPF_MAP_TYPE_LRU_HASH（带 LRU 淘汰，防止异常路径泄漏内存）
 * Key:  struct sock *（被 accept 的连接 sock 指针）
 * Value: __u64（accept 时刻，bpf_ktime_get_ns）
 * 用途: 标记“这是本机 accept 的入向连接”并记录起始时间；
 *       tcp_close 时若能在此 map 命中，则计入连接时长统计。
 *       出向连接 / 未被本进程 accept 的 socket 不会进入此 map，
 *       因此 tcp_close 对它们的触发会被自然过滤掉。
 */
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, CONN_START_MAX_ENTRIES);
    __type(key, struct sock *);
    __type(value, __u64);
} conn_start SEC(".maps");

/*
 * Map: conn_stats
 * 类型: BPF_MAP_TYPE_HASH（普通哈希表，单条目聚合）
 * Key:  __u32，固定值 0
 * Value: struct tcp_conn_global
 * 用途: 全局连接生命周期聚合统计。
 */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct tcp_conn_global);
} conn_stats SEC(".maps");

/*
 * Map: conn_ports
 * 类型: BPF_MAP_TYPE_LRU_HASH
 * Key:  __u16（监听端口，主机序；accept 出的子连接与监听者同端口）
 * Value: struct tcp_conn_port_value
 * 用途: 定位“哪个服务在接收连接”（如 sshd:22）。
 */
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, CONN_PORTS_MAX_ENTRIES);
    __type(key, __u16);
    __type(value, struct tcp_conn_port_value);
} conn_ports SEC(".maps");

// =============================================================================
// 辅助函数（static __always_inline，避免函数调用开销）
// =============================================================================

/*
 * 读取（或首次创建）全局聚合条目
 * 与 dns_monitor.bpf.c 的 update_dns_stats 一样采用 lookup-or-create 模式，
 * 但这里空闲期用户态会预创建条目（TcpConnMonitor::init），正常路径 lookup 必命中。
 */
static __always_inline struct tcp_conn_global *global_stats(void)
{
    __u32 key = 0;
    struct tcp_conn_global *g = bpf_map_lookup_elem(&conn_stats, &key);
    if (!g) {
        struct tcp_conn_global zero = {};
        bpf_map_update_elem(&conn_stats, &key, &zero, BPF_ANY);
        g = bpf_map_lookup_elem(&conn_stats, &key);
    }
    return g;
}

/*
 * 连接时长所属分桶下标
 * 分桶阈值: <100ms / <1s / <10s / <1min / <5min / <1h / >=1h
 */
static __always_inline __u32 duration_bucket(__u64 duration_ns)
{
    if (duration_ns < 100000000ULL)         return 0;  // <100ms
    if (duration_ns < 1000000000ULL)        return 1;  // <1s
    if (duration_ns < 10000000000ULL)       return 2;  // <10s
    if (duration_ns < 60000000000ULL)       return 3;  // <1min
    if (duration_ns < 300000000000ULL)      return 4;  // <5min
    if (duration_ns < 3600000000000ULL)     return 5;  // <1h
    return 6;                                          // >=1h
}

// =============================================================================
// BPF 入口函数 1: 入向连接建立
// =============================================================================

/*
 * kretprobe/inet_csk_accept
 *
 * inet_csk_accept 是 TCP 监听 socket accept 的内核路径，
 * 返回值为新建立的连接 sock（或 ERR_PTR 错误指针）。
 * 在这里：
 *   1. 错误指针 → accept_failures++（客户端 SYN 后立即 RST 等场景）
 *   2. 有效 sock → 记录起始时间到 conn_start，累加全局与每端口 accept 计数
 */
SEC("kretprobe/inet_csk_accept")
int BPF_KRETPROBE(trace_inet_csk_accept, struct sock *newsk)
{
    struct tcp_conn_global *g = global_stats();
    if (!g)
        return 0;

    unsigned long rc = (unsigned long)newsk;
    if (!rc || rc >= (unsigned long)-4095L) {
        // ERR_PTR 范围：accept 因信号中断 / 非阻塞无连接 / 队列错误而失败
        __sync_fetch_and_add(&g->total_accept_failures, 1);
        return 0;
    }

    __u16 family = BPF_CORE_READ(newsk, __sk_common.skc_family);
    // 子连接的本地端口 = 监听端口（skc_num 为主机序）
    __u16 port = BPF_CORE_READ(newsk, __sk_common.skc_num);

    // 记录连接起始时间
    __u64 now = bpf_ktime_get_ns();
    bpf_map_update_elem(&conn_start, &newsk, &now, BPF_ANY);

    __sync_fetch_and_add(&g->total_accepts, 1);
    __sync_fetch_and_add(&g->active_inbound, 1);
    if (family == AF_INET)
        __sync_fetch_and_add(&g->accepts_v4, 1);
    else if (family == AF_INET6)
        __sync_fetch_and_add(&g->accepts_v6, 1);

    // 每端口 accept 计数（lookup-or-create + 原子累加，避免并发丢计数）
    struct tcp_conn_port_value *pv = bpf_map_lookup_elem(&conn_ports, &port);
    if (!pv) {
        struct tcp_conn_port_value zero = {};
        bpf_map_update_elem(&conn_ports, &port, &zero, BPF_ANY);
        pv = bpf_map_lookup_elem(&conn_ports, &port);
    }
    if (pv)
        __sync_fetch_and_add(&pv->accepts, 1);

    return 0;
}

// =============================================================================
// BPF 入口函数 2: 连接关闭
// =============================================================================

/*
 * kprobe/tcp_close
 *
 * tcp_close 对所有 TCP socket 的关闭触发（包括出向连接）。
 * 只有在 conn_start 中命中的 sock（即本机 accept 的入向连接）才计入
 * 生命周期统计：关闭数、活跃数递减、连接时长与分桶。
 */
SEC("kprobe/tcp_close")
int BPF_KPROBE(trace_tcp_close, struct sock *sk)
{
    __u64 *start = bpf_map_lookup_elem(&conn_start, &sk);
    if (!start)
        return 0;  // 非被跟踪的入向连接（出向 socket / 未 accept 的 socket）

    __u64 now = bpf_ktime_get_ns();
    __u64 duration = now - *start;
    bpf_map_delete_elem(&conn_start, &sk);  // 先删除，保证 close 只统计一次

    struct tcp_conn_global *g = global_stats();
    if (!g)
        return 0;

    __sync_fetch_and_add(&g->total_closes, 1);
    __sync_fetch_and_add(&g->active_inbound, -1);
    __sync_fetch_and_add(&g->duration_count, 1);
    __sync_fetch_and_add(&g->total_duration_ns, duration);
    if (duration > g->max_duration_ns)
        g->max_duration_ns = duration;
    __u32 idx = duration_bucket(duration);
    __sync_fetch_and_add(&g->hist[idx], 1);

    __u16 port = BPF_CORE_READ(sk, __sk_common.skc_num);
    struct tcp_conn_port_value *pv = bpf_map_lookup_elem(&conn_ports, &port);
    if (pv)
        __sync_fetch_and_add(&pv->closes, 1);

    return 0;
}

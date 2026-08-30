/*
 * 文件: dns_monitor.bpf.c
 * 功能: DNS 解析延迟监控。通过 kprobe 挂点捕获 DNS 请求发送和响应接收，
 *       计算端到端解析延迟，检测超时和失败情况。
 *
 * 挂载的探针类型和内核函数:
 *   - kprobe/udp_sendmsg : 捕获 DNS 请求发送（目的端口 53）
 *   - kprobe/udp_recvmsg : 捕获 DNS 响应接收（源端口 53）
 *
 * 使用的 BPF Map:
 *   - dns_queries  : LRU_HASH，存储进行中的 DNS 查询，收到响应时匹配并计算延迟
 *   - dns_stats    : HASH，key=0 的单条目聚合统计（总查询数、响应数、超时数、平均/最大延迟）
 *
 * 用户态对应的 Monitor 类: DnsMonitor
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

// DNS 标准端口号（主机序和网络序两种写法兼容不同内核）
#define DNS_PORT 53
// DNS 超时判定阈值：5 秒内未收到响应视为超时
#define DNS_TIMEOUT_NS 5000000000ULL  // 5 秒 = 5,000,000,000 纳秒

char LICENSE[] SEC("license") = "GPL";

// =============================================================================
// 数据结构定义
// =============================================================================

/*
 * DNS 查询标识（作为 Map Key）
 * 使用 3 元组 (saddr, daddr, sport) 来唯一标识一次 DNS 查询：
 *   saddr = 本机 IP（发起方）
 *   daddr = DNS 服务器 IP（目的方）
 *   sport = 本机随机 UDP 源端口（用于匹配请求和响应）
 */
struct dns_query_key {
    __u32 saddr;     // 源 IP（网络字节序存储在 skc_rcv_saddr 中）
    __u32 daddr;     // 目的 IP（网络字节序存储在 skc_daddr 中）
    __u16 sport;     // 源端口（主机序，直接来自 skc_num）
} __attribute__((packed));  // packed 确保无填充字节，保证 Map Key 唯一性

/*
 * DNS 查询记录（存储在 dns_queries Map 中）
 */
struct dns_query_record {
    __u64 send_time_ns;   // 请求发送时间戳（纳秒，由 bpf_ktime_get_ns() 获取）
    __u64 recv_time_ns;   // 响应接收时间戳；0 表示尚未收到响应（即超时）
    __u32 reply_len;      // DNS 响应报文长度（字节）
    __u8  rcode;          // DNS 响应码：0=成功，3=NXDOMAIN 等
    __u8  is_response;    // 标记此记录当前是请求(0)还是已匹配响应(1)
};

/*
 * DNS 聚合统计记录（存储在 dns_stats Map 中，全局单条）
 * 用户态定期读取此 Map 即可获得累计 DNS 健康指标
 */
struct dns_stats_record {
    __u64 total_queries;     // DNS 请求总数（每次 sendmsg 入口 +1）
    __u64 total_responses;   // 成功收到响应的次数
    __u64 total_timeouts;    // 超时次数（5s 内无响应）
    __u64 total_errors;      // DNS 响应异常次数（rcode != 0）
    __u64 total_latency_ns;  // 所有成功响应的延迟总和（用于计算平均值 = total_latency_ns / total_responses）
    __u64 max_latency_ns;    // 观测到的最大延迟（p99 近似值）
};

// =============================================================================
// BPF Map 定义
// =============================================================================

/*
 * Map: dns_queries
 * 类型: BPF_MAP_TYPE_LRU_HASH（带最近最少使用淘汰的哈希表，防止内存泄漏）
 * Key:  struct dns_query_key（源 IP / 目的 IP / 源端口三元组）
 * Value: struct dns_query_record（查询记录）
 * 最大条目数: 256
 * 用途: 存储进行中的 DNS 查询请求。当响应到达时，用相同的三元组反向查找
 *        并计算延迟。LRU 策略会自动淘汰超时未匹配的旧条目。
 */
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 256);
    __type(key, struct dns_query_key);
    __type(value, struct dns_query_record);
} dns_queries SEC(".maps");

/*
 * Map: dns_stats
 * 类型: BPF_MAP_TYPE_HASH（普通哈希表，单条目聚合）
 * Key:  __u32，固定值 0（用单条目存全局统计，避免 PERCPU 额外合并开销）
 * Value: struct dns_stats_record
 * 最大条目数: 1
 * 用途: 聚合所有 DNS 健康指标。使用 __sync_fetch_and_add 原子操作保证
 *        并发安全（多个 CPU 同时触发 KPROBE）。
 */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1);
    __type(key, __u32); // 固定 key=0
    __type(value, struct dns_stats_record);
} dns_stats SEC(".maps");

// =============================================================================
// 辅助函数（static __always_inline，避免函数调用开销）
// =============================================================================

/*
 * 判断 UDP 端口是否为 DNS 端口（53）
 * bpf_htons 将主机序（小端）转为网络序（大端），
 * 因为 skc_daddr/dport 在内核中存储为网络序，需要兼容两种情况。
 */
static __always_inline bool is_dns_port(__u16 port)
{
    return port == DNS_PORT || port == bpf_htons(DNS_PORT);
}

/*
 * 更新全局 DNS 聚合统计
 * @latency_ns  本次延迟（纳秒），超时则为 0
 * @is_timeout  是否超时
 * @is_error    是否 DNS 响应码异常（rcode != 0）
 *
 * 使用 __sync_fetch_and_add 原子内置函数实现无锁累加，
 * 适用于 BPF_MAP_TYPE_HASH 中 Value 被并发写入的场景。
 */
static __always_inline void update_dns_stats(__u64 latency_ns, bool is_timeout, bool is_error)
{
    __u32 key = 0;
    struct dns_stats_record *stat = bpf_map_lookup_elem(&dns_stats, &key);
    if (!stat) {
        // Map 中尚无条目，创建初始化记录
        struct dns_stats_record init = {0};
        init.total_queries = 1;
        init.total_responses = is_timeout ? 0 : 1;
        init.total_timeouts = is_timeout ? 1 : 0;
        init.total_errors = is_error ? 1 : 0;
        init.total_latency_ns = is_timeout ? 0 : latency_ns;
        init.max_latency_ns = is_timeout ? 0 : latency_ns;
        bpf_map_update_elem(&dns_stats, &key, &init, BPF_ANY);
    } else {
        // 原子累加各字段
        __sync_fetch_and_add(&stat->total_queries, 1);
        if (!is_timeout) {
            __sync_fetch_and_add(&stat->total_responses, 1);
            __sync_fetch_and_add(&stat->total_latency_ns, latency_ns);
            if (latency_ns > stat->max_latency_ns)
                stat->max_latency_ns = latency_ns;
        } else {
            __sync_fetch_and_add(&stat->total_timeouts, 1);
        }
        if (is_error)
            __sync_fetch_and_add(&stat->total_errors, 1);
    }
}

// =============================================================================
// BPF 入口函数 1: DNS 请求发送
// =============================================================================

/*
 * 函数: trace_dns_send
 * 挂点: SEC("kprobe/udp_sendmsg")
 * 触发时机: 每当内核调用 udp_sendmsg() 时（任何 UDP socket 发送数据）。
 *           内核函数签名: int udp_sendmsg(struct sock *sk, struct msghdr *msg, ...)
 * 主要逻辑:
 *   1. 从第一个参数 pt_regs 获取 struct sock*（内核 socket 结构体）
 *   2. 过滤出 AF_INET (IPv4) 家族
 *   3. 用 skc_dport 检查目的端口是否为 53（DNS）
 *   4. 从 skc_rcv_saddr/skc_daddr/skc_num 提取 IP 和端口，构造查询 Key
 *   5. 记录发送时间戳 bpf_ktime_get_ns() 并写入 dns_queries Map
 *   6. 更新全局统计 total_queries
 * 写入的 Map:
 *   - dns_queries（更新/插入 key 对应的查询记录）
 *   - dns_stats（累加 total_queries）
 */
SEC("kprobe/udp_sendmsg")
int trace_dns_send(struct pt_regs *ctx)
{
    // PT_REGS_PARM1(ctx) 取出 kprobe 第一个参数：struct sock *sk
    // 这是 libbpf 提供的宏，适配不同架构（x86/arm64）的寄存器调用约定
    struct sock *sk = (struct sock *)PT_REGS_PARM1(ctx);
    if (!sk)
        return 0;

    // BPF_CORE_READ 是 CO-RE（Compile Once, Run Everywhere）的核心宏，
    // 它会根据内核 BTF 自动获取正确的结构体偏移，编译时生成 reloc，
    // 运行时由 libbpf 解析，从而实现跨内核版本兼容性。
    __u16 family = BPF_CORE_READ(sk, __sk_common.skc_family);
    if (family != AF_INET)
        return 0;  // 只处理 IPv4（可扩展 AF_INET6）

    // skc_num：主机序，存储本地端口号
    // skc_dport：网络序，存储对端端口号（因为 UDP 头中 dport 是大端存储）
    __u16 sport = BPF_CORE_READ(sk, __sk_common.skc_num);
    __u16 dport = BPF_CORE_READ(sk, __sk_common.skc_dport);

    // 过滤目的端口 53：只有发往 DNS 服务器的 UDP 包才是 DNS 请求
    if (!is_dns_port(dport))
        return 0;

    // skc_rcv_saddr：本机绑定的源 IP（内核语义）
    // skc_daddr：对端目的 IP
    __u32 saddr = BPF_CORE_READ(sk, __sk_common.skc_rcv_saddr);
    __u32 daddr = BPF_CORE_READ(sk, __sk_common.skc_daddr);

    // 构造 dns_query_key，sport 直接用主机序，
    // 因为 skc_num 在内核里本身就是主机序存储
    struct dns_query_key key = {0};
    key.saddr = saddr;
    key.daddr = daddr;
    key.sport = sport;

    // bpf_ktime_get_ns()：获取当前内核单调时钟时间戳（纳秒）。
    // 基于 sched_clock()，不随系统时间调整而变化，适合测延迟。
    struct dns_query_record rec = {0};
    rec.send_time_ns = bpf_ktime_get_ns();
    rec.is_response = 0;
    // BPF_ANY：存在则覆盖，不存在则创建。LRU Hash 会自动淘汰旧条目
    // 防止在高 DNS 请求率下内存泄漏
    bpf_map_update_elem(&dns_queries, &key, &rec, BPF_ANY);

    // 发送请求即累计 total_queries，不依赖响应路径
    __u32 stats_key = 0;
    struct dns_stats_record *stat = bpf_map_lookup_elem(&dns_stats, &stats_key);
    if (stat) {
        // __sync_fetch_and_add：GCC 原子内置函数，在 BPF JIT 中会编译
        // 成单条 lock add 指令，保证多核并发安全
        __sync_fetch_and_add(&stat->total_queries, 1);
    } else {
        struct dns_stats_record init = {0};
        init.total_queries = 1;
        bpf_map_update_elem(&dns_stats, &stats_key, &init, BPF_ANY);
    }

    return 0;
}

// =============================================================================
// BPF 入口函数 2: DNS 响应接收
// =============================================================================

/*
 * 函数: trace_dns_recv
 * 挂点: SEC("kprobe/udp_recvmsg")
 * 触发时机: 每当内核调用 udp_recvmsg() 时（任何 UDP socket 接收数据）。
 * 主要逻辑:
 *   1. 获取 struct sock*，过滤 IPv4
 *   2. 判断当前 socket 是否属于 DNS 通信（dport==53 或 sport==53）
 *   3. 从 dns_queries Map 中查找匹配的发送记录
 *   4. 用 bpf_ktime_get_ns() 计算端到端延迟
 *   5. 调用 update_dns_stats() 更新聚合统计
 *   6. 删除已匹配的查询记录（LRU 不再需要）
 * 写入的 Map:
 *   - dns_queries（删除已匹配条目）
 *   - dns_stats（update_dns_stats 内部累加）
 *
 * 注意：此处存在"socket 方向"问题：
 *   - 客户端 socket 接收 DNS 响应时，skc_num=客户端随机端口, skc_dport=53
 *   - DNS 服务器 socket 接收请求时，skc_num=53, skc_dport=客户端端口
 *   我们用 is_server_side 来区分两种方向，Key 构造需保证与发送侧一致。
 */
SEC("kprobe/udp_recvmsg")
int trace_dns_recv(struct pt_regs *ctx)
{
    struct sock *sk = (struct sock *)PT_REGS_PARM1(ctx);
    if (!sk)
        return 0;

    __u16 family = BPF_CORE_READ(sk, __sk_common.skc_family);
    if (family != AF_INET)
        return 0;

    __u16 sport = BPF_CORE_READ(sk, __sk_common.skc_num);
    __u16 dport = BPF_CORE_READ(sk, __sk_common.skc_dport);

    // 同时检查 dport==53 和 sport==53：
    //   dport==53 → 服务器 socket（在监听 DNS 请求）
    //   sport==53 → 客户端 socket（从 DNS 服务器 53 端口接收响应）
    bool is_server_side = is_dns_port(dport);
    if (!is_server_side && !is_dns_port(sport))
        return 0;

    __u32 saddr = BPF_CORE_READ(sk, __sk_common.skc_rcv_saddr);
    __u32 daddr = BPF_CORE_READ(sk, __sk_common.skc_daddr);

    // 构造 Key，确保与发送侧一致：
    //   发送侧（客户端 sock）: saddr=本机, daddr=DNS, sport=随机端口
    //   接收侧（客户端 sock）: skc_rcv_saddr=本机, skc_daddr=DNS
    //   接收侧（服务器 sock）: skc_rcv_saddr=服务器, skc_daddr=客户端
    struct dns_query_key key = {0};
    if (is_server_side) {
        // 服务器端 socket：saddr=服务器IP, daddr=客户端IP
        key.saddr = saddr;
        key.daddr = daddr;
        key.sport = sport;
    } else {
        // 客户端 socket 收到响应：Key 需与发送侧对齐
        // 发送侧在服务器 socket 上 saddr=服务器, daddr=客户端
        // 因此这里反转 saddr/daddr 来匹配
        key.saddr = daddr;
        key.daddr = saddr;
        key.sport = sport;
    }

    // 在 dns_queries 中查找匹配的发送记录
    struct dns_query_record *rec = bpf_map_lookup_elem(&dns_queries, &key);
    if (!rec || rec->send_time_ns == 0)
        return 0;  // 未找到匹配的请求（可能是其他进程的 DNS 流量，或已超时被 LRU 淘汰）

    // 计算延迟：当前时间 - 发送时间
    __u64 now = bpf_ktime_get_ns();
    __u64 latency = now - rec->send_time_ns;

    // 更新查询记录
    rec->recv_time_ns = now;
    rec->is_response = 1;
    rec->reply_len = 0;  // 暂未解析实际响应长度（需要额外读 sk_buff，此处简化）

    // 调用辅助函数更新聚合统计
    update_dns_stats(latency, false, false);

    // 清理已匹配的查询记录，LRU Hash 不再保留无意义的条目
    bpf_map_delete_elem(&dns_queries, &key);

    return 0;
}

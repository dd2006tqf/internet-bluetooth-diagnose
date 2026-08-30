/*
 * 文件: http_latency.bpf.c
 * 功能: HTTP 请求级延迟追踪。通过挂载 TCP 发送/接收路径的 kprobe，
 *       从用户态 msghdr.msg_iter.iov 中提取明文 HTTP 请求/响应首部，
 *       计算 TTFB（Time To First Byte，首字节延迟），
 *       用于区分"应用慢"（服务器处理延迟长）vs "网络慢"（传输延迟长）。
 *
 * 挂载的探针类型和内核函数:
 *   - kprobe/tcp_sendmsg           : 捕获 HTTP 请求发送（读取用户态明文 GET/POST 等）
 *   - kprobe/tcp_recvmsg_locked    : entry probe，保存 sk + msg 上下文
 *   - kretprobe/tcp_recvmsg_locked : return probe，读取 entry 保存的上下文，匹配 HTTP 响应
 *
 * 设计说明: 为什么用 entry+return 配对？
 *   kretprobe 的 PT_REGS_PARM1 是返回值，不是入口参数。
 *   因此必须在 entry kprobe 时保存 struct sock *sk 和 struct msghdr *msg 到 Map，
 *   在 retprobe 时读回，才能正确提取 socket 和 I/O 缓冲区信息。
 *
 * 使用的 BPF Map:
 *   - http_txn_stats  : LRU_HASH，存储进行中的 HTTP 事务（请求→响应配对）
 *   - recvmsg_ctx_map : HASH，key=PID，保存 tcp_recvmsg 的 entry 上下文供 retprobe 使用
 *   - http_debug      : ARRAY，64 槽位的调试计数器，用 dbg_inc(idx) 验证各代码分支是否命中
 *
 * 用户态对应的 Monitor 类: HttpLatencyMonitor
 */

#define __TARGET_ARCH_arm64
#define AF_INET 2
#define AF_INET6 10
// 只读取用户态缓冲区前 64 字节用于 HTTP 方法/状态码识别
// 原因：BPF 栈大小限制（默认 512 字节），不能读太大
#define MAX_HTTP_PAYLOAD 64

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
 * TCP 连接标识 Key（支持 IPv4/IPv6）
 * IPv4 时 saddr[3]/daddr[3] 存 32 位地址，[0]-[2] 为 0
 * IPv6 时 saddr/daddr 为完整 128 位地址
 */
struct tcp_conn_key {
    __u32 saddr[4];  // 源地址（IPv4 低 32 位有效；IPv6 完整 128 位）
    __u32 daddr[4];  // 目的地址
    __u16 sport;     // 源端口（网络序）
    __u16 dport;     // 目的端口（网络序）
};

/* HTTP 事务 Key —— 与 tcp_conn_key 结构相同 */
struct http_txn_key {
    __u32 saddr[4];
    __u32 daddr[4];
    __u16 sport;
    __u16 dport;
};

/*
 * HTTP 事务记录（存储在 http_txn_stats Map 中）
 * 一次 HTTP 请求→响应周期：
 *   is_request=1 → 请求已发送，等待响应
 *   is_request=0 → 响应已接收，记录完整
 * 用户态读取后可计算 TTFB = recv_ns - send_ns
 */
struct http_txn_record {
    __u64 send_ns;       // HTTP 请求发送时间戳（bpf_ktime_get_ns）
    __u64 recv_ns;       // HTTP 响应首字节到达时间戳
    __u32 req_bytes;     // 请求体字节数（tcp_sendmsg 的 size 参数）
    __u32 resp_bytes;    // 响应体字节数（tcp_recvmsg 的返回值 ret）
    __u16 status_code;   // HTTP 状态码（200/301/404/500 等）
    __u8  is_request;    // 标记：1=仅请求，0=请求+响应完整
    __u8  padding;       // 对齐填充，不使用
};

/*
 * tcp_recvmsg 的 entry→retprobe 配对上下文
 * kretprobe 无法直接拿到入口的参数，需要用 PID 作为中间键保存
 */
struct recvmsg_ctx {
    struct sock *sk;      // TCP socket 指针（用于构造连接 Key）
    struct msghdr *msg;   // 消息头指针（用于读用户态 iov 里的 HTTP 响应明文）
};

// =============================================================================
// BPF Map 定义
// =============================================================================

/*
 * Map: http_txn_stats
 * 类型: BPF_MAP_TYPE_LRU_HASH（最近最少使用淘汰）
 * Key:  struct http_txn_key（TCP 连接 4 元组）
 * Value: struct http_txn_record（HTTP 事务记录）
 * 最大条目数: 8192（同时进行的 HTTP 连接上限）
 * 用途: 存储进行中的 HTTP 请求。tcp_sendmsg 入口创建，
 *       tcp_recvmsg 返回时匹配并补全响应信息。LRU 淘汰保证长时间无响应
 *       的连接不会撑爆内存。
 */
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 8192);
    __type(key, struct http_txn_key);
    __type(value, struct http_txn_record);
} http_txn_stats SEC(".maps");

/*
 * Map: recvmsg_ctx_map
 * 类型: BPF_MAP_TYPE_HASH
 * Key:  __u32 = PID（bpf_get_current_pid_tgid() 高 32 位）
 * Value: struct recvmsg_ctx（sk + msg 指针）
 * 最大条目数: 1024
 * 用途: entry kprobe 保存 tcp_recvmsg 的入口参数，retprobe 时读回。
 *       之所以用 PID 而不是 sk，是因为 entry 和 retprobe 之间可能有
 *       其他 socket 的 recvmsg 穿插，PID 保证是同一个 syscall 上下文。
 *       retprobe 读完后立即 delete，避免残留。
 */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, __u32);
    __type(value, struct recvmsg_ctx);
} recvmsg_ctx_map SEC(".maps");

/*
 * Map: http_debug
 * 类型: BPF_MAP_TYPE_ARRAY（固定大小数组，所有 CPU 共享同一个槽位）
 * Key:  __u32（数组索引 0~63）
 * Value: __u64（计数器）
 * 最大条目数: 64
 * 用途: 调试辅助。dbg_inc(idx) 在指定索引处 +1，用户态读出来可以
 *       精确知道每个 BPF 代码分支命中了多少次。
 *       对 eBPF 验证器友好（ARRAY 是预分配固定大小，无锁竞争）。
 */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 64);
    __type(key, __u32);
    __type(value, __u64);
} http_debug SEC(".maps");

// =============================================================================
// 调试辅助函数
// =============================================================================

/*
 * 在 http_debug Map 的 idx 槽位自增 1
 * 先 lookup 再判断是否存在：ARRAY Map 的 slot 总是存在（预分配），
 * 但首次 lookup 可能返回 NULL（刚加载时），需要处理。
 */
static __always_inline void dbg_inc(__u32 idx) {
    __u32 k = idx;
    __u64 *v = bpf_map_lookup_elem(&http_debug, &k);
    if (v) __sync_fetch_and_add(v, 1);  // 原子自增
    else { __u64 one = 1; bpf_map_update_elem(&http_debug, &k, &one, BPF_ANY); }
}

// =============================================================================
// HTTP 明文检测辅助函数
// =============================================================================

/*
 * 检测用户态/内核态缓冲区中的数据是否为 HTTP 请求（GET/POST/PUT/DELETE/HEAD 开头）
 * @data_start 数据起始地址（内核空间，用 bpf_probe_read_kernel 读）
 * 返回: 1=是 HTTP 请求方法，0=不是
 *
 * bpf_probe_read_kernel：BPF 辅助函数 #11，从内核空间读取数据到栈上。
 * 必须用它访问内核结构体成员，直接解引用指针会被验证器拒绝。
 */
static __always_inline int check_http_request(void *data_start)
{
    char prefix[8] = {};  // 栈上分配 8 字节（BPF 栈限制 512B，够用）
    if (bpf_probe_read_kernel(prefix, sizeof(prefix), data_start) < 0)
        return 0;
    // 各 HTTP 方法的 ASCII 特征前缀匹配
    if (prefix[0]=='G' && prefix[1]=='E' && prefix[2]=='T' && prefix[3]==' ')
        return 1;  // "GET "
    if (prefix[0]=='P' && prefix[1]=='O' && prefix[2]=='S' && prefix[3]=='T')
        return 1;  // "POST"
    if (prefix[0]=='P' && prefix[1]=='U' && prefix[2]=='T' && prefix[3]==' ')
        return 1;  // "PUT "
    if (prefix[0]=='D' && prefix[1]=='E' && prefix[2]=='L' && prefix[3]=='E')
        return 1;  // "DELE"（DELETE）
    if (prefix[0]=='H' && prefix[1]=='E' && prefix[2]=='A' && prefix[3]=='D')
        return 1;  // "HEAD"
    return 0;
}

/*
 * 检测数据是否为 HTTP 响应（以 "HTTP/1.0" 或 "HTTP/1.1" 开头）
 * @data_start 内核空间指针
 * 返回: 1=是 HTTP 响应，0=不是
 */
static __always_inline int check_http_response(void *data_start)
{
    char prefix[12] = {};
    if (bpf_probe_read_kernel(prefix, sizeof(prefix), data_start) < 0)
        return 0;
    // 匹配 "HTTP/1"（兼容 HTTP/1.0 和 HTTP/1.1）
    if (prefix[0]=='H' && prefix[1]=='T' && prefix[2]=='T' && prefix[3]=='P' &&
        prefix[4]=='/' && prefix[5]=='1')
        return 1;
    return 0;
}

/*
 * 从 HTTP 响应首部提取状态码（如 "HTTP/1.1 200 OK\r\n" 中的 "200"）
 * 格式偏移分析:
 *   "HTTP/1.1 " 共 9 字节（H T T P / 1 . 1 空格），
 *   状态码从 offset + 9 开始，共 3 位数字
 * 返回: 状态码（如 200, 404, 500），解析失败返回 0
 */
static __always_inline __u16 extract_status_code(void *data_start)
{
    char buf[16] = {};
    // +9 跳过 "HTTP/1.1 " 前缀，从状态码开始读 16 字节
    if (bpf_probe_read_kernel(buf, sizeof(buf), data_start + 9) < 0)
        return 0;
    __u16 code = 0;
    // 3 位状态码的每个 ASCII 字符必须是 '0'-'9'
    if (buf[0] >= '1' && buf[0] <= '9' && buf[1] >= '0' && buf[1] <= '9' && buf[2] >= '0' && buf[2] <= '9')
        code = (buf[0]-'0')*100 + (buf[1]-'0')*10 + (buf[2]-'0');
    return code;
}

/*
 * 从 struct sock 构造 TCP 连接 Key（IPv4 或 IPv6）
 * IPv4: 从 skc_rcv_saddr / skc_daddr 取 32 位，放在数组最后一个元素（saddr[3]）
 * IPv6: 用 bpf_probe_read_kernel 从 sock 内偏移读取 skc_v6_rcv_saddr / skc_v6_daddr
 *        （BPF_CORE_READ 对 in6_addr 数组支持好，但用 bpf_probe_read_kernel 更灵活）
 * 端口: sport = skc_num 是主机序，用 bpf_htons 转网络序；dport 内核直接存网络序
 */
static __always_inline struct tcp_conn_key get_conn_key(struct sock *sk)
{
    struct tcp_conn_key k = {};
    __u16 family = BPF_CORE_READ(sk, __sk_common.skc_family);
    if (family == AF_INET) {
        // IPv4：读 32 位地址
        __u32 saddr4 = BPF_CORE_READ(sk, __sk_common.skc_rcv_saddr);
        __u32 daddr4 = BPF_CORE_READ(sk, __sk_common.skc_daddr);
        k.saddr[3] = saddr4;
        k.daddr[3] = daddr4;
    } else if (family == AF_INET6) {
        // IPv6：手动算偏移读取 in6_addr（16 字节）
        // offsetof(struct sock, __sk_common) + offsetof(struct sock_common, skc_v6_rcv_saddr)
        bpf_probe_read_kernel(k.saddr, sizeof(k.saddr),
            (char *)sk + offsetof(struct sock, __sk_common) + offsetof(struct sock_common, skc_v6_rcv_saddr));
        bpf_probe_read_kernel(k.daddr, sizeof(k.daddr),
            (char *)sk + offsetof(struct sock, __sk_common) + offsetof(struct sock_common, skc_v6_daddr));
    } else {
        return k;  // 其他协议族（AF_UNIX 等）返回全零 Key，调用方应跳过
    }
    // skc_num 是主机序（小端），转为网络序（大端）存入 Key
    __u16 sport_host = BPF_CORE_READ(sk, __sk_common.skc_num);
    k.sport = bpf_htons(sport_host);
    // skc_dport 内核已存网络序，直接使用
    k.dport = BPF_CORE_READ(sk, __sk_common.skc_dport);
    return k;
}

// =============================================================================
// 用户态 msghdr 读取辅助
// =============================================================================

/*
 * 从用户态 msg->msg_iter.iov 读取 HTTP 明文前 buf_sz 字节
 * TCP 层明文发送走 tcp_sendmsg()，此时用户态数据还未被加密（非 TLS），
 * 直接在 sock->sk 上调用即得到应用层明文。
 *
 * msghdr 结构层次:
 *   msghdr → msg_iter (union) → iovec* (数组) → iov_base (用户态 buffer 指针)
 *                                        → iov_len (buffer 长度)
 *
 * 关键约束: eBPF 验证器要求所有循环边界固定，这里只读**第一段 iov**
 * （大多数 HTTP 客户端 send 都是单段，首段即含请求首部）。
 * bpf_probe_read_user：BPF 辅助函数 #116，安全读取用户态内存，
 *                      遇到非法地址不会 OOM，而是返回错误码。
 *
 * @return 实际读取字节数，失败返回 0
 */
static __always_inline int read_http_user(void *http_buf, __u32 buf_sz,
                                               struct msghdr *msg)
{
    // BPF_CORE_READ 从用户态 msghdr 结构读 msg_iter.iov 指针
    // 注意：msg 本身是内核传入的指针，但 msg_iter.iov 指向的是用户态虚拟地址空间
    struct iovec *iov = (struct iovec *)BPF_CORE_READ(msg, msg_iter.iov);
    if (!iov) { dbg_inc(72); return 0; }

    // msg_iter.count 是 iov 数组段数
    __u32 nr = BPF_CORE_READ(msg, msg_iter.count);
    if (nr == 0) { dbg_inc(73); return 0; }
    dbg_inc(74);

    // 只读第一段 iov：大多数 HTTP 客户端（curl, libcurl）send 都只传一段
    void *base = BPF_CORE_READ(iov, iov_base);
    __u64 len = BPF_CORE_READ(iov, iov_len);
    if (!base || len == 0) { dbg_inc(77); return 0; }

    // 不超过调用方缓冲区大小
    __u32 take = buf_sz;
    if (len < take) take = (__u32)len;

    // bpf_probe_read_user：从用户态地址空间安全读取，遇到 SIGSEGV 也能优雅返回
    if (bpf_probe_read_user(http_buf, take, base) != 0) { dbg_inc(75); return 0; }
    dbg_inc(76);
    return (int)take;
}

// =============================================================================
// BPF 入口函数 1: tcp_sendmsg — 捕获 HTTP 请求
// =============================================================================

/*
 * 函数: probe_http_req
 * 挂点: SEC("kprobe/tcp_sendmsg")
 * 触发时机: 每当内核调用 tcp_sendmsg() 发送 TCP 数据时。
 *           内核函数签名: int tcp_sendmsg(struct sock *sk, struct msghdr *msg, size_t size)
 *           使用 BPF_KPROBE 宏自动从 pt_regs 提取前三个参数。
 * 主要逻辑:
 *   1. 调用 read_http_user() 从用户态 msghdr 读前 64 字节明文
 *   2. check_http_request() 识别 HTTP 请求方法前缀
 *   3. get_conn_key() 从 struct sock 构造 IPv4/IPv6 连接 Key
 *   4. 在 http_txn_stats Map 中创建请求记录，写入 send_ns 和 req_bytes
 * 写入的 Map: http_txn_stats（插入新条目，is_request=1）
 */
SEC("kprobe/tcp_sendmsg")
int BPF_KPROBE(probe_http_req, struct sock *sk, struct msghdr *msg, size_t size)
{
    if (!sk || !msg) return 0;
    dbg_inc(0);  // 分支 0：进入 tcp_sendmsg

    // size 是本次 send 的总字节数；<=0 表示无实际数据
    if (size <= 0) return 0;

    // 栈上分配 64 字节缓冲区，read_http_user 会填充
    char http_buf[MAX_HTTP_PAYLOAD] = {};
    int n = read_http_user(http_buf, sizeof(http_buf), msg);
    if (n <= 0) { dbg_inc(2); return 0; }
    dbg_inc(3);

    // 检查明文是否以 HTTP 方法开头
    if (!check_http_request(http_buf)) { dbg_inc(4); return 0; }
    dbg_inc(5);

    struct tcp_conn_key key = get_conn_key(sk);
    // 检查地址是否全零（get_conn_key 对非 IPv4/IPv6 会返回全零 Key）
    // 用 unroll 的目的：让验证器知道循环次数固定，允许优化
    __u32 addr_sum = 0;
    #pragma unroll
    for (int i = 0; i < 4; i++) { addr_sum |= key.saddr[i] | key.daddr[i]; }
    if (addr_sum == 0) { dbg_inc(6); return 0; }

    // 组装 HTTP 事务记录，写入 Map
    struct http_txn_record rec = {};
    rec.send_ns = bpf_ktime_get_ns();  // 请求发送时间戳
    rec.req_bytes = (__u32)size;       // 本次请求的总字节数
    rec.is_request = 1;                 // 标记：仅请求，等待响应
    bpf_map_update_elem(&http_txn_stats, &key, &rec, BPF_ANY);
    return 0;
}

// =============================================================================
// BPF 入口函数 2: tcp_recvmsg entry — 保存上下文
// =============================================================================

/*
 * 函数: trace_recvmsg_entry
 * 挂点: SEC("kprobe/tcp_recvmsg_locked")
 * 触发时机: 每当内核进入 tcp_recvmsg_locked() 时（TCP 数据接收入口，持锁版本）。
 * 主要逻辑: 以当前 PID 为 key，将 sk + msg 保存到 recvmsg_ctx_map。
 *           retprobe 之后将用同一个 PID 取回上下文。
 * 写入的 Map: recvmsg_ctx_map（key=PID）
 *
 * 为什么需要这个 entry→return 配对？
 *   kretprobe 触发时，内核函数已经返回，PT_REGS_PARM1 是返回值（long ret），
 *   不再是入口参数。但我们还需要 struct sock* 来构造连接 Key，
 *   以及 struct msghdr* 来读用户态响应明文。
 *   所以必须在 entry 时把这两个指针存起来，retprobe 时再取。
 */
SEC("kprobe/tcp_recvmsg_locked")
int BPF_KPROBE(trace_recvmsg_entry, struct sock *sk, struct msghdr *msg, size_t len, int flags)
{
    if (!sk || !msg) return 0;
    // bpf_get_current_pid_tgid() 返回 64 位值: 高 32 位 = PID，低 32 位 = TID
    // 右移 32 位拿 PID 部分作为 Map Key（同一个线程的 entry 和 retprobe 同 PID）
    __u32 pid = (__u32)(bpf_get_current_pid_tgid() >> 32);
    struct recvmsg_ctx saved = { .sk = sk, .msg = msg };
    bpf_map_update_elem(&recvmsg_ctx_map, &pid, &saved, BPF_ANY);
    return 0;
}

// =============================================================================
// BPF 入口函数 3: tcp_recvmsg retprobe — 匹配 HTTP 响应
// =============================================================================

/*
 * 函数: trace_recvmsg_return
 * 挂点: SEC("kretprobe/tcp_recvmsg_locked")
 * 触发时机: 每当 tcp_recvmsg_locked() 返回时。BPF_KPROBE 把返回值 ret 作为参数。
 * 主要逻辑:
 *   1. 用 PID 从 recvmsg_ctx_map 取回 entry 时保存的 sk + msg
 *   2. 用 sk 构造连接 Key，在 http_txn_stats 中查找等待响应的请求
 *   3. 从 msg->msg_iter.iov[0].iov_base 读用户态接收缓冲区（HTTP 响应明文）
 *   4. check_http_response() 识别 "HTTP/1." 前缀
 *   5. extract_status_code() 解析 HTTP 状态码
 *   6. 更新事务记录：设置 recv_ns、resp_bytes、status_code、is_request=0
 *   7. 删除 recvmsg_ctx_map 中的临时条目（清理）
 * 写入的 Map:
 *   - http_txn_stats（匹配到的条目补全响应信息）
 *   - recvmsg_ctx_map（delete 清理）
 */
SEC("kretprobe/tcp_recvmsg_locked")
int BPF_KPROBE(trace_recvmsg_return, long ret)
{
    dbg_inc(90);

    // 用 PID 取回 entry probe 保存的上下文
    __u32 pid = (__u32)(bpf_get_current_pid_tgid() >> 32);
    struct recvmsg_ctx *saved = bpf_map_lookup_elem(&recvmsg_ctx_map, &pid);
    if (!saved || !saved->sk || !saved->msg) { dbg_inc(91); return 0; }
    struct sock *sk = saved->sk;
    struct msghdr *msg = saved->msg;
    // 读完立即 delete，避免下次同 PID 的其他 syscall 误用旧数据
    bpf_map_delete_elem(&recvmsg_ctx_map, &pid);

    // 构造连接 Key 匹配之前 http_txn_stats 中的请求
    struct tcp_conn_key key = get_conn_key(sk);
    __u32 addr_sum = 0;
    #pragma unroll
    for (int i = 0; i < 4; i++) { addr_sum |= key.saddr[i] | key.daddr[i]; }
    if (addr_sum == 0) { dbg_inc(91); return 0; }

    // 查找等待响应的 HTTP 请求（is_request==1 表示只存了发送时间）
    struct http_txn_record *existing = bpf_map_lookup_elem(&http_txn_stats, &key);
    if (!existing) { dbg_inc(92); return 0; }
    if (existing->is_request == 0) { dbg_inc(93); return 0; }

    // 从用户态 msg->msg_iter.iov[0] 读取接收到的 HTTP 响应明文前 64 字节
    char http_buf[MAX_HTTP_PAYLOAD] = {};
    struct iovec *iov = BPF_CORE_READ(msg, msg_iter.iov);
    if (!iov) { dbg_inc(80); return 0; }
    void *data = BPF_CORE_READ(iov, iov_base);  // 用户态接收缓冲区起始地址
    if (!data) { dbg_inc(81); return 0; }
    // bpf_probe_read_user：安全读取用户态内存（这次是 recv 写入的那一端）
    if (bpf_probe_read_user(&http_buf, sizeof(http_buf), data) < 0) { dbg_inc(82); return 0; }
    dbg_inc(83);

    // 确认是 HTTP 响应（以 "HTTP/1." 开头）
    if (!check_http_response(http_buf)) { dbg_inc(84); return 0; }
    dbg_inc(85);

    __u64 recv_ns = bpf_ktime_get_ns();  // 响应首字节到达时间（TTFB 的终点）
    dbg_inc(86);

    __u16 status = extract_status_code(http_buf);  // 解析 200/404/500 等状态码
    dbg_inc(87);

    // 更新事务记录：补全响应端信息
    existing->recv_ns = recv_ns;          // 响应接收时间戳（TTFB = recv_ns - send_ns）
    existing->resp_bytes = (__u32)ret;    // tcp_recvmsg 返回值 = 实际接收字节数
    existing->status_code = status;        // HTTP 状态码
    existing->is_request = 0;              // 标记为"请求+响应完整"，供用户态识别
    dbg_inc(88);

    return 0;
}

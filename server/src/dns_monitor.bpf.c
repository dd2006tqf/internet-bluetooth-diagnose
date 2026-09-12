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

/*
 * Userspace event ABI.  The syscall tracepoints below are deliberately
 * separate from the legacy aggregate kprobes: the aggregate maps remain a
 * compatibility/observability path, while lifecycle truth comes only from
 * these payload-bearing events (SR-11).
 */
struct dns_event {
    __u8 direction;       /* 0=query, 1=response */
    __u8 rcode;
    __u8 tc;
    __u8 malformed;
    __u8 fingerprint_quality; /* 0=ENRICHED, 1=PARTIAL */
    __u8 reserved[3];
    __u32 client_ip;      /* network order; syscall path may be 0 */
    __u32 resolver_ip;    /* network order */
    __u16 client_port;    /* host order; 0 when unavailable */
    __u16 txid;
    __u16 qtype;
    __u16 payload_len;
    __u64 qname_hash;
    /* DNS 头之后的原始字节（含 QNAME），由**用户态**解析。
     * 不在 eBPF 内解析 QNAME：verifier 会拒绝变址栈访问与复杂控制流，
     * 且解析本属用户态职责（eBPF 只负责 capture 原始事实）。*/
    __u8  qname_area[24];
    __u64 timestamp_ns;
};

struct recv_pending {
    __u64 buffer;
    __u64 length;
    __u64 peer;
    __u64 peer_length;
    __u32 client_ip;
    __u32 resolver_ip;
    __u16 client_port;
    __s32 fd;
};

struct dns_iovec {
    __u64 base;
    __u64 len;
};

/* Linux user_msghdr layout on the supported ARM64 userspace ABI. */
struct dns_user_msghdr {
    __u64 name;
    __u32 namelen;
    __u32 _pad;
    __u64 iov;
    __u64 iovlen;
    __u64 control;
    __u64 controllen;
    __u32 flags;
    __u32 _pad2;
};

/* Kernel-independent sockaddr_in prefix (user memory supplied by the caller). */
struct dns_sockaddr_in {
    __u16 sin_family;
    __u16 sin_port;   /* network order */
    __u32 sin_addr;   /* network order */
};

/* DNS 端口判定（主机序与网络序兼容，见文件末尾同名函数的历史用法） */
static __always_inline bool is_dns_port(__u16 port)
{
    return port == DNS_PORT || port == bpf_htons(DNS_PORT);
}

/**
 * @brief 从 msghdr.name 解析对端 UDP 端口（网络序）。
 * @return 0 表示无法解析；否则返回 sin_port。
 *
 * 这是区分 DNS 与其他 UDP 流量的**唯一可靠依据**：仅靠 payload 长度与首字节
 * 会把任何 >=12 字节的 UDP（QUIC/STUN/NTP 等）误判成 DNS 查询，产生幽灵事务。
 */
static __always_inline __u16 msghdr_udp_port(const struct dns_user_msghdr *hdr, bool *ok)
{
    *ok = false;
    if (!hdr->name || hdr->namelen < sizeof(struct dns_sockaddr_in))
        return 0;
    struct dns_sockaddr_in sa = {};
    if (bpf_probe_read_user(&sa, sizeof(sa), (const void *)hdr->name) != 0)
        return 0;
    if (sa.sin_family != AF_INET)
        return 0;
    *ok = true;
    return sa.sin_port;
}



/*
 * Capture observability counters (per-CPU, one key per stat). These turn
 * "NO_DNS_EVENTS" into a located boundary: hook -> msghdr -> iovec -> header
 * -> emit, without guessing user ABI offsets (Evidence-first).
 */
enum dns_capture_stat {
    DNS_STAT_SENDTO_ENTER = 0,
    DNS_STAT_SENDMSG_ENTER,
    DNS_STAT_SENDMMSG_ENTER,
    DNS_STAT_RECVFROM_ENTER,
    DNS_STAT_RECVMSG_ENTER,
    DNS_STAT_RECVMSG_EXIT,
    DNS_STAT_RECVMMSG_EXIT,
    DNS_STAT_MSGHDR_READ_FAIL,
    DNS_STAT_IOVEC_READ_FAIL,
    DNS_STAT_PAYLOAD_READ_FAIL,
    DNS_STAT_SHORT_PAYLOAD,
    DNS_STAT_NOT_DNS_PORT,
    DNS_STAT_PORT_UNKNOWN,
    DNS_STAT_CONNECT_SEEN,
    DNS_STAT_UNSUPPORTED_ITER,
    DNS_STAT_SEND_ENTER,
    DNS_STAT_QUEUE_ENTER,
    DNS_STAT_SEND_DNS_PORT,
    DNS_STAT_SEND_NONIPV4,
    DNS_STAT_QUEUE_DNS_PORT,
    DNS_STAT_SEND_NON_DNS_PORT,
    DNS_STAT_SEND_HAS_MSG_NAME,
    DNS_STAT_SEND_CONNECTED_PEER,
    DNS_STAT_SEND_PAYLOAD_FAIL,
    DNS_STAT_SEND_EMITTED,
    DNS_STAT_QUEUE_NONIPV4,
    DNS_STAT_QUEUE_HEADER_FAIL,
    DNS_STAT_QUEUE_PAYLOAD_FAIL,
    DNS_STAT_QUEUE_NONDNS,
    DNS_STAT_QUEUE_EMITTED,
    DNS_STAT_EMITTED,
    DNS_STAT_EMIT_FAIL,
    DNS_STAT_MAX
};

struct fd_resolver_key {
    __u64 pid_tgid;
    __s32 fd;
    __u32 pad;
};

struct fd_resolver_value {
    __u32 resolver_ip;
    __u16 resolver_port;  /* network order */
    __u16 valid;
};

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, DNS_STAT_MAX);
    __type(key, __u32);
    __type(value, __u64);
} dns_capture_counters SEC(".maps");

static __always_inline void dns_stat_inc(__u32 key)
{
    __u64 *value = bpf_map_lookup_elem(&dns_capture_counters, &key);
    if (value)
        (*value)++;
}

struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 1024);
    __type(key, struct fd_resolver_key);
    __type(value, struct fd_resolver_value);
} fd_resolvers SEC(".maps");

/*
 * Connected UDP sockets carry no peer address in msghdr, so the destination port
 * cannot be recovered from sendmsg/recvmsg arguments alone. We therefore learn
 * the (pid, fd) → resolver binding from connect() and consult it at send time.
 * Without this the capture cannot tell DNS apart from other UDP traffic.
 */
static __always_inline bool lookup_fd_resolver(__u64 pid_tgid, __s32 fd, __u16 *port_out)
{
    struct fd_resolver_key key = {.pid_tgid = pid_tgid, .fd = fd, .pad = 0};
    struct fd_resolver_value *v = bpf_map_lookup_elem(&fd_resolvers, &key);
    if (!v || !v->valid)
        return false;
    *port_out = v->resolver_port;
    return true;
}

/**
 * @brief 登记/缓存 (pid, fd) → resolver 绑定。
 *
 * 已 connect 的 UDP socket 在 sendmsg/recvmsg 的 msghdr 中不带 name，
 * 因此只要在任一路径（connect 或带 name 的 sendmsg）学到绑定就缓存下来，
 * 供同 fd 的后续调用复用。否则"查得到 query 却查不到 response"会伪造超时。
 */
static __always_inline void remember_fd_resolver(__u64 pid_tgid, __s32 fd, __u16 port_nbo)
{
    struct fd_resolver_key key = {.pid_tgid = pid_tgid, .fd = fd, .pad = 0};
    struct fd_resolver_value val = {};
    val.resolver_port = port_nbo;
    val.valid = 1;
    bpf_map_update_elem(&fd_resolvers, &key, &val, BPF_ANY);
}



/*
 * ===========================================================================
 * 单一来源（single-source）DNS 捕获
 *
 * 架构原则（IR-DNS-5）：一个 DNS observation 的 endpoint 与 payload
 * 必须来自同一个 hook / 同一语义源，不允许依赖跨独立 probe 的时序 join。
 * 跨探针 join 需要猜测关联键（pid_tgid 等），在并发与交错场景下已被实机
 * 证明不可靠，并会留下陈旧条目污染后续事件。
 *
 * 本文件的三个单一来源点：
 *   - udp_sendmsg          : sk(sock) → endpoint, msg(msghdr) → payload   [query]
 *   - udp_queue_rcv_skb    : sk(sock) → endpoint, skb        → payload   [response]
 *   - udp_recvmsg(kret)    : sk(sock) → endpoint, msg_iter   → payload   [response, fallback]
 * 每个 hook 单独取齐两半后 emit，不产生任何 pending pair。
 * ===========================================================================
 */

/* iov_iter 类型最小判定所需（与 BTF 一致，仅用 CO-RE 读取，不硬编码整体偏移） */
#define DNS_ITER_IOVEC  0
#define DNS_ITER_KVEC   1

/*
 * 从内核 msghdr 的迭代器取前 12 字节 DNS 头。
 * 第一版只支持单段 ITER_IOVEC（用户态 sendmsg 的常见形态），
 * 其余类型计入 unsupported 并放弃，绝不猜测内存布局。
 */
static __always_inline bool read_dns_header_from_iter(const struct msghdr *msg, __u8 *header)
{
    if (!msg)
        return false;
    __u8 iter_type = BPF_CORE_READ(msg, msg_iter.iter_type);
    if (iter_type != DNS_ITER_IOVEC)
        return false;

    size_t iov_offset = BPF_CORE_READ(msg, msg_iter.iov_offset);
    const struct iovec *iov = BPF_CORE_READ(msg, msg_iter.iov);
    if (!iov)
        return false;

    // iovec 数组本身在**内核**内存：小 iovec 数时内核用栈上的 iovstack，
    // 大 iovec 数时是 kmalloc 的副本。只有 iov_base 指向用户数据。
    // 用 bpf_probe_read_user 读它必然失败（这正是此前 emitted 恒为 0 的原因）。
    struct iovec first = {};
    if (bpf_probe_read_kernel(&first, sizeof(first), (const void *)iov) != 0)
        return false;
    __u64 base = (__u64)first.iov_base;
    if (!base)
        return false;

    const void *cursor = (const void *)(base + iov_offset);
    return bpf_probe_read_user(header, 12, cursor) == 0;
}


/*
 * 从 sk_buff 取前 12 字节 DNS 头。
 * udp_queue_rcv_skb 时 skb->data 尚未被剥离 UDP 载荷之外的内容（队列态），
 * 这里按内核语义读取 data 指针起始处，并受 skb->len 约束。
 */
static __always_inline bool read_dns_header_from_skb(const struct sk_buff *skb, __u8 *header)
{
    if (!skb)
        return false;
    unsigned int len = BPF_CORE_READ(skb, len);
    if (len < 12)
        return false;
    unsigned char *data = BPF_CORE_READ(skb, data);
    if (!data)
        return false;

    // UDP 头可能已被 pull。用前 4 字节的源/目的端口是否存在 53 来判定：
    //   有 53 -> data 指向 UDP 头，DNS 在 data+8
    //   无 53 -> 头已被 pull，data 即 DNS 起始
    __u8 ports[4] = {};
    if (bpf_probe_read_kernel(ports, sizeof(ports), data) != 0)
        return false;
    __u16 p0 = ((__u16)ports[0] << 8) | ports[1];   // sport (网络序->大端值)
    __u16 p1 = ((__u16)ports[2] << 8) | ports[3];   // dport
    bool udp_hdr_present = (p0 == DNS_PORT) || (p1 == DNS_PORT);

    const void *cursor = udp_hdr_present ? (const void *)(data + 8) : (const void *)data;
    return bpf_probe_read_kernel(header, 12, cursor) == 0;
}


/*
 * QNAME 哈希（FNV-1a）。
 *
 * 为什么需要：判定"本机解析能力是否整体失效"时，必须区分
 *   - 单个域名的权威链路异常（可能导致该域名长时间无响应）
 *   - 解析器整体不响应（所有域名都失败）
 * 只有后者才够格否决 Internet Access。因此需要按 QNAME 区分失败来源。
 *
 * 只扫描 QNAME 所在区域，遇到 0x00（名字结束）即停。
 * 读不到则返回 0，表示"无法确认"，调用方不得据此授予能力级否决权。
 */
struct {
    __uint(type, BPF_MAP_TYPE_PERF_EVENT_ARRAY);
    __type(key, __u32);
    __type(value, __u32);
} dns_events SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 1024);
    __type(key, __u64);
    __type(value, struct recv_pending);
} pending_recv SEC(".maps");

char LICENSE[] SEC("license") = "GPL";

/* arm64 syscall numbers (asm-generic/unistd.h) */
#define DNS_NR_SENDTO      206
#define DNS_NR_RECVFROM    207
#define DNS_NR_SENDMSG     211
#define DNS_NR_RECVMSG     212
#define DNS_NR_SENDMMSG    213
#define DNS_NR_RECVMMSG    243
#define DNS_NR_CONNECT     203

static __always_inline __u64 event_pid_tgid(void)
{
    return bpf_get_current_pid_tgid();
}

static __always_inline int emit_dns_payload(void *ctx, __u8 direction,
                                             const void *buf, __u64 len,
                                             __u32 client_ip, __u32 resolver_ip,
                                             __u16 client_port)
{
    if (!buf || len < 12 || len > 4096) {
        dns_stat_inc(DNS_STAT_SHORT_PAYLOAD);
        return 0;
    }

    __u8 header[12] = {};
    if (bpf_probe_read_user(header, sizeof(header), buf) != 0) {
        dns_stat_inc(DNS_STAT_PAYLOAD_READ_FAIL);
        return 0;
    }

    struct dns_event event = {};
    event.direction = direction;
    event.client_ip = client_ip;
    event.resolver_ip = resolver_ip;
    event.client_port = client_port;
    event.txid = ((__u16)header[0] << 8) | header[1];
    event.tc = (header[2] & 0x02) != 0;
    event.rcode = header[3] & 0x0f;
    event.payload_len = len > 65535 ? 65535 : (__u16)len;
    event.timestamp_ns = bpf_ktime_get_ns();
    // 旧 syscall 路径不再向 dns_events 投递：单一来源原则下，
    // DNS observation 只能由 udp_sendmsg / udp_queue_rcv_skb 产生，
    // 否则两条路径并存会让 dns_events 混入不对称事件。
    (void)event;
    (void)client_ip;
    (void)resolver_ip;
    (void)client_port;
    return 0;
}

static __always_inline int emit_dns_msghdr(void *ctx, __u8 direction, __s32 fd,
                                            const void *msg, __u64 result_len,
                                            __u32 client_ip, __u32 resolver_ip,
                                            __u16 client_port)
{
    struct dns_user_msghdr hdr = {};
    struct dns_iovec iov = {};
    if (!msg || bpf_probe_read_user(&hdr, sizeof(hdr), msg) != 0) {
        dns_stat_inc(DNS_STAT_MSGHDR_READ_FAIL);
        return 0;
    }
    if (hdr.iov == 0 || hdr.iovlen == 0 || hdr.iovlen > 64) {
        dns_stat_inc(DNS_STAT_MSGHDR_READ_FAIL);
        return 0;
    }

    // 端口门控：只有确实指向/来自 53 的 UDP 流才是 DNS。
    // 缺少这一步会把任意 >=12 字节的 UDP（QUIC/STUN/NTP 等）当作 DNS 查询，
    // 产生永远等不到响应的幽灵事务，进而伪造超时并压低 DNS 健康度。
    bool port_ok = false;
    __u16 peer_port = msghdr_udp_port(&hdr, &port_ok);
    if (!port_ok) {
        // 已 connect 的 socket 在 msghdr 中不带 name：改用此前学到的绑定。
        port_ok = lookup_fd_resolver(event_pid_tgid(), fd, &peer_port);
        if (!port_ok) {
            dns_stat_inc(DNS_STAT_PORT_UNKNOWN);
            return 0;   // 无法确认是 DNS，宁可漏采也不伪造事务
        }
    } else if (is_dns_port(peer_port)) {
        // 本次带 name 且确实是 DNS：缓存绑定，供同 fd 后续无 name 的调用复用
        // （典型情形是 sendmsg 带 name、对应 recvmsg 不带）。
        remember_fd_resolver(event_pid_tgid(), fd, peer_port);
    }
    if (!is_dns_port(peer_port)) {
        dns_stat_inc(DNS_STAT_NOT_DNS_PORT);
        return 0;
    }

    if (bpf_probe_read_user(&iov, sizeof(iov), (const void *)hdr.iov) != 0) {
        dns_stat_inc(DNS_STAT_IOVEC_READ_FAIL);
        return 0;
    }
    __u64 len = result_len ? result_len : iov.len;
    if (len > iov.len) len = iov.len;
    return emit_dns_payload(ctx, direction, (const void *)iov.base, len,
                            client_ip, resolver_ip, client_port);
}

/*
 * 学习 (pid, fd) → resolver 绑定。已 connect 的 UDP socket 在后续
 * sendmsg/recvmsg 的 msghdr 中不再带对端地址，因此必须在此登记。
 */
SEC("tracepoint/raw_syscalls/sys_enter")
int trace_dns_enter_connect(struct trace_event_raw_sys_enter *ctx)
{
    if (ctx->id != DNS_NR_CONNECT) return 0;

    __s32 fd = (__s32)ctx->args[0];
    const void *addr = (const void *)ctx->args[1];
    if (!addr) return 0;

    struct dns_sockaddr_in sa = {};
    if (bpf_probe_read_user(&sa, sizeof(sa), addr) != 0)
        return 0;
    if (sa.sin_family != AF_INET)
        return 0;   // 仅跟踪 IPv4 UDP，与 Phase 2A 覆盖范围一致

    struct fd_resolver_key key = {.pid_tgid = bpf_get_current_pid_tgid(), .fd = fd, .pad = 0};
    struct fd_resolver_value val = {};
    val.resolver_ip = sa.sin_addr;
    val.resolver_port = sa.sin_port;
    val.valid = 1;
    bpf_map_update_elem(&fd_resolvers, &key, &val, BPF_ANY);
    dns_stat_inc(DNS_STAT_CONNECT_SEEN);
    return 0;
}


SEC("tracepoint/raw_syscalls/sys_enter")
int trace_dns_enter_sendmsg(struct trace_event_raw_sys_enter *ctx)
{
    if (ctx->id != DNS_NR_SENDMSG) return 0;
    dns_stat_inc(DNS_STAT_SENDMSG_ENTER);
    emit_dns_msghdr(ctx, 0, (__s32)ctx->args[0], (const void *)ctx->args[1], 0, 0, 0, 0);
    return 0;
}

SEC("tracepoint/raw_syscalls/sys_enter")
int trace_dns_enter_sendmmsg(struct trace_event_raw_sys_enter *ctx)
{
    if (ctx->id != DNS_NR_SENDMMSG) return 0;
    dns_stat_inc(DNS_STAT_SENDMMSG_ENTER);
    /* The first mmsghdr is layout-compatible with msghdr for its msg_hdr. */
    emit_dns_msghdr(ctx, 0, (__s32)ctx->args[0], (const void *)ctx->args[1], 0, 0, 0, 0);
    return 0;
}

SEC("tracepoint/raw_syscalls/sys_enter")
int trace_dns_enter_recvmsg(struct trace_event_raw_sys_enter *ctx)
{
    if (ctx->id != DNS_NR_RECVMSG) return 0;
    dns_stat_inc(DNS_STAT_RECVMSG_ENTER);
    __u64 pid = event_pid_tgid();
    struct recv_pending pending = {};
    pending.buffer = ctx->args[1];
    pending.length = 0;
    pending.fd = (__s32)ctx->args[0];
    bpf_map_update_elem(&pending_recv, &pid, &pending, BPF_ANY);
    return 0;
}

SEC("tracepoint/raw_syscalls/sys_exit")
int trace_dns_exit_recvmsg(struct trace_event_raw_sys_exit *ctx)
{
    if (ctx->id != DNS_NR_RECVMSG) return 0;
    dns_stat_inc(DNS_STAT_RECVMSG_EXIT);
    __u64 pid = event_pid_tgid();
    struct recv_pending *pending = bpf_map_lookup_elem(&pending_recv, &pid);
    if (pending && ctx->ret > 0)
        emit_dns_msghdr(ctx, 1, (__s32)pending->fd, (const void *)pending->buffer, (__u64)ctx->ret,
                        pending->client_ip, pending->resolver_ip, pending->client_port);
    bpf_map_delete_elem(&pending_recv, &pid);
    return 0;
}

SEC("tracepoint/raw_syscalls/sys_exit")
int trace_dns_exit_recvmmsg(struct trace_event_raw_sys_exit *ctx)
{
    if (ctx->id != DNS_NR_RECVMMSG) return 0;
    dns_stat_inc(DNS_STAT_RECVMMSG_EXIT);
    /* recvmmsg uses the same pending buffer ABI; keep a separate body so
     * libbpf does not emit a cross-section call relocation. */
    __u64 pid = event_pid_tgid();
    struct recv_pending *pending = bpf_map_lookup_elem(&pending_recv, &pid);
    if (pending && ctx->ret > 0)
        emit_dns_msghdr(ctx, 1, (__s32)pending->fd, (const void *)pending->buffer, (__u64)ctx->ret,
                        pending->client_ip, pending->resolver_ip, pending->client_port);
    bpf_map_delete_elem(&pending_recv, &pid);
    return 0;
}


SEC("tracepoint/raw_syscalls/sys_enter")
int trace_dns_enter_sendto(struct trace_event_raw_sys_enter *ctx)
{
    if (ctx->id != DNS_NR_SENDTO) return 0;
    int fd = (int)ctx->args[0];
    const void *buf = (const void *)ctx->args[1];
    __u64 len = ctx->args[2];
    const void *addr = (const void *)ctx->args[4];
    __u32 resolver_ip = 0;
    __u16 resolver_port = 0;
    if (addr) {
        bpf_probe_read_user(&resolver_port, sizeof(resolver_port), addr + 2);
        bpf_probe_read_user(&resolver_ip, sizeof(resolver_ip), addr + 4);
    }
    if (resolver_port != bpf_htons(DNS_PORT) && resolver_port != DNS_PORT)
        return 0;

    dns_stat_inc(DNS_STAT_SENDTO_ENTER);

    // 单一来源：query 事件由 udp_sendmsg 产生，此处不投递。
    (void)fd; (void)buf; (void)len; (void)resolver_ip;
    return 0;
}

SEC("tracepoint/raw_syscalls/sys_enter")
int trace_dns_enter_recvfrom(struct trace_event_raw_sys_enter *ctx)
{
    if (ctx->id != DNS_NR_RECVFROM) return 0;
    int fd = (int)ctx->args[0];
    const void *buf = (const void *)ctx->args[1];
    __u64 len = ctx->args[2];
    const void *addr = (const void *)ctx->args[4];
    struct recv_pending pending = {};
    pending.buffer = (__u64)buf;
    pending.length = len;
    pending.fd = (__s32)fd;
    pending.peer = (__u64)addr;
    pending.peer_length = ctx->args[5];
    __u64 pid = event_pid_tgid();
    (void)fd;
    bpf_map_update_elem(&pending_recv, &pid, &pending, BPF_ANY);
    return 0;
}

SEC("tracepoint/raw_syscalls/sys_exit")
int trace_dns_exit_recvfrom(struct trace_event_raw_sys_exit *ctx)
{
    __u64 pid = event_pid_tgid();
    struct recv_pending *pending = bpf_map_lookup_elem(&pending_recv, &pid);
    if (!pending) return 0;
    // 单一来源：response 事件由 udp_queue_rcv_skb 产生，此处不投递。
    bpf_map_delete_elem(&pending_recv, &pid);
    return 0;
}

/*
 * Periodically dump per-CPU capture counters into the perf channel so
 * userspace can log the exact boundary where DNS evidence stops.
 */
SEC("tracepoint/syscalls/sys_enter_getpid")
int trace_dns_counter_probe(struct trace_event_raw_sys_enter *ctx)
{
    return 0;
}


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
 * 挂点: kprobe/udp_sendmsg（单一来源：sk→endpoint, msg_iter→payload）
 */
/*
 * 函数: trace_dns_egress_skb
 * 挂点: kprobe/ip_finish_output2(struct net *net, struct sock *sk, struct sk_buff *skb)
 *
 * 为什么不在 udp_sendmsg / udp_send_skb 取 query endpoint：
 *   - udp_sendmsg **入口**：未 connect 的 socket 尚未 autobind，
 *     skc_num / skc_rcv_saddr 仍为 0，get 不到真实 local endpoint。
 *   - udp_send_skb **入口**：UDP 头尚未写入（该函数自己才写），
 *     transport_header 不可用。
 *   - ip_finish_output2：UDP 与 IP 头均已构造完毕、地址已定型，
 *     endpoint 与 payload 可全部从**同一个报文**读出，与接收侧完全对称。
 *
 * endpoint 与 payload 同源（该 skb），无跨探针 join。
 */
SEC("kprobe/ip_finish_output2")
int trace_dns_egress_skb(struct pt_regs *ctx)
{
    dns_stat_inc(DNS_STAT_SEND_ENTER);
    struct sk_buff *skb = (struct sk_buff *)PT_REGS_PARM3(ctx);
    if (!skb)
        return 0;

    unsigned char *head = BPF_CORE_READ(skb, head);
    if (!head)
        return 0;

    __u16 transport_off = BPF_CORE_READ(skb, transport_header);
    __u16 net_off = BPF_CORE_READ(skb, network_header);

    __u8 ports[4] = {};
    if (bpf_probe_read_kernel(ports, sizeof(ports), (const void *)(head + transport_off)) != 0) {
        dns_stat_inc(DNS_STAT_QUEUE_HEADER_FAIL);
        return 0;
    }
    __u16 sport = ((__u16)ports[0] << 8) | ports[1];   // 客户端端口（autobind 后）
    __u16 dport = ((__u16)ports[2] << 8) | ports[3];   // 目的端口，应为 53
    if (dport != DNS_PORT) {
        dns_stat_inc(DNS_STAT_SEND_NON_DNS_PORT);
        return 0;
    }
    dns_stat_inc(DNS_STAT_SEND_DNS_PORT);

    __u8 iphdr[20] = {};
    if (bpf_probe_read_kernel(iphdr, sizeof(iphdr), (const void *)(head + net_off)) != 0) {
        dns_stat_inc(DNS_STAT_QUEUE_HEADER_FAIL);
        return 0;
    }
    __u32 saddr = 0, daddr = 0;
    __builtin_memcpy(&saddr, iphdr + 12, 4);   // 客户端源地址（已定型）
    __builtin_memcpy(&daddr, iphdr + 16, 4);   // DNS 服务器

    __u8 header[12] = {};
    if (bpf_probe_read_kernel(header, sizeof(header),
                              (const void *)(head + transport_off + 8)) != 0) {
        dns_stat_inc(DNS_STAT_SEND_PAYLOAD_FAIL);
        return 0;
    }

    struct dns_event ev = {};
    ev.direction = 0;
    // 一次固定大小读（无循环、无常量外索引），verifier 可静态证明安全
    bpf_probe_read_kernel(ev.qname_area, sizeof(ev.qname_area),
                          (const void *)(head + transport_off + 8 + 12));
    ev.client_ip = saddr;        // 与接收侧 daddr 同域（原始 NBO 字节）
    ev.resolver_ip = daddr;
    ev.client_port = sport;      // 与接收侧 dport 同域（大端组装）
    ev.txid = ((__u16)header[0] << 8) | header[1];
    ev.tc = (header[2] & 0x02) != 0;
    ev.rcode = header[3] & 0x0f;
    ev.timestamp_ns = bpf_ktime_get_ns();
    ev.fingerprint_quality = 0;
    bpf_perf_event_output(ctx, &dns_events, BPF_F_CURRENT_CPU, &ev, sizeof(ev));
    dns_stat_inc(DNS_STAT_SEND_EMITTED);
    dns_stat_inc(DNS_STAT_EMITTED);
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

/*
 * ===========================================================================
 * BPF 入口函数 3: DNS 响应接收（单一来源）
 *
 * 挂点选择：udp_queue_rcv_skb(struct sock *sk, struct sk_buff *skb)
 *   - sk  → endpoint（client_pc / resolver）
 *   - skb → payload（DNS 头）
 * 两半来自同一 hook 的同一次调用，无需任何跨探针关联或时序假设。
 *
 * 选择它而不是 udp_recvmsg 的原因：udp_recvmsg 在 entry 时用户缓冲区尚未
 * 写入数据，需要 entry/return 配对保存指针，属于跨阶段脆弱状态；而队列态
 * 的 skb 已经持有完整载荷。
 * ===========================================================================
 */
SEC("kprobe/udp_queue_rcv_skb")
int trace_dns_queue_rcv(struct pt_regs *ctx)
{
    dns_stat_inc(DNS_STAT_QUEUE_ENTER);
    struct sock *sk = (struct sock *)PT_REGS_PARM1(ctx);
    struct sk_buff *skb = (struct sk_buff *)PT_REGS_PARM2(ctx);
    if (!sk || !skb)
        return 0;

    __u16 family = BPF_CORE_READ(sk, __sk_common.skc_family);
    if (family != AF_INET) {
        dns_stat_inc(DNS_STAT_QUEUE_NONIPV4);
        return 0;
    }

    // 用 skb->head + transport_header 定位 UDP 头。
    // 不用 skb->data：skb_pull 会移动 data，其含义取决于挂载点；head 是稳定基址，
    // transport_header 是相对 head 的偏移，这是内核自身的定位方式。
    unsigned char *head = BPF_CORE_READ(skb, head);
    __u16 transport_off = BPF_CORE_READ(skb, transport_header);
    if (!head)
        return 0;

    const void *udp_hdr = (const void *)(head + transport_off);
    __u8 ports[4] = {};
    if (bpf_probe_read_kernel(ports, sizeof(ports), udp_hdr) != 0) {
        dns_stat_inc(DNS_STAT_QUEUE_HEADER_FAIL);
        return 0;
    }
    // 大端组装 = 数值化的端口（与 query 侧的主机序语义一致）
    __u16 sport = ((__u16)ports[0] << 8) | ports[1];
    __u16 dport = ((__u16)ports[2] << 8) | ports[3];

    // 响应方向：packet 源端口才是 DNS server 的 53，目的端口是客户端随机端口。
    // （query 侧相反：目的端口是 53。）
    if (sport != DNS_PORT) {
        dns_stat_inc(DNS_STAT_QUEUE_NONDNS);
        return 0;
    }
    dns_stat_inc(DNS_STAT_QUEUE_DNS_PORT);

    // 网络层地址：同样从 packet 取，不依赖 sock 的 remote peer
    // （未 connect 的 UDP socket 不保证有固定 remote）
    __u16 net_off = BPF_CORE_READ(skb, network_header);
    __u8 iphdr[20] = {};
    if (bpf_probe_read_kernel(iphdr, sizeof(iphdr), (const void *)(head + net_off)) != 0) {
        dns_stat_inc(DNS_STAT_QUEUE_HEADER_FAIL);
        return 0;
    }
    // 直接按 4 字节原样读出（网络序字节的小端加载），与 query 侧
    // msg_name.sin_addr / skc_daddr 的字节序保持一致；
    // 用户态统一做 ntohl()，两侧必须同域，否则 canonical key 无法相等。
    __u32 saddr = 0, daddr = 0;
    __builtin_memcpy(&saddr, iphdr + 12, 4);   // DNS server
    __builtin_memcpy(&daddr, iphdr + 16, 4);   // client

    __u8 header[12] = {};
    if (bpf_probe_read_kernel(header, sizeof(header),
                              (const void *)(head + transport_off + 8)) != 0) {
        dns_stat_inc(DNS_STAT_QUEUE_PAYLOAD_FAIL);
        return 0;
    }

    struct dns_event ev = {};
    ev.direction = 1;
    bpf_probe_read_kernel(ev.qname_area, sizeof(ev.qname_area),
                          (const void *)(head + transport_off + 8 + 12));
    // canonical（客户端视角）：client = packet 目的端，resolver = packet 源端
    ev.client_ip = daddr;
    ev.resolver_ip = saddr;
    ev.client_port = dport;   /* 大端组装=主机序数值，与 query 侧 skc_num 同域 */
    ev.txid = ((__u16)header[0] << 8) | header[1];
    ev.tc = (header[2] & 0x02) != 0;
    ev.rcode = header[3] & 0x0f;
    ev.timestamp_ns = bpf_ktime_get_ns();
    ev.fingerprint_quality = 0;
    bpf_perf_event_output(ctx, &dns_events, BPF_F_CURRENT_CPU, &ev, sizeof(ev));
    dns_stat_inc(DNS_STAT_QUEUE_EMITTED);
    return 0;
}

// http_latency.bpf.c
// HTTP 请求级延迟追踪 eBPF 程序
// 挂载 kprobe/tcp_sendmsg 和 kprobe/tcp_recvmsg，从 sk_buff 提取 HTTP 首部
// 计算 TTFB（首字节延迟），区分应用慢 vs 网络慢

#define __TARGET_ARCH_arm64
#define AF_INET 2
#define MAX_HTTP_PAYLOAD 64  // 只读取前 64 字节用于 HTTP 识别

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_endian.h>

char LICENSE[] SEC("license") = "GPL";

// ---- 数据结构 ----

// TCP 连接标识
struct tcp_conn_key {
    __u32 saddr;
    __u32 daddr;
    __u16 sport;
    __u16 dport;
};

struct http_txn_key {
    __u32 saddr;
    __u32 daddr;
    __u16 sport;
    __u16 dport;
};

struct http_txn_record {
    __u64 send_ns;
    __u64 recv_ns;
    __u32 req_bytes;
    __u32 resp_bytes;
    __u16 status_code;
    __u8  is_request;     // 1=请求已发送，0=响应已接收
    __u8  padding;
};

// ---- BPF Map ----

struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 8192);
    __type(key, struct http_txn_key);
    __type(value, struct http_txn_record);
} http_txn_stats SEC(".maps");

// entry kprobe 保存的 sk + msg 指针（key = PID），retprobe 读取
struct recvmsg_ctx {
    struct sock *sk;
    struct msghdr *msg;
};
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 1024);
    __type(key, __u32);                // PID
    __type(value, struct recvmsg_ctx); // sk + msg 指针
} recvmsg_ctx_map SEC(".maps");

// ---- debug map ----
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 64);
    __type(key, __u32);
    __type(value, __u64);
} http_debug SEC(".maps");

static __always_inline void dbg_inc(__u32 idx) {
    __u32 k = idx;
    __u64 *v = bpf_map_lookup_elem(&http_debug, &k);
    if (v) __sync_fetch_and_add(v, 1);
    else { __u64 one = 1; bpf_map_update_elem(&http_debug, &k, &one, BPF_ANY); }
}

// ---- HTTP 检测辅助 ----

static __always_inline int check_http_request(void *data_start)
{
    char prefix[8] = {};
    if (bpf_probe_read_kernel(prefix, sizeof(prefix), data_start) < 0)
        return 0;
    if (prefix[0]=='G' && prefix[1]=='E' && prefix[2]=='T' && prefix[3]==' ')
        return 1;
    if (prefix[0]=='P' && prefix[1]=='O' && prefix[2]=='S' && prefix[3]=='T')
        return 1;
    if (prefix[0]=='P' && prefix[1]=='U' && prefix[2]=='T' && prefix[3]==' ')
        return 1;
    if (prefix[0]=='D' && prefix[1]=='E' && prefix[2]=='L' && prefix[3]=='E')
        return 1;
    if (prefix[0]=='H' && prefix[1]=='E' && prefix[2]=='A' && prefix[3]=='D')
        return 1;
    return 0;
}

static __always_inline int check_http_response(void *data_start)
{
    char prefix[12] = {};
    if (bpf_probe_read_kernel(prefix, sizeof(prefix), data_start) < 0)
        return 0;
    if (prefix[0]=='H' && prefix[1]=='T' && prefix[2]=='T' && prefix[3]=='P' &&
        prefix[4]=='/' && prefix[5]=='1')
        return 1;
    return 0;
}

static __always_inline __u16 extract_status_code(void *data_start)
{
    // 格式: "HTTP/1.0 200 OK\r\n" 或 "HTTP/1.1 404 Not Found\r\n"
    char buf[16] = {};
    if (bpf_probe_read_kernel(buf, sizeof(buf), data_start + 9) < 0)
        return 0;
    __u16 code = 0;
    if (buf[0] >= '1' && buf[0] <= '9' && buf[1] >= '0' && buf[1] <= '9' && buf[2] >= '0' && buf[2] <= '9')
        code = (buf[0]-'0')*100 + (buf[1]-'0')*10 + (buf[2]-'0');
    return code;
}

static __always_inline struct tcp_conn_key get_conn_key(struct sock *sk)
{
    struct tcp_conn_key k = {};
    __u16 family = BPF_CORE_READ(sk, __sk_common.skc_family);
    if (family != AF_INET) return k;
    k.saddr = BPF_CORE_READ(sk, __sk_common.skc_rcv_saddr);
    k.daddr = BPF_CORE_READ(sk, __sk_common.skc_daddr);
    __u16 sport_host = BPF_CORE_READ(sk, __sk_common.skc_num);
    k.sport = bpf_htons(sport_host);
    k.dport = BPF_CORE_READ(sk, __sk_common.skc_dport);
    return k;
}

// ---- 挂点 ----

// tcp_sendmsg 是应用层明文入口，msg_iter 直接指向用户态待发 HTTP 明文。
// 关键点（文档 3.2）：
//   - msg_iter.iov 可能有多段（count>1），HTTP 首部可能跨段
//   - 必须按 iov_base/iov_len 逐段 bpf_probe_read_user，拼接前 N 字节到 http_buf
//   - 只处理 msg_iter 指向的用户态 iovec
// dev_queue_xmit 已实测走不通：skb->len 含非线性 fragment(data_len)，明文 payload
// 不在线性区 head+offset，继续细化偏移无意义。（见 ebpf-http-debug记录.md 第四版）

// 从用户态 msg->msg_iter 读取明文前 buf_sz 字节。为满足 eBPF 验证器对固定偏移的要求，
// 仅读取**首段** iov（curl/常见 HTTP 客户端 send 多为单段，首段即含请求首部）。
// 返回填充字节数。若后续需要多段拼接，需改用固定 nr=1 的循环边界或 bpf_dynptr。
static __always_inline int read_http_user(void *http_buf, __u32 buf_sz,
                                          struct msghdr *msg)
{
    struct iovec *iov = (struct iovec *)BPF_CORE_READ(msg, msg_iter.iov);
    if (!iov) { dbg_inc(72); return 0; }
    __u32 nr = BPF_CORE_READ(msg, msg_iter.count);
    if (nr == 0) { dbg_inc(73); return 0; }
    dbg_inc(74);  // iov+count OK

    void *base = BPF_CORE_READ(iov, iov_base);
    __u64 len = BPF_CORE_READ(iov, iov_len);
    if (!base || len == 0) { dbg_inc(77); return 0; }
    __u32 take = buf_sz;
    if (len < take) take = (__u32)len;
    if (bpf_probe_read_user(http_buf, take, base) != 0) { dbg_inc(75); return 0; }
    dbg_inc(76);  // 首段读成功
    return (int)take;
}

// tcp_sendmsg: int tcp_sendmsg(struct sock *sk, struct msghdr *msg, size_t size)
SEC("kprobe/tcp_sendmsg")
int BPF_KPROBE(probe_http_req, struct sock *sk, struct msghdr *msg, size_t size)
{
    if (!sk || !msg) return 0;
    dbg_inc(0);

    // 仅处理应用层有实际数据发送的情况
    if (size <= 0) return 0;

    char http_buf[MAX_HTTP_PAYLOAD] = {};
    int n = read_http_user(http_buf, sizeof(http_buf), msg);
    if (n <= 0) { dbg_inc(2); return 0; }
    dbg_inc(3);
    if (!check_http_request(http_buf)) { dbg_inc(4); return 0; }
    dbg_inc(5);

    struct tcp_conn_key key = get_conn_key(sk);
    if (key.saddr == 0 && key.daddr == 0) { dbg_inc(6); return 0; }

    struct http_txn_record rec = {};
    rec.send_ns = bpf_ktime_get_ns();
    rec.req_bytes = (__u32)size;
    rec.is_request = 1;
    bpf_map_update_elem(&http_txn_stats, &key, &rec, BPF_ANY);
    return 0;
}

// entry kprobe：保存 sk + msg 到 map，供 retprobe 读取
// kretprobe 触发时 PT_REGS_PARM1 是返回值不是入口参数，必须用 entry→return 配对
SEC("kprobe/tcp_recvmsg_locked")
int BPF_KPROBE(trace_recvmsg_entry, struct sock *sk, struct msghdr *msg, size_t len, int flags)
{
    if (!sk || !msg) return 0;
    __u32 pid = (__u32)(bpf_get_current_pid_tgid() >> 32);
    struct recvmsg_ctx saved = { .sk = sk, .msg = msg };
    bpf_map_update_elem(&recvmsg_ctx_map, &pid, &saved, BPF_ANY);
    return 0;
}

// kretprobe：读取 entry 保存的 sk + msg，用 sk 构造 key 匹配请求
SEC("kretprobe/tcp_recvmsg_locked")
int BPF_KPROBE(trace_recvmsg_return, long ret)
{
    dbg_inc(90);

    // 从 map 读取 entry probe 保存的 sk + msg
    __u32 pid = (__u32)(bpf_get_current_pid_tgid() >> 32);
    struct recvmsg_ctx *saved = bpf_map_lookup_elem(&recvmsg_ctx_map, &pid);
    if (!saved || !saved->sk || !saved->msg) { dbg_inc(91); return 0; }
    struct sock *sk = saved->sk;
    struct msghdr *msg = saved->msg;
    bpf_map_delete_elem(&recvmsg_ctx_map, &pid);

    struct tcp_conn_key key = get_conn_key(sk);
    if (key.saddr == 0 && key.daddr == 0) { dbg_inc(91); return 0; }

    // 查找是否有对应的请求在等待响应
    struct http_txn_record *existing = bpf_map_lookup_elem(&http_txn_stats, &key);
    if (!existing) { dbg_inc(92); return 0; }
    if (existing->is_request == 0) { dbg_inc(93); return 0; }

    // 尝试读取接收数据前缀，检查是否是 HTTP 响应
    // 用 bpf_probe_read_user 读用户态 msg->msg_iter.iov[0].iov_base
    char http_buf[MAX_HTTP_PAYLOAD] = {};
    struct iovec *iov = BPF_CORE_READ(msg, msg_iter.iov);
    if (!iov) { dbg_inc(80); return 0; }
    void *data = BPF_CORE_READ(iov, iov_base);
    if (!data) { dbg_inc(81); return 0; }
    if (bpf_probe_read_user(&http_buf, sizeof(http_buf), data) < 0) { dbg_inc(82); return 0; }
    dbg_inc(83);  // 读响应明文成功

    if (!check_http_response(http_buf)) { dbg_inc(84); return 0; }
    dbg_inc(85);  // 识别为 HTTP 响应
    __u64 recv_ns = bpf_ktime_get_ns();
    dbg_inc(86);  // 走到计算
    __u16 status = extract_status_code(http_buf);
    dbg_inc(87);  // 状态码提取完成

    // 更新记录：设置响应接收时间和状态码
    existing->recv_ns = recv_ns;
    existing->resp_bytes = (__u32)ret;
    existing->status_code = status;
    existing->is_request = 0;  // 标记为响应已接收
    dbg_inc(88);  // 完整更新提交

    return 0;
}

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

// kprobe: int tcp_sendmsg(struct sock *sk, struct msghdr *msg, size_t size)
SEC("kprobe/tcp_sendmsg")
int BPF_KPROBE(trace_tcp_sendmsg, struct sock *sk, struct msghdr *msg, size_t size)
{
    if (!sk || !msg || size < 10) return 0;

    struct tcp_conn_key key = get_conn_key(sk);
    if (key.saddr == 0 && key.daddr == 0) return 0;

    // 尝试从 msg 中读取数据前缀，检查是否是 HTTP 请求
    // msg->msg_iter.iov 的数据地址在内核内存中
    // 简化：直接用 probe_read 从 msg->msg_iter 开始读取
    char http_buf[MAX_HTTP_PAYLOAD] = {};
    // msg_iter 在偏移量 16 处是迭代器，第一个 iov_base
    if (bpf_probe_read_kernel(&http_buf, sizeof(http_buf), (void *)msg + 16) < 0)
        return 0;

    if (!check_http_request(http_buf))
        return 0;

    // 是 HTTP 请求，记录发送时间
    struct http_txn_record rec = {};
    rec.send_ns = bpf_ktime_get_ns();
    rec.req_bytes = (__u32)size;
    rec.is_request = 1;
    bpf_map_update_elem(&http_txn_stats, &key, &rec, BPF_ANY);

    return 0;
}

// kprobe: int tcp_recvmsg(struct sock *sk, struct msghdr *msg, size_t len, int flags, int *addr_len)
SEC("kprobe/tcp_recvmsg")
int BPF_KPROBE(trace_tcp_recvmsg, struct sock *sk, struct msghdr *msg, size_t len, int flags)
{
    if (!sk || !msg) return 0;

    struct tcp_conn_key key = get_conn_key(sk);
    if (key.saddr == 0 && key.daddr == 0) return 0;

    // 查找是否有对应的请求在等待响应
    struct http_txn_record *existing = bpf_map_lookup_elem(&http_txn_stats, &key);
    if (!existing || existing->is_request == 0)
        return 0;  // 没有对应的请求，或已经是响应状态

    // 尝试读取接收数据前缀，检查是否是 HTTP 响应
    char http_buf[MAX_HTTP_PAYLOAD] = {};
    if (bpf_probe_read_kernel(&http_buf, sizeof(http_buf), (void *)msg + 16) < 0)
        return 0;

    if (!check_http_response(http_buf))
        return 0;

    // 计算 TTFB
    __u64 recv_ns = bpf_ktime_get_ns();
    __u16 status = extract_status_code(http_buf);

    // 更新记录：设置响应接收时间和状态码
    existing->recv_ns = recv_ns;
    existing->resp_bytes = (__u32)len;
    existing->status_code = status;
    existing->is_request = 0;  // 标记为响应已接收

    return 0;
}

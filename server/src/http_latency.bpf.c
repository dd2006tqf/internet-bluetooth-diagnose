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

// 从出站 skb 读取 HTTP 明文。
// 用 skb->network_header 定位网络头部起点（IP 头），再跳过 IP 头 + TCP 头读 payload。
static __always_inline int read_http_from_skb(struct sk_buff *skb, char *http_buf, __u32 buf_sz)
{
    unsigned char *head = BPF_CORE_READ(skb, head);
    __u16 nh_off = BPF_CORE_READ(skb, network_header);
    __u32 total_len = BPF_CORE_READ(skb, len);
    if (!head) { dbg_inc(21); return 0; }
    if (!nh_off) { dbg_inc(22); return 0; }
    if (total_len < 40) { dbg_inc(23); return 0; }
    dbg_inc(24);  // head/nh_off/len OK

    // IP 头起点 = head + network_header
    unsigned char *ip_start = head + nh_off;

    // IPv4: 首字节 = version(4bit) | IHL(4bit)
    __u8 version_ihl;
    if (bpf_probe_read_kernel(&version_ihl, 1, ip_start) < 0) { dbg_inc(25); return 0; }
    __u8 version = version_ihl >> 4;
    if (version != 4) { dbg_inc(26); return 0; }  // 非 IPv4
    __u8 ihl = version_ihl & 0x0F;
    if (ihl < 5) { dbg_inc(27); return 0; }
    __u16 ip_hdr_len = ihl * 4;
    dbg_inc(28);  // IPv4 ihl OK

    // TCP 头: data_off 在 TCP 头偏移 12（低 nibble）
    __u8 tcp_off;
    if (bpf_probe_read_kernel(&tcp_off, 1, ip_start + ip_hdr_len + 12) < 0) { dbg_inc(29); return 0; }
    __u16 tcp_hdr_len = (tcp_off >> 4) * 4;
    if (tcp_hdr_len < 20) { dbg_inc(30); return 0; }
    dbg_inc(31);  // tcp hdr OK

    __u32 payload_off = ip_hdr_len + tcp_hdr_len;
    if (payload_off + buf_sz > total_len) { dbg_inc(32); return 0; }
    if (bpf_probe_read_kernel(http_buf, buf_sz, ip_start + payload_off) < 0) { dbg_inc(33); return 0; }
    dbg_inc(34);  // payload 读取成功

    // 取样：把 payload 前 4 字节写成 uint32 存 debug map key 40
    {
        __u32 sample = ((__u32)(__u8)http_buf[0]) |
                       ((__u32)(__u8)http_buf[1] << 8) |
                       ((__u32)(__u8)http_buf[2] << 16) |
                       ((__u32)(__u8)http_buf[3] << 24);
        __u32 k40 = 40;
        bpf_map_update_elem(&http_debug, &k40, &sample, BPF_ANY);
    }
    return 1;
}

// kprobe: int dev_queue_xmit(struct sk_buff *skb)  // 出站，PT_REGS_PARM1 = skb
SEC("kprobe/dev_queue_xmit")
int BPF_KPROBE(probe_http_req, struct sk_buff *skb)
{
    if (!skb) return 0;
    dbg_inc(0);

    struct tcp_conn_key key = {};
    __u32 len = BPF_CORE_READ(skb, len);

    char http_buf[MAX_HTTP_PAYLOAD] = {};
    if (!read_http_from_skb(skb, http_buf, sizeof(http_buf))) {
        dbg_inc(2); return 0;
    }
    dbg_inc(3);
    if (!check_http_request(http_buf)) {
        dbg_inc(4); return 0;
    }
    dbg_inc(5);

    // 用 5 元组（从 skb 的 sock/头提取）作为 key。dev_queue_xmit 只有 skb，从 skb->sk 拿 sock
    struct sock *sk = BPF_CORE_READ(skb, sk);
    if (sk) key = get_conn_key(sk);
    if (key.saddr == 0 && key.daddr == 0) {
        dbg_inc(6); return 0;
    }

    struct http_txn_record rec = {};
    rec.send_ns = bpf_ktime_get_ns();
    rec.req_bytes = len;
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
    // 用 bpf_probe_read_user 读用户态 msg->msg_iter.iov[0].iov_base
    char http_buf[MAX_HTTP_PAYLOAD] = {};
    struct iovec *iov = BPF_CORE_READ(msg, msg_iter.iov);
    if (!iov) return 0;
    void *data = BPF_CORE_READ(iov, iov_base);
    if (!data) return 0;
    if (bpf_probe_read_user(&http_buf, sizeof(http_buf), data) < 0)
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

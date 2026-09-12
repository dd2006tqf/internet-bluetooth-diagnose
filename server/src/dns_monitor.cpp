/**
 * @file dns_monitor.cpp
 * @brief DNS 查询延迟监控器 - 用户态实现
 *
 * 监控指标：
 *   - DNS 平均解析延迟（avgLatencyMs）
 *   - DNS 最大解析延迟（maxLatencyMs）
 *   - DNS 超时次数（totalTimeouts）
 *   - DNS 总查询数 / 总响应数
 *   - DNS 超时率：timeouts / (queries + responses)
 *
 * 数据源（eBPF）：
 *   - BPF 对象文件名：dns_monitor.bpf.o
 *   - 探针类型：kprobe（内核函数入口探针）
 *     - kprobe/udp_sendmsg  → trace_dns_send（捕获 DNS 查询包发送时间戳）
 *     - kprobe/udp_recvmsg  → trace_dns_recv（捕获 DNS 响应包接收时间戳）
 *   - 数据通道：BPF Map（数组类型 / Per-CPU）
 *     - dns_queries：查询记录 Map（键 = 四元组 saddr/daddr/sport，值 = send_time_ns 等）
 *     - dns_stats ：聚合统计 Map（键 = 0，值 = total_queries/latency_ns 等）
 *   - 用户态通过 bpf_map_lookup_elem 从 dns_stats Map 读取聚合统计
 *
 * 线程模型：
 *   - 本类本身不创建独立线程，由外部（如 NetworkQualityAssessor）周期性调用 getStats()
 *   - 无锁：eBPF Map 的读取本身对并发安全，内部状态通过 EbpfMonitorStateSupport 封装
 */

#include "dns_monitor.hpp"
#include "logger.hpp"

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <vector>
#include <iomanip>
#include <sstream>
#include <arpa/inet.h>
#include <atomic>

#if defined(__has_include)
#  if __has_include(<linux/bpf.h>) && __has_include(<bpf/libbpf.h>) && __has_include(<bpf/bpf.h>)
#    define HAVE_LIBBPF 1
extern "C" {
#include <linux/bpf.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
}
#  else
#    define HAVE_LIBBPF 0
#  endif
#else
#  define HAVE_LIBBPF 0
#endif

namespace weaknet_dbus {

// 配对诊断输出条数上限（避免污染日志）
static constexpr int kDnsKeyDumpBudget = 12;

// ---- 数据结构映射（与 BPF 端 C 结构体一一对应，必须保持字段和 __packed 一致） ----

/**
 * @brief DNS 查询记录的键结构（与 BPF 端 dns_queries Map 的 key 一致）
 *
 * 用源 IP + 目的 IP + 源端口（DNS 查询的唯一标识）作为键，
 * 目的端口固定为 53（DNS 服务端口），因此不纳入键
 */
struct dns_query_key {
    __u32 saddr;       // 源 IP（网络字节序）
    __u32 daddr;       // 目的 IP（网络字节序）
    __u16 sport;       // 源端口（DNS 查询的临时端口）
} __attribute__((packed));

/**
 * @brief DNS 查询/响应记录（与 BPF 端 dns_queries Map 的 value 一致）
 */
struct dns_query_record {
    __u64 send_time_ns;   // DNS 查询发送时间戳（纳秒，CLOCK_MONOTONIC）
    __u64 recv_time_ns;   // DNS 响应接收时间戳（纳秒，0 表示超时未收到）
    __u32 reply_len;      // DNS 响应报文长度
    __u8  rcode;          // DNS 响应码（0=NOERROR, 2=SERVFAIL, 3=NXDOMAIN 等）
    __u8  is_response;    // 0=查询记录，1=响应记录（用于区分 send/recv 两个探针写入）
};

/**
 * @brief DNS 聚合统计记录（与 BPF 端 dns_stats Map 的 value 一致）
 *
 * BPF 程序在每次 DNS 事务完成时（收到响应或超时）更新此 Map
 */
struct dns_stats_record {
    __u64 total_queries;      // 总查询数
    __u64 total_responses;   // 总响应数（有回复的查询）
    __u64 total_timeouts;     // 总超时数（未收到响应的查询）
    __u64 total_errors;       // 总错误数（rcode != 0 的响应）
    __u64 total_latency_ns;   // 累计解析延迟（纳秒），用于计算平均延迟
    __u64 max_latency_ns;    // 历史最大解析延迟（纳秒）
};

struct dns_event {
    __u8 direction;
    __u8 rcode;
    __u8 tc;
    __u8 malformed;
    __u8 fingerprint_quality;
    __u8 reserved[3];
    __u32 client_ip;
    __u32 resolver_ip;
    __u16 client_port;
    __u16 txid;
    __u16 qtype;
    __u16 payload_len;
    __u64 qname_hash;
    __u8  qname_area[24];   // DNS 头之后的原始字节（含 QNAME），由本层解析
    __u64 timestamp_ns;
};

/**
 * @brief 从原始字节解析 QNAME 并计算 FNV-1a 哈希。
 *
 * eBPF 只 capture 原始字节；解析放在用户态：
 *   - verifier 会拒绝 eBPF 内的变址栈访问与复杂控制流
 *   - 解析本属用户态职责（与项目"eBPF 只产证据、用户态做语义"的分层一致）
 * 返回 0 表示无法解析（此时 qname_hash 保持未设置，不影响配对）。
 */
static uint64_t qname_hash_from_area(const __u8* area, size_t n) {
    uint64_t h = 1469598103934665603ULL;  // FNV-1a 64 offset basis
    bool saw_label = false;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t c = area[i];
        if (c == 0) break;               // QNAME 结束
        if (c >= 'A' && c <= 'Z') {
            // DNS 名字大小写不敏感
        }
        const uint8_t lc = (c >= 'A' && c <= 'Z') ? static_cast<uint8_t>(c + 32) : c;
        saw_label = true;
        h ^= lc;
        h *= 1099511628211ULL;
    }
    return saw_label ? h : 0;
}

// Mirror of the BPF-side capture counter enum (dns_monitor.bpf.c).
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

struct DnsCaptureCounters {
    uint64_t values[DNS_STAT_MAX] = {};
};

struct DnsDrainStats {
    uint64_t poll_calls = 0;
    uint64_t poll_records = 0;
    uint64_t poll_errors = 0;
    uint64_t sample_callbacks = 0;
    uint64_t lost_events = 0;
    uint64_t tracker_query_accepted = 0;
    uint64_t tracker_response_accepted = 0;
};


/**
 * @brief Pimpl 实现结构体，持有 libbpf 句柄
 *
 * 采用 Pimpl 模式隔离 libbpf 依赖，避免在没有 libbpf 的编译环境中暴露 BPF 类型
 */
struct DnsMonitor::Impl {
    int dns_queries_fd = -1;       ///< dns_queries Map fd（查询记录）
    int dns_stats_fd = -1;         ///< dns_stats Map fd（聚合统计）
    struct bpf_object *obj = nullptr;  ///< BPF 对象实例
    struct bpf_link *link_send = nullptr;  ///< kprobe/udp_sendmsg 的 BPF link
    struct bpf_link *link_recv = nullptr;  ///< kprobe/udp_recvmsg 的 BPF link
    struct bpf_link *link_queue = nullptr; ///< kprobe/udp_queue_rcv_skb（响应单源）
    std::vector<struct bpf_link *> capture_links;
    struct perf_buffer *events = nullptr;
    int dns_capture_fd = -1;
    uint64_t lost_events = 0;
    DnsDrainStats drain_stats{};
    // 上一次读取的 BPF 累计计数器，用于计算本窗口的传输层增量。
    DnsCaptureCounters last_counters{};
    bool has_last_counters = false;
    uint64_t last_lost_events = 0;
    weaknet::DnsTransactionTracker* drain_tracker = nullptr;
};

static void on_dns_event(void* ctx, int /*cpu*/, void* data, __u32 size) {
    auto* monitor = static_cast<DnsMonitor::Impl*>(ctx);
    if (!monitor || !monitor->drain_tracker || size < sizeof(dns_event)) return;
    const auto* event = static_cast<const dns_event*>(data);
    weaknet::DnsCanonicalKey key;
    key.family = weaknet::AddressFamily::IPv4;
    key.client_ip = ntohl(event->client_ip);
    key.resolver_ip = ntohl(event->resolver_ip);
    key.client_port = event->client_port;
    key.txid = event->txid;
    if (event->qname_hash) {
        key.qname_hash = event->qname_hash;
    } else {
        const uint64_t qh = qname_hash_from_area(event->qname_area, sizeof(event->qname_area));
        if (qh) key.qname_hash = qh;   // 解析不出则不设置，不伪造
    }
    if (event->qtype) key.qtype = event->qtype;
    auto now = std::chrono::steady_clock::now();
    auto quality = event->fingerprint_quality == 0 ? weaknet::FingerprintQuality::ENRICHED
                                                    : weaknet::FingerprintQuality::PARTIAL;
    // 配对诊断：低频打印真实 canonical 字段，用于定位 query/response 键差异。
    // 计数只能说明"没配上"，不能说明"哪个字段不同"。
    {
        static std::atomic<int> diag_budget{kDnsKeyDumpBudget};
        if (diag_budget.load() > 0 && diag_budget.fetch_sub(1) >= 0) {
            LOG_INFO(LogModule::NETWORK, "DNSKEY dir=" << (int)event->direction
                << " cli=" << key.client_ip << ":" << key.client_port
                << " rs=" << key.resolver_ip
                << " txid=" << key.txid);
        }
    }
    if (event->direction == 0) {
        if (monitor->drain_tracker->onQueryCaptured(key, quality, now)) {
            monitor->drain_stats.tracker_query_accepted++;
        }
    } else {
        monitor->drain_tracker->onResponseCaptured(key, event->rcode, event->tc != 0,
                                                   event->malformed != 0, now);
        monitor->drain_stats.tracker_response_accepted++;
    }
    monitor->drain_stats.sample_callbacks++;
}

static void on_dns_lost(void* ctx, int /*cpu*/, __u64 lost) {
    auto* monitor = static_cast<DnsMonitor::Impl*>(ctx);
    if (monitor) {
        monitor->drain_stats.lost_events += lost;
        monitor->lost_events += lost;
    }
}

static DnsCaptureCounters read_capture_counters(int map_fd) {
    DnsCaptureCounters result;
    if (map_fd < 0) return result;
    for (int key = 0; key < DNS_STAT_MAX; ++key) {
        __u64 total = 0;
        // Sum across all CPUs (per-CPU array, read in one batch).
        __u32 ncpus = libbpf_num_possible_cpus();
        if (ncpus == 0 || ncpus > 512) continue;
        std::vector<__u64> per_cpu(ncpus, 0);
        if (bpf_map_lookup_elem(map_fd, &key, per_cpu.data()) == 0) {
            for (__u32 c = 0; c < ncpus; ++c) total += per_cpu[c];
        }
        result.values[key] = total;
    }
    return result;
}

DnsMonitor::DnsMonitor()
    : impl_(std::make_unique<Impl>()) {}

DnsMonitor::~DnsMonitor() {
    stop();
}

/**
 * @brief 初始化 DNS 监控器：加载 BPF 对象并挂载探针
 *
 * 初始化流程：
 *   1. 打开并加载 BPF 对象文件（dns_monitor.bpf.o）
 *   2. 查找 dns_queries 和 dns_stats 两个 Map 的 fd
 *   3. 找到 trace_dns_send 和 trace_dns_recv 两个 BPF 程序
 *   4. attach 到内核 kprobe：udp_sendmsg 和 udp_recvmsg
 *
 * @param bpfObjPath BPF 对象文件路径（通常为 "build/dns_monitor.bpf.o"）
 * @return true  初始化成功，探针已挂载
 *         false 初始化失败（libbpf 不可用、文件不存在、attach 失败等）
 */
bool DnsMonitor::init(const std::string& bpfObjPath, uint32_t capture_pages) {
    stateSupport_.setState(EbpfMonitorState::Initializing, false, "loading BPF object");
#if !HAVE_LIBBPF
    LOG_INFO(LogModule::NETWORK, "DnsMonitor: BPF not available (no libbpf)");
    available_ = false;
    initialized_ = true;
    stateSupport_.setState(EbpfMonitorState::Fallback, false, "libbpf unavailable");
    return false;
#else
    LOG_INFO(LogModule::NETWORK, "DnsMonitor: loading BPF object from " << bpfObjPath);

    LIBBPF_OPTS(bpf_object_open_opts, opts);
    struct bpf_object *obj = bpf_object__open_file(bpfObjPath.c_str(), &opts);
    if (!obj) {
        LOG_ERROR(LogModule::NETWORK, "DnsMonitor: failed to open BPF object: " << bpfObjPath);
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Error, false, "failed to open BPF object");
        return false;
    }

    if (bpf_object__load(obj) != 0) {
        LOG_ERROR(LogModule::NETWORK, "DnsMonitor: failed to load BPF object");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Error, false, "failed to load BPF object");
        return false;
    }

    impl_->dns_queries_fd = bpf_object__find_map_fd_by_name(obj, "dns_queries");
    if (impl_->dns_queries_fd < 0) {
        LOG_ERROR(LogModule::NETWORK, "DnsMonitor: dns_queries map not found");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Error, false, "dns_queries map not found");
        return false;
    }

    impl_->dns_stats_fd = bpf_object__find_map_fd_by_name(obj, "dns_stats");
    if (impl_->dns_stats_fd < 0) {
        LOG_ERROR(LogModule::NETWORK, "DnsMonitor: dns_stats map not found");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Error, false, "dns_stats map not found");
        return false;
    }

    impl_->dns_capture_fd = bpf_object__find_map_fd_by_name(obj, "dns_capture_counters");

    // attach 探针到 kprobe/udp_sendmsg 和 kprobe/udp_recvmsg
    // 内核探针类型：kprobe（函数入口）
    struct bpf_program *send_prog = bpf_object__find_program_by_name(obj, "trace_dns_egress_skb");
    struct bpf_program *recv_prog = bpf_object__find_program_by_name(obj, "trace_dns_recv");
    if (!send_prog || !recv_prog) {
        LOG_ERROR(LogModule::NETWORK, "DnsMonitor: BPF program not found");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Error, false, "BPF program not found");
        return false;
    }

    impl_->link_send = bpf_program__attach(send_prog);
    long err_send = libbpf_get_error(impl_->link_send);
    if (err_send) {
        LOG_ERROR(LogModule::NETWORK, "DnsMonitor: attach kprobe/udp_sendmsg failed err=" << err_send);
        impl_->link_send = nullptr;
    }

    // 响应侧单一来源：udp_queue_rcv_skb 同时持有 sk(endpoint) 与 skb(payload)
    struct bpf_program *queue_prog = bpf_object__find_program_by_name(obj, "trace_dns_queue_rcv");
    if (queue_prog) {
        impl_->link_queue = bpf_program__attach(queue_prog);
        if (libbpf_get_error(impl_->link_queue)) {
            LOG_WARNING(LogModule::NETWORK, "DnsMonitor: attach kprobe/udp_queue_rcv_skb failed");
            impl_->link_queue = nullptr;
        } else {
            stateSupport_.recordProbeAttached();
        }
    }

    impl_->link_recv = bpf_program__attach(recv_prog);
    long err_recv = libbpf_get_error(impl_->link_recv);
    if (err_recv) {
        LOG_ERROR(LogModule::NETWORK, "DnsMonitor: attach kprobe/udp_recvmsg failed err=" << err_recv);
        impl_->link_recv = nullptr;
    }

    if (!impl_->link_send && !impl_->link_recv) {
        LOG_ERROR(LogModule::NETWORK, "DnsMonitor: both probes failed to attach");
        bpf_object__close(obj);
        impl_->link_send = impl_->link_recv = nullptr;
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Fallback, false, "all BPF probes failed to attach");
        return false;
    }

    impl_->obj = obj;

    // Attach every raw_syscall capture program. bpf_program__attach resolves
    // the tracepoint from the SEC section; explicit iteration keeps the attach
    // error visible per program instead of relying on autoload side effects.
    {
        struct bpf_program *prog = nullptr;
        bpf_object__for_each_program(prog, obj) {
            const char *sec = bpf_program__section_name(prog);
            if (!sec || strncmp(sec, "tracepoint/raw_syscalls/", strlen("tracepoint/raw_syscalls/")) != 0) {
                continue;
            }
            struct bpf_link *link = bpf_program__attach(prog);
            if (libbpf_get_error(link)) {
                LOG_WARNING(LogModule::NETWORK, "DnsMonitor: attach failed for prog "
                            << bpf_program__name(prog) << " section=" << sec);
                link = nullptr;
            } else {
                impl_->capture_links.push_back(link);
                stateSupport_.recordProbeAttached();
            }
        }
        LOG_INFO(LogModule::NETWORK, "DnsMonitor: raw_syscall capture programs attached="
                 << impl_->capture_links.size());
    }

    {
        auto events_fd = bpf_object__find_map_fd_by_name(obj, "dns_events");
        if (events_fd >= 0) {
            impl_->events = perf_buffer__new(events_fd, capture_pages, on_dns_event, on_dns_lost,
                                             impl_.get(), nullptr);
            if (!impl_->events) {
                LOG_WARNING(LogModule::NETWORK, "DnsMonitor: perf buffer unavailable; lifecycle events disabled");
            } else {
                LOG_INFO(LogModule::NETWORK, "DnsMonitor: perf buffer pages=" << capture_pages);
            }
        }
    }
    available_ = true;
    initialized_ = true;

    LOG_INFO(LogModule::NETWORK, "DnsMonitor: initialized successfully");
    // 预创建 key=0 聚合条目：BPF 侧只在首个 DNS 事件到达时才创建该条目，
    // 若窗口期内无 DNS 流量，用户态 lookup 会得到 ENOENT。预先写入零值记录，
    // 保证 getStats() 在无流量时也能成功读到零值而不是被计为读取错误。
    {
        __u32 stats_key = 0;
        dns_stats_record empty_record = {};
        if (bpf_map_update_elem(impl_->dns_stats_fd, &stats_key, &empty_record, BPF_ANY) != 0) {
            LOG_WARNING(LogModule::NETWORK,
                        "DnsMonitor: pre-create dns_stats entry failed (errno=" << errno
                        << "), getStats will treat ENOENT as empty stats");
        }
    }
    stateSupport_.setState(EbpfMonitorState::Attached, true, "BPF probes attached");
    stateSupport_.recordProbeAttached();
    stateSupport_.recordProbeAttached();
    return true;
#endif
}

/**
 * @brief 停止 DNS 监控器：销毁 BPF link 和 BPF 对象
 *
 * 释放所有 libbpf 资源，将状态置为 Stopped，available 置为 false
 */
void DnsMonitor::stop() {
    stateSupport_.setState(EbpfMonitorState::Stopped, false, "stopped");
#if HAVE_LIBBPF
    if (impl_->events) { perf_buffer__free(impl_->events); impl_->events = nullptr; }
    if (impl_->link_send) { bpf_link__destroy(impl_->link_send); impl_->link_send = nullptr; }
    if (impl_->link_recv) { bpf_link__destroy(impl_->link_recv); impl_->link_recv = nullptr; }
    if (impl_->link_queue) { bpf_link__destroy(impl_->link_queue); impl_->link_queue = nullptr; }
    for (auto *link : impl_->capture_links) { if (link) bpf_link__destroy(link); }
    impl_->capture_links.clear();
#endif
    if (impl_->obj) {
        bpf_object__close(impl_->obj);
        impl_->obj = nullptr;
        LOG_INFO(LogModule::NETWORK, "DnsMonitor: stopped");
    }
    impl_->dns_queries_fd = -1;
    impl_->dns_stats_fd = -1;
    available_ = false;
}

/**
 * @brief 从 dns_stats Map 读取 DNS 聚合统计
 *
 * 读取流程：通过 bpf_map_lookup_elem 查询 key=0 的 dns_stats Map 条目，
 * 然后从原始纳秒值转换为毫秒（/ 1000000）。
 *
 * @return DnsAggStats DNS 聚合统计结果；失败时返回零值结构体
 */
DnsAggStats DnsMonitor::getStats() {
    DnsAggStats result = {};
#if HAVE_LIBBPF
    if (!available_ || impl_->dns_stats_fd < 0)
        return result;

    auto started = std::chrono::steady_clock::now();
    __u32 key = 0;
    dns_stats_record stats = {};
    if (bpf_map_lookup_elem(impl_->dns_stats_fd, &key, &stats) == 0) {
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
        stateSupport_.recordReadSuccess(static_cast<uint64_t>(elapsed));
        result.totalQueries = stats.total_queries;
        result.totalResponses = stats.total_responses;
        result.totalTimeouts = stats.total_timeouts;
        result.totalErrors = stats.total_errors;
        // 平均延迟 = 累计总延迟 / 响应数（避免除以零）
        result.avgLatencyMs = (stats.total_responses > 0)
            ? (stats.total_latency_ns / stats.total_responses / 1000000)
            : 0;
        result.maxLatencyMs = stats.max_latency_ns / 1000000;
    } else if (errno == ENOENT) {
        // 条目尚未创建（窗口期内无 DNS 流量）——“暂无数据”不是监控故障，
        // 记为一次成功读取并返回零值，避免把空闲期误报成持续读取错误。
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
        stateSupport_.recordReadSuccess(static_cast<uint64_t>(elapsed));
    } else {
        stateSupport_.recordReadFailure("dns_stats map lookup failed");
    }
#endif
    return result;
}

/**
 * @brief 获取 DNS 平均解析延迟（毫秒）
 * @return 平均延迟 ms；无数据时返回 0.0
 */
double DnsMonitor::getAvgLatencyMs() {
    auto stats = getStats();
    if (stats.totalResponses == 0) return 0.0;
    return stats.avgLatencyMs;
}

/**
 * @brief 获取 DNS 超时率
 * @return 超时率（0.0 ~ 1.0），由 DnsAggStats::timeoutRate() 计算
 */
double DnsMonitor::getTimeoutRate() {
    auto stats = getStats();
    return stats.timeoutRate();
}

size_t DnsMonitor::drainEvents(weaknet::DnsTransactionTracker* tracker) {
    if (!tracker || !impl_->events) return 0;
    impl_->drain_tracker = tracker;
    impl_->drain_stats.poll_calls++;

    // Drain the whole backlog, not just one poll pass. A burst of syscalls can
    // leave far more records ready than a single non-blocking pass consumes;
    // stopping early is what forced the perf buffer to drop events.
    constexpr int kMaxPollRounds = 64;
    uint64_t total_records = 0;
    int last_ret = 0;
    for (int round = 0; round < kMaxPollRounds; ++round) {
        last_ret = perf_buffer__poll(impl_->events, 0);
        if (last_ret <= 0) break;
        total_records += static_cast<uint64_t>(last_ret);
    }
    impl_->drain_tracker = nullptr;

    if (last_ret < 0 && last_ret != -EINTR) {
        impl_->drain_stats.poll_errors++;
        LOG_WARNING(LogModule::NETWORK, "DnsMonitor: perf buffer poll failed: " << last_ret);
        return 0;
    }
    impl_->drain_stats.poll_records += total_records;
    auto lost = consumeLostEvents();
    if (lost) tracker->recordDeliveryLoss(lost);
    return static_cast<size_t>(total_records);
}

std::string DnsMonitor::getCaptureDiagnostics() {
#if HAVE_LIBBPF
    auto counters = read_capture_counters(impl_->dns_capture_fd);
    const auto& s = impl_->drain_stats;
    // Two transport stages are reported separately and never summed: capture-stage
    // emit failure and perf-stage delivery loss are different failure points, and
    // whether they describe the same congestion episode is unverified.
    const uint64_t capture_attempts = impl_->last_counters.values[DNS_STAT_SENDTO_ENTER]
        + impl_->last_counters.values[DNS_STAT_SENDMSG_ENTER]
        + impl_->last_counters.values[DNS_STAT_SENDMMSG_ENTER]
        + impl_->last_counters.values[DNS_STAT_RECVFROM_ENTER]
        + impl_->last_counters.values[DNS_STAT_RECVMSG_ENTER]
        + impl_->last_counters.values[DNS_STAT_RECVMMSG_EXIT];
    const uint64_t emit_fail = counters.values[DNS_STAT_EMIT_FAIL];
    const uint64_t delivered = counters.values[DNS_STAT_EMITTED];
    const uint64_t perf_lost = s.lost_events;
    const double emit_fail_ratio = capture_attempts > 0
        ? static_cast<double>(emit_fail) / static_cast<double>(capture_attempts) : 0.0;
    const double perf_loss_ratio = (delivered + perf_lost) > 0
        ? static_cast<double>(perf_lost) / static_cast<double>(delivered + perf_lost) : 0.0;
    std::ostringstream json;
    json << "{"
         << "\"capture\":{"
         << "\"sendto\":" << counters.values[DNS_STAT_SENDTO_ENTER] << ","
         << "\"sendmsg\":" << counters.values[DNS_STAT_SENDMSG_ENTER] << ","
         << "\"sendmmsg\":" << counters.values[DNS_STAT_SENDMMSG_ENTER] << ","
         << "\"recvfrom\":" << counters.values[DNS_STAT_RECVFROM_ENTER] << ","
         << "\"recvmsg_enter\":" << counters.values[DNS_STAT_RECVMSG_ENTER] << ","
         << "\"recvmsg_exit\":" << counters.values[DNS_STAT_RECVMSG_EXIT] << ","
         << "\"recvmmsg_exit\":" << counters.values[DNS_STAT_RECVMMSG_EXIT] << ","
         << "\"msghdr_read_fail\":" << counters.values[DNS_STAT_MSGHDR_READ_FAIL] << ","
         << "\"iovec_read_fail\":" << counters.values[DNS_STAT_IOVEC_READ_FAIL] << ","
         << "\"payload_read_fail\":" << counters.values[DNS_STAT_PAYLOAD_READ_FAIL] << ","
         << "\"short_payload\":" << counters.values[DNS_STAT_SHORT_PAYLOAD] << ","
         << "\"not_dns_port\":" << counters.values[DNS_STAT_NOT_DNS_PORT] << ","
         << "\"port_unknown\":" << counters.values[DNS_STAT_PORT_UNKNOWN] << ","
         << "\"connect_seen\":" << counters.values[DNS_STAT_CONNECT_SEEN] << ","
         << "\"send_enter\":" << counters.values[DNS_STAT_SEND_ENTER] << ","
         << "\"queue_enter\":" << counters.values[DNS_STAT_QUEUE_ENTER] << ","
         << "\"send_dns_port\":" << counters.values[DNS_STAT_SEND_DNS_PORT] << ","
         << "\"send_nonipv4\":" << counters.values[DNS_STAT_SEND_NONIPV4] << ","
         << "\"send_nondns\":" << counters.values[DNS_STAT_SEND_NON_DNS_PORT] << ","
         << "\"queue_dns_port\":" << counters.values[DNS_STAT_QUEUE_DNS_PORT] << ","
         << "\"send_has_msg_name\":" << counters.values[DNS_STAT_SEND_HAS_MSG_NAME] << ","
         << "\"send_connected_peer\":" << counters.values[DNS_STAT_SEND_CONNECTED_PEER] << ","
         << "\"send_payload_fail\":" << counters.values[DNS_STAT_SEND_PAYLOAD_FAIL] << ","
         << "\"send_emitted\":" << counters.values[DNS_STAT_SEND_EMITTED] << ","
         << "\"queue_nonipv4\":" << counters.values[DNS_STAT_QUEUE_NONIPV4] << ","
         << "\"queue_header_fail\":" << counters.values[DNS_STAT_QUEUE_HEADER_FAIL] << ","
         << "\"queue_payload_fail\":" << counters.values[DNS_STAT_QUEUE_PAYLOAD_FAIL] << ","
         << "\"queue_nondns\":" << counters.values[DNS_STAT_QUEUE_NONDNS] << ","
         << "\"queue_emitted\":" << counters.values[DNS_STAT_QUEUE_EMITTED] << ","
         << "\"emitted\":" << delivered << ","
         << "\"emit_fail\":" << emit_fail
         << "},\"drain\":{"
         << "\"poll_calls\":" << s.poll_calls << ","
         << "\"poll_records\":" << s.poll_records << ","
         << "\"poll_errors\":" << s.poll_errors << ","
         << "\"sample_callbacks\":" << s.sample_callbacks << ","
         << "\"lost_events\":" << perf_lost << ","
         << "\"tracker_query_accepted\":" << s.tracker_query_accepted << ","
         << "\"tracker_response_accepted\":" << s.tracker_response_accepted
         << "},\"delivery\":{"
         << "\"capture_attempts\":" << capture_attempts << ","
         << "\"capture_emit_failures\":" << emit_fail << ","
         << "\"delivered_events\":" << delivered << ","
         << "\"perf_lost_events\":" << perf_lost << ","
         << "\"capture_emit_failure_ratio\":" << std::fixed << std::setprecision(6) << emit_fail_ratio << ","
         << "\"perf_delivery_loss_ratio\":" << std::fixed << std::setprecision(6) << perf_loss_ratio
         << "}}";
    return json.str();
#else
    return "{}";
#endif
}

uint64_t DnsMonitor::consumeLostEvents() {
    auto value = impl_->lost_events;
    impl_->lost_events = 0;
    return value;
}

void DnsMonitor::feedTransportDelta(weaknet::DnsTransactionTracker* tracker) {
    if (!tracker || impl_->dns_capture_fd < 0) return;
    auto counters = read_capture_counters(impl_->dns_capture_fd);

    auto sum_entries = [](const DnsCaptureCounters& c) {
        return c.values[DNS_STAT_SENDTO_ENTER] + c.values[DNS_STAT_SENDMSG_ENTER]
             + c.values[DNS_STAT_SENDMMSG_ENTER] + c.values[DNS_STAT_RECVFROM_ENTER]
             + c.values[DNS_STAT_RECVMSG_ENTER] + c.values[DNS_STAT_RECVMMSG_EXIT];
    };

    if (!impl_->has_last_counters) {
        impl_->last_counters = counters;
        impl_->has_last_counters = true;
        return; // 首个窗口只建立基线
    }

    const auto& prev = impl_->last_counters;
    const uint64_t attempts_delta = sum_entries(counters) - sum_entries(prev);
    const uint64_t emit_fail_delta = counters.values[DNS_STAT_EMIT_FAIL] - prev.values[DNS_STAT_EMIT_FAIL];
    const uint64_t emitted_delta = counters.values[DNS_STAT_EMITTED] - prev.values[DNS_STAT_EMITTED];
    const uint64_t lost_delta = impl_->drain_stats.lost_events - impl_->last_lost_events;

    tracker->recordTransportDelta(attempts_delta, emit_fail_delta, emitted_delta, lost_delta);

    impl_->last_counters = counters;
    impl_->last_lost_events = impl_->drain_stats.lost_events;
}

}  // namespace weaknet_dbus

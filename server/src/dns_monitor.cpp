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
#include <arpa/inet.h>

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

// ---- 实现 ----

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
};

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
bool DnsMonitor::init(const std::string& bpfObjPath) {
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

    // attach 探针到 kprobe/udp_sendmsg 和 kprobe/udp_recvmsg
    // 内核探针类型：kprobe（函数入口）
    struct bpf_program *send_prog = bpf_object__find_program_by_name(obj, "trace_dns_send");
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
    if (impl_->link_send) { bpf_link__destroy(impl_->link_send); impl_->link_send = nullptr; }
    if (impl_->link_recv) { bpf_link__destroy(impl_->link_recv); impl_->link_recv = nullptr; }
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

}  // namespace weaknet_dbus

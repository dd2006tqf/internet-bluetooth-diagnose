// dns_monitor.cpp
// DNS 监控器 - 用户态实现
// 从 BPF Map 读取 DNS 查询记录，计算解析延迟

#include "dns_monitor.hpp"
#include "logger.hpp"

#include <cstring>
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

// ---- 数据结构映射（与 BPF 端一致） ----

struct dns_query_key {
    __u32 saddr;
    __u32 daddr;
    __u16 sport;
} __attribute__((packed));

struct dns_query_record {
    __u64 send_time_ns;
    __u64 recv_time_ns;
    __u32 reply_len;
    __u8  rcode;
    __u8  is_response;
};

struct dns_stats_record {
    __u64 total_queries;
    __u64 total_responses;
    __u64 total_timeouts;
    __u64 total_errors;
    __u64 total_latency_ns;
    __u64 max_latency_ns;
};

// ---- 实现 ----

struct DnsMonitor::Impl {
    int dns_queries_fd = -1;
    int dns_stats_fd = -1;
    struct bpf_object *obj = nullptr;
};

DnsMonitor::DnsMonitor()
    : impl_(std::make_unique<Impl>()) {}

DnsMonitor::~DnsMonitor() {
    stop();
}

bool DnsMonitor::init(const std::string& bpfObjPath) {
#if !HAVE_LIBBPF
    LOG_INFO(LogModule::NETWORK, "DnsMonitor: BPF not available (no libbpf)");
    available_ = false;
    initialized_ = true;
    return false;
#else
    LOG_INFO(LogModule::NETWORK, "DnsMonitor: loading BPF object from " << bpfObjPath);

    struct bpf_object_open_opts opts = {};
    struct bpf_object *obj = bpf_object__open_file(bpfObjPath.c_str(), &opts);
    if (!obj) {
        LOG_ERROR(LogModule::NETWORK, "DnsMonitor: failed to open BPF object: " << bpfObjPath);
        available_ = false;
        initialized_ = true;
        return false;
    }

    if (bpf_object__load(obj) != 0) {
        LOG_ERROR(LogModule::NETWORK, "DnsMonitor: failed to load BPF object");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        return false;
    }

    impl_->dns_queries_fd = bpf_object__find_map_fd_by_name(obj, "dns_queries");
    if (impl_->dns_queries_fd < 0) {
        LOG_ERROR(LogModule::NETWORK, "DnsMonitor: dns_queries map not found");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        return false;
    }

    impl_->dns_stats_fd = bpf_object__find_map_fd_by_name(obj, "dns_stats");
    if (impl_->dns_stats_fd < 0) {
        LOG_ERROR(LogModule::NETWORK, "DnsMonitor: dns_stats map not found");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        return false;
    }

    impl_->obj = obj;
    available_ = true;
    initialized_ = true;

    LOG_INFO(LogModule::NETWORK, "DnsMonitor: initialized successfully");
    return true;
#endif
}

void DnsMonitor::stop() {
    if (impl_->obj) {
        bpf_object__close(impl_->obj);
        impl_->obj = nullptr;
        LOG_INFO(LogModule::NETWORK, "DnsMonitor: stopped");
    }
    impl_->dns_queries_fd = -1;
    impl_->dns_stats_fd = -1;
    available_ = false;
}

DnsAggStats DnsMonitor::getStats() {
    DnsAggStats result = {};
#if HAVE_LIBBPF
    if (!available_ || impl_->dns_stats_fd < 0)
        return result;

    // 读取 PERCPU 统计
    __u32 key = 0;
    dns_stats_record percpu_stats = {};
    if (bpf_map_lookup_elem(impl_->dns_stats_fd, &key, &percpu_stats) == 0) {
        result.totalQueries = percpu_stats.total_queries;
        result.totalResponses = percpu_stats.total_responses;
        result.totalTimeouts = percpu_stats.total_timeouts;
        result.totalErrors = percpu_stats.total_errors;
        result.avgLatencyMs = (percpu_stats.total_responses > 0)
            ? (percpu_stats.total_latency_ns / percpu_stats.total_responses / 1000000)
            : 0;
        result.maxLatencyMs = percpu_stats.max_latency_ns / 1000000;
    }
#endif
    return result;
}

double DnsMonitor::getAvgLatencyMs() {
    auto stats = getStats();
    if (stats.totalResponses == 0) return 0.0;
    return stats.avgLatencyMs;
}

double DnsMonitor::getTimeoutRate() {
    auto stats = getStats();
    return stats.timeoutRate();
}

}  // namespace weaknet_dbus
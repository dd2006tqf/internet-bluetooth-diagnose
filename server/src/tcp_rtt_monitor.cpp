/**
 * @file tcp_rtt_monitor.cpp
 * @brief TcpRttMonitor 用户态实现
 */

#include "tcp_rtt_monitor.hpp"
#include <algorithm>
#include <mutex>
#include <arpa/inet.h>
#include <unistd.h>
#include "logger.hpp"

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

namespace {

struct TcpConnKeyBpf {
    uint32_t saddr;
    uint32_t daddr;
    uint16_t sport;
    uint16_t dport;
    uint16_t family;
    uint16_t pad;
};

struct TcpRttStatBpf {
    uint32_t srtt_us;
    uint32_t min_rtt_us;
    uint32_t max_rtt_us;
    uint32_t snd_cwnd;
    uint32_t snd_wnd;
    uint32_t rcv_wnd;
    uint64_t sample_count;
    uint64_t last_timestamp_ns;
    uint64_t sock_cookie;
};

struct TcpRttGlobalStatBpf {
    uint64_t total_samples;
    uint64_t total_conns;
    uint64_t srtt_sum_us;
    uint32_t min_srtt_us;
    uint32_t max_srtt_us;
};

std::string formatIpv4(uint32_t addr) {
    char buf[INET_ADDRSTRLEN] = {0};
    struct in_addr in;
    in.s_addr = addr;
    if (::inet_ntop(AF_INET, &in, buf, sizeof(buf))) {
        return std::string(buf);
    }
    return "";
}

} // namespace

struct TcpRttMonitor::Impl {
    mutable std::mutex mutex;
    EbpfMonitorState state = EbpfMonitorState::Uninitialized;
    EbpfMonitorMetrics metricsData;
    uint64_t consecutiveErrors = 0;
    uint64_t lastSampleNs = 0;
    std::string bpfPath;

#if HAVE_LIBBPF
    struct bpf_object* obj = nullptr;
    struct bpf_link* link = nullptr;
    int statsMapFd = -1;
    int globalMapFd = -1;
    int selfPidMapFd = -1;
#endif

    TcpRttSummary cachedSummary;
};

TcpRttMonitor::TcpRttMonitor() : impl_(std::make_unique<Impl>()) {}

TcpRttMonitor::~TcpRttMonitor() {
    stop();
}

bool TcpRttMonitor::init(const std::string& bpfObjPath, const weaknet::MapSizingPlan& plan) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->bpfPath = bpfObjPath;
    impl_->state = EbpfMonitorState::Initializing;

#if HAVE_LIBBPF
    impl_->obj = bpf_object__open_file(bpfObjPath.c_str(), nullptr);
    if (!impl_->obj) {
        LOG_WARNING(LogModule::TCP_LOSS, "TcpRttMonitor: failed to open BPF object: " << bpfObjPath);
        impl_->state = EbpfMonitorState::Fallback;
        return false;
    }

    weaknet::applyMapSizingPlan(impl_->obj, plan);

    if (bpf_object__load(impl_->obj) != 0) {
        LOG_WARNING(LogModule::TCP_LOSS, "TcpRttMonitor: failed to load BPF object");
        bpf_object__close(impl_->obj);
        impl_->obj = nullptr;
        impl_->state = EbpfMonitorState::Fallback;
        return false;
    }

    struct bpf_program* prog = bpf_object__find_program_by_name(impl_->obj, "trace_tcp_probe");
    if (!prog) {
        LOG_WARNING(LogModule::TCP_LOSS, "TcpRttMonitor: program trace_tcp_probe not found");
        bpf_object__close(impl_->obj);
        impl_->obj = nullptr;
        impl_->state = EbpfMonitorState::Fallback;
        return false;
    }

    impl_->link = bpf_program__attach(prog);
    if (!impl_->link) {
        LOG_WARNING(LogModule::TCP_LOSS, "TcpRttMonitor: failed to attach tracepoint trace_tcp_probe");
        bpf_object__close(impl_->obj);
        impl_->obj = nullptr;
        impl_->state = EbpfMonitorState::Fallback;
        return false;
    }

    impl_->statsMapFd = bpf_object__find_map_fd_by_name(impl_->obj, "tcp_rtt_stats");
    impl_->globalMapFd = bpf_object__find_map_fd_by_name(impl_->obj, "tcp_rtt_global");
    impl_->selfPidMapFd = bpf_object__find_map_fd_by_name(impl_->obj, "tcp_rtt_self_pid");

    if (impl_->statsMapFd < 0 || impl_->globalMapFd < 0) {
        LOG_WARNING(LogModule::TCP_LOSS, "TcpRttMonitor: essential BPF maps not found");
        if (impl_->link) { bpf_link__destroy(impl_->link); impl_->link = nullptr; }
        bpf_object__close(impl_->obj);
        impl_->obj = nullptr;
        impl_->state = EbpfMonitorState::Fallback;
        return false;
    }

    // 设置自身 PID 过滤
    if (impl_->selfPidMapFd >= 0) {
        uint32_t key = 0;
        uint32_t pid = static_cast<uint32_t>(::getpid());
        bpf_map_update_elem(impl_->selfPidMapFd, &key, &pid, BPF_ANY);
    }

    impl_->metricsData.attachedProbes = 1;
    impl_->state = EbpfMonitorState::Attached;
    LOG_INFO(LogModule::TCP_LOSS, "TcpRttMonitor: eBPF attached successfully (trace_tcp_probe)");
    return true;
#else
    impl_->state = EbpfMonitorState::Fallback;
    return false;
#endif
}

void TcpRttMonitor::stop() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
#if HAVE_LIBBPF
    if (impl_->link) {
        bpf_link__destroy(impl_->link);
        impl_->link = nullptr;
    }
    if (impl_->obj) {
        bpf_object__close(impl_->obj);
        impl_->obj = nullptr;
    }
    impl_->statsMapFd = -1;
    impl_->globalMapFd = -1;
    impl_->selfPidMapFd = -1;
#endif
    impl_->state = EbpfMonitorState::Stopped;
}

TcpRttSummary TcpRttMonitor::getStats() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    TcpRttSummary summary;

#if HAVE_LIBBPF
    if (impl_->state != EbpfMonitorState::Attached || impl_->statsMapFd < 0) {
        return impl_->cachedSummary;
    }

    impl_->metricsData.mapReads++;

    // 1. 读取全局聚合统计
    if (impl_->globalMapFd >= 0) {
        uint32_t g_key = 0;
        TcpRttGlobalStatBpf g_val = {};
        if (bpf_map_lookup_elem(impl_->globalMapFd, &g_key, &g_val) == 0) {
            summary.totalSamples = g_val.total_samples;
            summary.minRttUs = g_val.min_srtt_us;
            summary.maxRttUs = g_val.max_srtt_us;
            if (g_val.total_samples > 0) {
                summary.avgRttUs = static_cast<uint32_t>(g_val.srtt_sum_us / g_val.total_samples);
            }
        }
    }

    // 2. 遍历 per-connection map
    TcpConnKeyBpf key = {}, next_key = {};
    TcpRttStatBpf val = {};
    std::vector<uint32_t> all_rtts;

    bool has_key = (bpf_map_get_next_key(impl_->statsMapFd, nullptr, &next_key) == 0);
    while (has_key) {
        key = next_key;
        if (bpf_map_lookup_elem(impl_->statsMapFd, &key, &val) == 0) {
            TcpRttConnItem item;
            item.saddr = formatIpv4(key.saddr);
            item.daddr = formatIpv4(key.daddr);
            item.sport = key.sport;
            item.dport = key.dport;
            item.srttUs = val.srtt_us;
            item.minRttUs = val.min_rtt_us;
            item.maxRttUs = val.max_rtt_us;
            item.sndCwnd = val.snd_cwnd;
            item.sndWnd = val.snd_wnd;
            item.rcvWnd = val.rcv_wnd;
            item.sampleCount = val.sample_count;
            item.lastTimestampNs = val.last_timestamp_ns;

            summary.topConns.push_back(item);
            all_rtts.push_back(val.srtt_us);
        }
        has_key = (bpf_map_get_next_key(impl_->statsMapFd, &key, &next_key) == 0);
    }

    summary.activeConns = summary.topConns.size();

    // 3. 计算分位数 (P50, P90, P99)
    if (!all_rtts.empty()) {
        std::sort(all_rtts.begin(), all_rtts.end());
        size_t n = all_rtts.size();
        summary.p50RttUs = all_rtts[n * 50 / 100];
        summary.p90RttUs = all_rtts[n * 90 / 100];
        summary.p99RttUs = all_rtts[std::min(n - 1, n * 99 / 100)];
    }

    // 4. 按最近采样时间排序取活跃连接
    std::sort(summary.topConns.begin(), summary.topConns.end(),
              [](const TcpRttConnItem& a, const TcpRttConnItem& b) {
                  return a.lastTimestampNs > b.lastTimestampNs;
              });

    impl_->cachedSummary = summary;
    impl_->metricsData.samples++;
    impl_->consecutiveErrors = 0;
#endif

    return summary;
}

EbpfMonitorState TcpRttMonitor::commonState() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->state;
}

bool TcpRttMonitor::isAvailable() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->state == EbpfMonitorState::Attached;
}

EbpfMonitorHealth TcpRttMonitor::health() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    EbpfMonitorHealth h;
    h.name = monitorName();
    h.state = impl_->state;
    h.available = (impl_->state == EbpfMonitorState::Attached);
    h.healthy = h.available && (impl_->consecutiveErrors < 3);
    h.lastSuccessfulSampleNs = impl_->lastSampleNs;
    h.consecutiveErrors = impl_->consecutiveErrors;
    h.status = ebpfMonitorStateName(impl_->state);
    return h;
}

EbpfMonitorMetrics TcpRttMonitor::metrics() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->metricsData;
}

void TcpRttMonitor::resetMetrics() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->metricsData = EbpfMonitorMetrics();
}

}  // namespace weaknet_dbus

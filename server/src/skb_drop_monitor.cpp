/**
 * @file skb_drop_monitor.cpp
 * @brief SkbDropMonitor 用户态实现
 */

#include "skb_drop_monitor.hpp"
#include <algorithm>
#include <mutex>
#include <unordered_map>
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

const char* skbDropReasonToString(uint32_t reason) {
    switch (reason) {
        case 1:  return "NO_SOCKET";
        case 2:  return "PKT_TOO_SMALL";
        case 3:  return "TCP_CSUM";
        case 4:  return "SOCKET_FILTER";
        case 5:  return "UDP_CSUM";
        case 6:  return "NETFILTER_DROP";
        case 7:  return "OTHERHOST";
        case 8:  return "IP_CSUM";
        case 9:  return "IP_INHDR";
        case 10: return "IP_RPFILTER";
        case 11: return "UNICAST_IN_L2_MULTICAST";
        default: return "NOT_SPECIFIED_OR_UNKNOWN";
    }
}

const char* skbDropReasonToDescription(uint32_t reason) {
    switch (reason) {
        case 1:  return "目标端口无监听套接字 (No socket listening on port)";
        case 2:  return "数据包长度过小或数据头损坏 (Packet too small)";
        case 3:  return "TCP 传输层校验和错误 (TCP checksum failure)";
        case 4:  return "BPF或套接字过滤规则丢弃 (Socket filter drop)";
        case 5:  return "UDP 校验和错误 (UDP checksum failure)";
        case 6:  return "iptables/nftables 防火墙规则拦截丢弃 (Netfilter drop)";
        case 7:  return "目的 MAC 不是本机且非混杂模式 (Other host MAC)";
        case 8:  return "IPv4 头部校验和错误 (IP checksum failure)";
        case 9:  return "IP 报头格式损坏或选项非法 (IP header invalid)";
        case 10: return "反向路径过滤 rp_filter 丢弃 (Reverse path filter drop)";
        case 11: return "单播包错误送入二层组播 (Unicast in L2 multicast)";
        default: return "内核常规丢弃或未指定分类";
    }
}

struct SkbDropMonitor::Impl {
    mutable std::mutex mutex;
    EbpfMonitorState state = EbpfMonitorState::Uninitialized;
    EbpfMonitorMetrics metricsData;
    uint64_t consecutiveErrors = 0;
    uint64_t lastSampleNs = 0;
    std::string bpfPath;

#if HAVE_LIBBPF
    struct bpf_object* obj = nullptr;
    struct bpf_link* link = nullptr;
    int mapFd = -1;
#endif

    DropStatsSummary cachedSummary;
};

SkbDropMonitor::SkbDropMonitor() : impl_(std::make_unique<Impl>()) {}

SkbDropMonitor::~SkbDropMonitor() {
    stop();
}

bool SkbDropMonitor::init(const std::string& bpfObjPath) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->bpfPath = bpfObjPath;
    impl_->state = EbpfMonitorState::Initializing;

#if HAVE_LIBBPF
    impl_->obj = bpf_object__open_file(bpfObjPath.c_str(), nullptr);
    if (!impl_->obj) {
        LOG_WARNING(LogModule::NETWORK, "SkbDropMonitor: failed to open BPF object: " << bpfObjPath);
        impl_->state = EbpfMonitorState::Fallback;
        return false;
    }

    if (bpf_object__load(impl_->obj) != 0) {
        LOG_WARNING(LogModule::NETWORK, "SkbDropMonitor: failed to load BPF object");
        bpf_object__close(impl_->obj);
        impl_->obj = nullptr;
        impl_->state = EbpfMonitorState::Fallback;
        return false;
    }

    struct bpf_program* prog = bpf_object__find_program_by_name(impl_->obj, "trace_kfree_skb");
    if (!prog) {
        LOG_WARNING(LogModule::NETWORK, "SkbDropMonitor: program trace_kfree_skb not found");
        bpf_object__close(impl_->obj);
        impl_->obj = nullptr;
        impl_->state = EbpfMonitorState::Fallback;
        return false;
    }

    impl_->link = bpf_program__attach(prog);
    if (!impl_->link) {
        LOG_WARNING(LogModule::NETWORK, "SkbDropMonitor: failed to attach tracepoint trace_kfree_skb");
        bpf_object__close(impl_->obj);
        impl_->obj = nullptr;
        impl_->state = EbpfMonitorState::Fallback;
        return false;
    }

    impl_->mapFd = bpf_object__find_map_fd_by_name(impl_->obj, "drop_stats_map");
    if (impl_->mapFd < 0) {
        LOG_WARNING(LogModule::NETWORK, "SkbDropMonitor: map drop_stats_map not found");
        bpf_link__destroy(impl_->link);
        impl_->link = nullptr;
        bpf_object__close(impl_->obj);
        impl_->obj = nullptr;
        impl_->state = EbpfMonitorState::Fallback;
        return false;
    }

    impl_->metricsData.attachedProbes = 1;
    impl_->state = EbpfMonitorState::Attached;
    LOG_INFO(LogModule::NETWORK, "SkbDropMonitor: eBPF attached successfully");
    return true;
#else
    // x86 非 eBPF 环境下作为 fallback 状态正常运行
    impl_->state = EbpfMonitorState::Fallback;
    return false;
#endif
}

void SkbDropMonitor::stop() {
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
    impl_->mapFd = -1;
#endif
    impl_->state = EbpfMonitorState::Stopped;
}

DropStatsSummary SkbDropMonitor::getDropStats() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    DropStatsSummary summary;

#if HAVE_LIBBPF
    if (impl_->state != EbpfMonitorState::Attached || impl_->mapFd < 0) {
        return impl_->cachedSummary;
    }

    struct drop_key {
        uint32_t protocol;
        uint32_t reason;
    } key = {}, next_key = {};

    struct drop_stat {
        uint64_t count;
        uint64_t last_timestamp_ns;
    } value = {};

    impl_->metricsData.mapReads++;
    bool has_key = (bpf_map_get_next_key(impl_->mapFd, nullptr, &next_key) == 0);
    while (has_key) {
        key = next_key;
        if (bpf_map_lookup_elem(impl_->mapFd, &key, &value) == 0) {
            DropReasonItem item;
            item.reasonCode = key.reason;
            item.reasonName = skbDropReasonToString(key.reason);
            item.humanDesc = skbDropReasonToDescription(key.reason);
            item.count = value.count;
            item.lastTimestampNs = value.last_timestamp_ns;

            if (key.protocol == 0x0800 || key.protocol == 6) {
                item.protocol = "TCP/IPv4";
            } else if (key.protocol == 17) {
                item.protocol = "UDP";
            } else if (key.protocol == 1) {
                item.protocol = "ICMP";
            } else {
                item.protocol = "Other";
            }

            summary.totalDrops += value.count;
            summary.topReasons.push_back(item);
        }
        has_key = (bpf_map_get_next_key(impl_->mapFd, &key, &next_key) == 0);
    }

    std::sort(summary.topReasons.begin(), summary.topReasons.end(),
              [](const DropReasonItem& a, const DropReasonItem& b) {
                  return a.count > b.count;
              });

    impl_->cachedSummary = summary;
    impl_->metricsData.samples++;
    impl_->consecutiveErrors = 0;
#endif

    return summary;
}

EbpfMonitorState SkbDropMonitor::commonState() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->state;
}

bool SkbDropMonitor::isAvailable() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->state == EbpfMonitorState::Attached;
}

EbpfMonitorHealth SkbDropMonitor::health() const {
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

EbpfMonitorMetrics SkbDropMonitor::metrics() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->metricsData;
}

void SkbDropMonitor::resetMetrics() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->metricsData = EbpfMonitorMetrics();
}

}  // namespace weaknet_dbus

// process_net_profiler.cpp
// 进程级网络画像 - 用户态实现
// 从 BPF Map 读取每个进程的网络流量统计

#include "process_net_profiler.hpp"
#include "logger.hpp"

#include <cstring>
#include <algorithm>

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

struct process_net_stats {
    char  comm[16];
    __u64 tx_bytes;
    __u64 tx_packets;
    __u64 retrans_count;
};

// ---- 实现 ----

struct ProcessNetProfiler::Impl {
    int process_stats_fd = -1;
    struct bpf_object *obj = nullptr;
    struct bpf_link *link_retrans = nullptr;
    struct bpf_link *link_xmit = nullptr;
    struct bpf_link *link_udp = nullptr;
};

ProcessNetProfiler::ProcessNetProfiler()
    : impl_(std::make_unique<Impl>()) {}

ProcessNetProfiler::~ProcessNetProfiler() {
    stop();
}

bool ProcessNetProfiler::init(const std::string& bpfObjPath) {
#if !HAVE_LIBBPF
    LOG_INFO(LogModule::NETWORK, "ProcessNetProfiler: BPF not available (no libbpf)");
    available_ = false;
    initialized_ = true;
    return false;
#else
    LOG_INFO(LogModule::NETWORK, "ProcessNetProfiler: loading BPF object from " << bpfObjPath);

    LIBBPF_OPTS(bpf_object_open_opts, opts);
    struct bpf_object *obj = bpf_object__open_file(bpfObjPath.c_str(), &opts);
    if (!obj) {
        LOG_ERROR(LogModule::NETWORK, "ProcessNetProfiler: failed to open BPF object: " << bpfObjPath);
        available_ = false;
        initialized_ = true;
        return false;
    }

    if (bpf_object__load(obj) != 0) {
        LOG_ERROR(LogModule::NETWORK, "ProcessNetProfiler: failed to load BPF object");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        return false;
    }

    impl_->process_stats_fd = bpf_object__find_map_fd_by_name(obj, "process_stats");
    if (impl_->process_stats_fd < 0) {
        LOG_ERROR(LogModule::NETWORK, "ProcessNetProfiler: process_stats map not found");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        return false;
    }

    // attach 3 个探针：tcp_retransmit_skb + ip_queue_xmit + udp_sendmsg
    struct bpf_program *retrans_prog = bpf_object__find_program_by_name(obj, "trace_tcp_retransmit");
    struct bpf_program *xmit_prog = bpf_object__find_program_by_name(obj, "tcp_transmit_entry");
    struct bpf_program *udp_prog = bpf_object__find_program_by_name(obj, "udp_send_entry");

    impl_->link_retrans = retrans_prog ? bpf_program__attach(retrans_prog) : nullptr;
    if (impl_->link_retrans && libbpf_get_error(impl_->link_retrans)) {
        LOG_ERROR(LogModule::NETWORK, "ProcessNetProfiler: attach tcp_retransmit_skb failed");
        impl_->link_retrans = nullptr;
    }
    impl_->link_xmit = xmit_prog ? bpf_program__attach(xmit_prog) : nullptr;
    if (impl_->link_xmit && libbpf_get_error(impl_->link_xmit)) {
        LOG_ERROR(LogModule::NETWORK, "ProcessNetProfiler: attach ip_queue_xmit failed");
        impl_->link_xmit = nullptr;
    }
    impl_->link_udp = udp_prog ? bpf_program__attach(udp_prog) : nullptr;
    if (impl_->link_udp && libbpf_get_error(impl_->link_udp)) {
        LOG_ERROR(LogModule::NETWORK, "ProcessNetProfiler: attach udp_sendmsg failed");
        impl_->link_udp = nullptr;
    }

    if (!impl_->link_retrans && !impl_->link_xmit && !impl_->link_udp) {
        LOG_ERROR(LogModule::NETWORK, "ProcessNetProfiler: all probes failed to attach");
        bpf_object__close(obj);
        impl_->link_retrans = impl_->link_xmit = impl_->link_udp = nullptr;
        available_ = false;
        initialized_ = true;
        return false;
    }

    impl_->obj = obj;
    available_ = true;
    initialized_ = true;

    LOG_INFO(LogModule::NETWORK, "ProcessNetProfiler: initialized successfully");
    return true;
#endif
}

void ProcessNetProfiler::stop() {
#if HAVE_LIBBPF
    if (impl_->link_retrans) { bpf_link__destroy(impl_->link_retrans); impl_->link_retrans = nullptr; }
    if (impl_->link_xmit) { bpf_link__destroy(impl_->link_xmit); impl_->link_xmit = nullptr; }
    if (impl_->link_udp) { bpf_link__destroy(impl_->link_udp); impl_->link_udp = nullptr; }
#endif
    if (impl_->obj) {
        bpf_object__close(impl_->obj);
        impl_->obj = nullptr;
        LOG_INFO(LogModule::NETWORK, "ProcessNetProfiler: stopped");
    }
    impl_->process_stats_fd = -1;
    available_ = false;
}

std::vector<ProcessNetInfo> ProcessNetProfiler::getProcesses() {
    std::vector<ProcessNetInfo> result;
#if HAVE_LIBBPF
    if (!available_ || impl_->process_stats_fd < 0)
        return result;

    __u32 cur_key = 0, next_key = 0;
    while (bpf_map_get_next_key(impl_->process_stats_fd, &cur_key, &next_key) == 0) {
        process_net_stats stats = {};
        if (bpf_map_lookup_elem(impl_->process_stats_fd, &next_key, &stats) == 0) {
            ProcessNetInfo info;
            info.pid = next_key;
            info.comm = stats.comm;
            info.txBytes = stats.tx_bytes;
            info.txPackets = stats.tx_packets;
            info.retransCount = stats.retrans_count;
            if (!info.comm.empty()) result.push_back(info);
        }
        cur_key = next_key;
    }
#endif
    return result;
}

std::vector<ProcessNetInfo> ProcessNetProfiler::getTopBandwidth(size_t topN) {
    auto all = getProcesses();
    std::sort(all.begin(), all.end(),
        [](const ProcessNetInfo& a, const ProcessNetInfo& b) {
            return a.txBytes > b.txBytes;
        });
    if (all.size() > topN) all.resize(topN);
    return all;
}

std::vector<ProcessNetInfo> ProcessNetProfiler::getTopRetransmit(size_t topN) {
    auto all = getProcesses();
    std::sort(all.begin(), all.end(),
        [](const ProcessNetInfo& a, const ProcessNetInfo& b) {
            return a.retransCount > b.retransCount;
        });
    if (all.size() > topN) all.resize(topN);
    return all;
}

bool ProcessNetProfiler::getProcess(uint32_t pid, ProcessNetInfo* out) {
#if HAVE_LIBBPF
    if (!available_ || impl_->process_stats_fd < 0 || !out)
        return false;
    process_net_stats stats = {};
    if (bpf_map_lookup_elem(impl_->process_stats_fd, &pid, &stats) != 0)
        return false;
    out->pid = pid;
    out->comm = stats.comm;
    out->txBytes = stats.tx_bytes;
    out->txPackets = stats.tx_packets;
    out->retransCount = stats.retrans_count;
    return true;
#else
    (void)pid; (void)out;
    return false;
#endif
}

}  // namespace weaknet_dbus
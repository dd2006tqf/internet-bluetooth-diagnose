// wifi_packet_loss_monitor.cpp
// Wi-Fi/网卡收发丢包归因监控器 - 用户态实现
// 从 BPF Map 读取收发/丢弃/重传统计，区分发送/接收丢包

#include "wifi_packet_loss_monitor.hpp"
#include "logger.hpp"

#include <cstring>
#include <cstdio>
#include <chrono>

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

struct iface_packet_stats {
    __u64 rx_pkts;
    __u64 rx_bytes;
    __u64 tx_pkts;
    __u64 tx_bytes;
    __u64 tx_drops;
    __u64 tx_retries;
};

#if HAVE_LIBBPF
// 辅助：按名字找到 BPF 程序并 attach，返回 link 或 nullptr
static struct bpf_link* attachProgram(struct bpf_object* obj, const char* progName) {
    struct bpf_program* prog = bpf_object__find_program_by_name(obj, progName);
    if (!prog) {
        LOG_ERROR(LogModule::NETWORK, "WifiPacketLossMonitor: BPF program " << progName << " not found");
        return nullptr;
    }
    struct bpf_link* link = bpf_program__attach(prog);
    if (libbpf_get_error(link)) {
        LOG_ERROR(LogModule::NETWORK, "WifiPacketLossMonitor: attach " << progName << " failed err=" << libbpf_get_error(link));
        return nullptr;
    }
    LOG_INFO(LogModule::NETWORK, "WifiPacketLossMonitor: " << progName << " attached");
    return link;
}
#endif

// ---- 实现 ----

struct WifiPacketLossMonitor::Impl {
    int packet_stats_fd = -1;
    struct bpf_object *obj = nullptr;
    struct bpf_link *link_rx = nullptr;
    struct bpf_link *link_tx_queue = nullptr;
    struct bpf_link *link_tx_xmit = nullptr;
};

WifiPacketLossMonitor::WifiPacketLossMonitor()
    : impl_(std::make_unique<Impl>()) {}

WifiPacketLossMonitor::~WifiPacketLossMonitor() {
    stop();
}

bool WifiPacketLossMonitor::init(const std::string& bpfObjPath) {
    stateSupport_.setState(EbpfMonitorState::Initializing, false, "loading BPF object");
    stateSupport_.setState(EbpfMonitorState::Initializing, false, "loading BPF object");
#if !HAVE_LIBBPF
    LOG_INFO(LogModule::NETWORK, "WifiPacketLossMonitor: BPF not available (no libbpf)");
    available_ = false;
    initialized_ = true;
    return false;
#else
    LOG_INFO(LogModule::NETWORK, "WifiPacketLossMonitor: loading BPF object from " << bpfObjPath);

    LIBBPF_OPTS(bpf_object_open_opts, opts);
    struct bpf_object *obj = bpf_object__open_file(bpfObjPath.c_str(), &opts);
    if (!obj) {
        LOG_ERROR(LogModule::NETWORK, "WifiPacketLossMonitor: failed to open BPF object: " << bpfObjPath);
        available_ = false;
        initialized_ = true;
        return false;
    }

    if (bpf_object__load(obj) != 0) {
        LOG_ERROR(LogModule::NETWORK, "WifiPacketLossMonitor: failed to load BPF object");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        return false;
    }

    impl_->packet_stats_fd = bpf_object__find_map_fd_by_name(obj, "packet_stats");
    if (impl_->packet_stats_fd < 0) {
        LOG_ERROR(LogModule::NETWORK, "WifiPacketLossMonitor: packet_stats map not found");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        return false;
    }

    // attach 3 个 tracepoint
    impl_->link_rx = attachProgram(obj, "trace_net_rx");
    impl_->link_tx_queue = attachProgram(obj, "trace_net_tx_queue");
    impl_->link_tx_xmit = attachProgram(obj, "trace_net_tx_xmit");

    if (!impl_->link_rx && !impl_->link_tx_queue && !impl_->link_tx_xmit) {
        LOG_ERROR(LogModule::NETWORK, "WifiPacketLossMonitor: all tracepoints failed to attach");
        bpf_object__close(obj);
        impl_->link_rx = impl_->link_tx_queue = impl_->link_tx_xmit = nullptr;
        available_ = false;
        initialized_ = true;
        return false;
    }

    impl_->obj = obj;
    available_ = true;
    initialized_ = true;

    LOG_INFO(LogModule::NETWORK, "WifiPacketLossMonitor: initialized successfully");
    stateSupport_.setState(EbpfMonitorState::Attached, true, "BPF probes attached");
    stateSupport_.recordProbeAttached();
    stateSupport_.recordProbeAttached();
    stateSupport_.recordProbeAttached();
    return true;
#endif
}

void WifiPacketLossMonitor::stop() {
    stateSupport_.setState(EbpfMonitorState::Stopped, false, "stopped");
#if HAVE_LIBBPF
    if (impl_->link_rx) { bpf_link__destroy(impl_->link_rx); impl_->link_rx = nullptr; }
    if (impl_->link_tx_queue) { bpf_link__destroy(impl_->link_tx_queue); impl_->link_tx_queue = nullptr; }
    if (impl_->link_tx_xmit) { bpf_link__destroy(impl_->link_tx_xmit); impl_->link_tx_xmit = nullptr; }
#endif
    if (impl_->obj) {
        bpf_object__close(impl_->obj);
        impl_->obj = nullptr;
        LOG_INFO(LogModule::NETWORK, "WifiPacketLossMonitor: stopped");
    }
    impl_->packet_stats_fd = -1;
    available_ = false;
}

std::map<uint32_t, IfacePacketStats> WifiPacketLossMonitor::getStats() {
    std::map<uint32_t, IfacePacketStats> result;
#if HAVE_LIBBPF
    if (impl_->packet_stats_fd < 0) {
        stateSupport_.recordReadFailure("packet_stats map unavailable");
        return result;
    }

    auto started = std::chrono::steady_clock::now();
    __u32 cur_key = 0, next_key = 0;
    while (bpf_map_get_next_key(impl_->packet_stats_fd, &cur_key, &next_key) == 0) {
        iface_packet_stats stats = {};
        if (bpf_map_lookup_elem(impl_->packet_stats_fd, &next_key, &stats) == 0) {
            IfacePacketStats out;
            out.ifindex = next_key;
            out.rxPkts = stats.rx_pkts;
            out.rxBytes = stats.rx_bytes;
            out.txPkts = stats.tx_pkts;
            out.txBytes = stats.tx_bytes;
            out.txDrops = stats.tx_drops;
            out.txRetries = stats.tx_retries;
            result[next_key] = out;
        }
        cur_key = next_key;
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
    stateSupport_.recordReadSuccess(static_cast<uint64_t>(elapsed), !result.empty());
#endif
    return result;
}

bool WifiPacketLossMonitor::getStats(uint32_t ifindex, IfacePacketStats* out) {
#if HAVE_LIBBPF
    if (!available_ || impl_->packet_stats_fd < 0 || !out)
        return false;
    iface_packet_stats stats = {};
    if (bpf_map_lookup_elem(impl_->packet_stats_fd, &ifindex, &stats) != 0)
        return false;
    out->ifindex = ifindex;
    out->rxPkts = stats.rx_pkts;
    out->rxBytes = stats.rx_bytes;
    out->txPkts = stats.tx_pkts;
    out->txBytes = stats.tx_bytes;
    out->txDrops = stats.tx_drops;
    out->txRetries = stats.tx_retries;
    return true;
#else
    (void)ifindex; (void)out;
    return false;
#endif
}

WifiPacketLossMonitor::LossAttribution WifiPacketLossMonitor::analyze(uint32_t ifindex, const std::string& ifaceName) {
    LossAttribution result;
    result.ifaceName = ifaceName;
    result.rxLossRate = 0.0;
    result.txLossRate = 0.0;
    result.txRetries = 0;
    result.analysis = "无数据";

    IfacePacketStats stats;
    if (!getStats(ifindex, &stats)) {
        result.analysis = "接口 " + ifaceName + " 无收发统计";
        return result;
    }

    // 估算接收丢包率：接收包数 vs 发送包数（近似，若 rx 远低于 tx 且字节相近可能接收丢包）
    // 更精确的接收丢包率需要对比驱动层 rx_dropped 计数，这里用发送/接收包比例近似
    double txLoss = stats.txLossRate();
    result.txRetries = stats.txRetries;
    result.txLossRate = txLoss;

    // 接收丢包率近似：接收包数显著少于发送包数，且都不是本地回环
    // 简化：用 (tx_pkts - rx_pkts) / tx_pkts 作为接收丢包率的粗略估计
    if (stats.txPkts > 0 && stats.rxPkts > 0) {
        double ratio = static_cast<double>(stats.rxPkts) / stats.txPkts;
        // TCP 回显场景 rx ≈ tx；纯下行 rx >> tx；纯上行 rx << tx
        // 无法单凭此判断接收丢包，这里仅当 txDrops 高时判断发送问题
        result.rxLossRate = 0.0; // 接收丢包率需要网卡驱动 rx_dropped 数据，此处不虚报
    }

    // 归因分析
    bool txProblem = txLoss > 1.0 || stats.txRetries > 100;
    bool rxProblem = false; // 接收丢包率当前无法精确测量，保守判断

    if (txProblem && rxProblem) {
        result.analysis = "收发均异常，链路整体拥塞或信号差";
    } else if (txProblem) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "主要发送丢包: tx丢包率=%.2f%%, 重试=%llu次, 判断本机到AP上行问题",
            txLoss, (unsigned long long)stats.txRetries);
        result.analysis = buf;
    } else if (rxProblem) {
        result.analysis = "主要接收丢包, 判断AP到本机下行问题";
    } else {
        result.analysis = "连接正常，无显著丢包";
    }

    LOG_INFO(LogModule::NETWORK, "WifiPacketLoss: iface=" << ifaceName
        << " txPkts=" << stats.txPkts << " rxPkts=" << stats.rxPkts
        << " txDrops=" << stats.txDrops << " txRetries=" << stats.txRetries
        << " -> " << result.analysis);
    return result;
}

}  // namespace weaknet_dbus
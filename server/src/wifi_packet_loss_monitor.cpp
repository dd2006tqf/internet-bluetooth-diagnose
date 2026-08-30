/**
 * @file wifi_packet_loss_monitor.cpp
 * @brief Wi-Fi/网卡收发丢包归因监控器 - 用户态实现
 *
 * 监控指标：
 *   - 每接口收发包数 / 收发字节数 / 发送丢弃数 / 发送重试数
 *   - 发送丢包率：txDrops / (txPkts + txDrops) × 100%
 *   - 丢包归因分析：区分"主要发送丢包" / "收发均异常" / "正常"
 *
 * 数据源（eBPF）：
 *   - BPF 对象文件名：wifi_packet_loss_monitor.bpf.o
 *   - 探针类型：tracepoint（内核静态跟踪点，比 kprobe 更稳定）
 *     - tracepoint/net/netif_receive_skb      → trace_net_rx（捕获接收包，统计 rxPkts/rxBytes）
 *     - tracepoint/net/net_dev_queue_xmit     → trace_net_tx_queue（入队时统计 txPkts/txBytes）
 *     - tracepoint/net/netif_xmit_failed      → trace_net_tx_xmit（发送失败时统计 txDrops/txRetries）
 *   - 数据通道：BPF Map（Hash 类型）
 *     - packet_stats：每接口收发统计 Map（键 = ifindex，值 = iface_packet_stats）
 *   - 用户态通过 bpf_map_get_next_key + bpf_map_lookup_elem 遍历 Map
 *
 * 线程模型：
 *   - 本类本身不创建独立线程，由外部周期性调用 getStats() / analyze()
 *   - 无锁：eBPF Map 读取对并发安全
 *
 * 丢包归因策略：
 *   - 发送丢包率 > 1% 或 txRetries > 100 → 判断本机到 AP 的上行问题
 *   - 接收丢包率（rxLossRate）当前无法精确测量，保守判断为 0
 *   - 两者同时高 → 判断链路整体拥塞或信号差
 */

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

// ---- 数据结构映射（与 BPF 端 C 结构体一一对应） ----

/**
 * @brief 接口收发统计结构（与 BPF 端 packet_stats Map 的 value 一致）
 */
struct iface_packet_stats {
    __u64 rx_pkts;    // 接收包总数
    __u64 rx_bytes;   // 接收字节总数
    __u64 tx_pkts;    // 发送包总数
    __u64 tx_bytes;   // 发送字节总数
    __u64 tx_drops;   // 发送丢弃包数（驱动层丢包）
    __u64 tx_retries; // 发送重试次数（Wi-Fi 重传计数器）
};

#if HAVE_LIBBPF
/**
 * @brief 辅助函数：按程序名查找并 attach BPF 程序
 *
 * 使用 bpf_program__attach 自动识别探针类型（kprobe/tracepoint），
 * 简化 init 中重复的 attach 逻辑。
 *
 * @param obj     BPF 对象指针
 * @param progName BPF 程序名（必须与 .bpf.c 中的 SEC() section 名对应）
 * @return bpf_link* 成功时返回 BPF link，失败时返回 nullptr
 */
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

/**
 * @brief Pimpl 实现结构体，持有 libbpf 句柄
 */
struct WifiPacketLossMonitor::Impl {
    int packet_stats_fd = -1;         ///< packet_stats Map fd（每接口收发统计）
    struct bpf_object *obj = nullptr;  ///< BPF 对象实例
    struct bpf_link *link_rx = nullptr;        ///< tracepoint/net/netif_receive_skb BPF link（接收侧）
    struct bpf_link *link_tx_queue = nullptr;  ///< tracepoint/net/net_dev_queue_xmit BPF link（发送入队）
    struct bpf_link *link_tx_xmit = nullptr;   ///< tracepoint/net/netif_xmit_failed BPF link（发送失败）
};

WifiPacketLossMonitor::WifiPacketLossMonitor()
    : impl_(std::make_unique<Impl>()) {}

WifiPacketLossMonitor::~WifiPacketLossMonitor() {
    stop();
}

/**
 * @brief 初始化 Wi-Fi 丢包监控器：加载 BPF 对象并挂载 tracepoint
 *
 * 初始化流程：
 *   1. 打开并加载 BPF 对象文件（wifi_packet_loss_monitor.bpf.o）
 *   2. 查找 packet_stats Map 的 fd
 *   3. 通过 attachProgram 辅助函数挂载 3 个 tracepoint 探针
 *      - trace_net_rx（接收侧）
 *      - trace_net_tx_queue（发送入队）
 *      - trace_net_tx_xmit（发送失败）
 *
 * @param bpfObjPath BPF 对象文件路径
 * @return true  初始化成功（至少一个 tracepoint 挂载成功）
 *         false 初始化失败（所有 tracepoint 均挂在失败或 libbpf 不可用）
 */
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

    // attach 3 个 tracepoint：
    //   trace_net_rx      → tracepoint/net/netif_receive_skb（接收侧）
    //   trace_net_tx_queue→ tracepoint/net/net_dev_queue_xmit（发送入队）
    //   trace_net_tx_xmit → tracepoint/net/netif_xmit_failed（发送失败）
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

/**
 * @brief 停止 Wi-Fi 丢包监控器：销毁所有 BPF link 和 BPF 对象
 *
 * 释放 rx/tx_queue/tx_xmit 三个 tracepoint 的 link 句柄
 */
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

/**
 * @brief 获取所有接口的收发统计
 *
 * 遍历 packet_stats Map 的所有条目，将内核态统计转为用户态 IfacePacketStats 结构。
 *
 * @return 以 ifindex 为键的收发统计 Map
 */
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

/**
 * @brief 获取指定接口的收发统计
 *
 * @param ifindex 网卡接口索引（IP 层 ifindex，如 eth0=2, wlan0=3）
 * @param out     输出参数，成功时写入 IfacePacketStats
 * @return true  查询成功
 *         false Map 不可用、ifindex 不存在或 out 为 nullptr
 */
bool WifiPacketLossMonitor::getStats(uint32_t ifindex, IfacePacketStats* out) {
#if HAVE_LIBBPF
    auto started = std::chrono::steady_clock::now();
    if (impl_->packet_stats_fd < 0 || !out) {
        stateSupport_.recordReadFailure("packet_stats map unavailable or invalid output");
        return false;
    }
    iface_packet_stats stats = {};
    if (bpf_map_lookup_elem(impl_->packet_stats_fd, &ifindex, &stats) != 0) {
        stateSupport_.recordReadFailure("packet_stats map lookup failed");
        return false;
    }
    out->ifindex = ifindex;
    out->rxPkts = stats.rx_pkts;
    out->rxBytes = stats.rx_bytes;
    out->txPkts = stats.tx_pkts;
    out->txBytes = stats.tx_bytes;
    out->txDrops = stats.tx_drops;
    out->txRetries = stats.tx_retries;
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
    stateSupport_.recordReadSuccess(static_cast<uint64_t>(elapsed));
    return true;
#else
    (void)ifindex; (void)out;
    return false;
#endif
}

/**
 * @brief 分析指定接口的丢包归因
 *
 * 读取该接口的收发统计，计算发送丢包率（txDrops/(txPkts+txDrops)×100%），
 * 并根据预定义阈值判断是发送侧问题还是整体正常。
 *
 * 归因阈值：
 *   - 发送丢包率 > 1% 或 txRetries > 100 → 主要发送丢包（上行问题）
 *   - 发送丢包率 > 1% 且接收也异常（当前 rxLossRate 不可测） → 收发均异常
 *   - 否则 → 连接正常
 *
 * @param ifindex  网卡接口索引
 * @param ifaceName 接口名（仅用于日志/返回描述）
 * @return LossAttribution 归因分析结果，含 rxLossRate/txLossRate/analysis 字段
 */
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

    // 计算发送丢包率：txDrops / (txPkts + txDrops) × 100%
    // 这是发送侧最直接的丢包度量（驱动层统计）
    double txLoss = stats.txLossRate();
    result.txRetries = stats.txRetries;
    result.txLossRate = txLoss;

    // 接收丢包率近似：接收包数显著少于发送包数，且都不是本地回环
    // 简化策略：由于没有驱动层 rx_dropped 精确计数，此处不估计接收丢包率
    // 原因：TCP 回显场景 rx ≈ tx；纯下行 rx >> tx；纯上行 rx << tx
    // 无法单凭此判断接收丢包，这里仅当 txDrops 高时判断发送问题
    if (stats.txPkts > 0 && stats.rxPkts > 0) {
        result.rxLossRate = 0.0; // 接收丢包率需要网卡驱动 rx_dropped 数据，此处不虚报
    }

    // 归因分析
    bool txProblem = txLoss > 1.0 || stats.txRetries > 100;   // 发送丢包率 > 1% 或重试次数 > 100
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

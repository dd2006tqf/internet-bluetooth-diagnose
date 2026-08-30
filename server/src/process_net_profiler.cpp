/**
 * @file process_net_profiler.cpp
 * @brief 进程级网络画像 - 用户态实现
 *
 * 监控指标：
 *   - 每进程发送字节数 / 发送包数 / TCP 重传次数
 *   - 顶 N 带宽占用进程（getTopBandwidth）
 *   - 顶 N 重传最多进程（getTopRetransmit）
 *
 * 数据源（eBPF）：
 *   - BPF 对象文件名：flow_rate.bpf.o（与 NetTrafficAnalyzer 共享，避免重复加载导致 kprobe 冲突）
 *   - 探针类型：kprobe（内核函数入口）
 *     - kprobe/tcp_retransmit_skb    → trace_tcp_retransmit（捕获 TCP 重传事件，按 pid 累加重传计数）
 *     - kprobe/ip_queue_xmit          → tcp_transmit_entry（捕获 IP 层发包，按 pid 累计发送字节/包数）
 *     - kprobe/udp_sendmsg            → udp_send_entry（捕获 UDP 发包，按 pid 累计）
 *   - 数据通道：BPF Map（Hash 类型）
 *     - process_stats：进程级网络统计 Map（键 = pid（tgid），值 = process_net_stats{comm, tx_bytes, tx_packets, retrans_count}）
 *   - 注意：本类优先从 NetTrafficAnalyzer 共享 process_stats Map fd，
 *     因为 flow_rate.bpf.o 可能已由 TrafficAnalyzer 加载并 attach 过 kprobe，
 *     重复加载会导致 kprobe attach 失败。仅在 TrafficAnalyzer 不可用时独立加载。
 *
 * 线程模型：
 *   - 本类本身不创建独立线程，由外部周期性调用 getProcesses() / getTopBandwidth()
 *   - 无锁：eBPF Map 读取对并发安全
 */

#include "process_net_profiler.hpp"
#include "logger.hpp"
#include "net_traffic.h"

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

// ---- 数据结构映射（与 BPF 端 C 结构体一一对应） ----

/**
 * @brief 进程级网络统计结构（与 BPF 端 process_stats Map 的 value 一致）
 */
struct process_net_stats {
    char  comm[16];       // 进程名（task_struct->comm，最多 15 字符 + '\0'）
    __u64 tx_bytes;       // 累计发送字节数（TCP + UDP）
    __u64 tx_packets;     // 累计发送包数
    __u64 retrans_count;  // 累计 TCP 重传次数
};

// ---- 实现 ----

/**
 * @brief Pimpl 实现结构体
 *
 * owns_obj 标志区分两种持有模式：
 *   - true：独立加载了 BPF 对象，stop() 时需 close
 *   - false：共享 TrafficAnalyzer 的 process_stats fd，stop() 时不 close
 */
struct ProcessNetProfiler::Impl {
    int process_stats_fd = -1;         ///< process_stats Map fd
    struct bpf_object *obj = nullptr;  ///< BPF 对象实例（独立加载时持有）
    struct bpf_link *link_retrans = nullptr;  ///< kprobe/tcp_retransmit_skb BPF link
    struct bpf_link *link_xmit = nullptr;     ///< kprobe/ip_queue_xmit BPF link
    struct bpf_link *link_udp = nullptr;       ///< kprobe/udp_sendmsg BPF link
    bool owns_obj = false;  ///< true=自己加载 BPF 对象，false=共享 TrafficAnalyzer 的 map fd
};

ProcessNetProfiler::ProcessNetProfiler()
    : impl_(std::make_unique<Impl>()) {}

ProcessNetProfiler::~ProcessNetProfiler() {
    stop();
}

/**
 * @brief 初始化进程级网络画像器
 *
 * 初始化优先策略：尝试与 NetTrafficAnalyzer 共享 process_stats Map fd，
 * 避免重复加载 flow_rate.bpf.o 导致 kprobe 冲突。
 * 若 TrafficAnalyzer 不可用，则独立加载 BPF 对象。
 *
 * 独立加载时挂载 3 个 kprobe：
 *   - kprobe/tcp_retransmit_skb → trace_tcp_retransmit（重传计数）
 *   - kprobe/ip_queue_xmit      → tcp_transmit_entry（TCP 发包字节/包数）
 *   - kprobe/udp_sendmsg        → udp_send_entry（UDP 发包字节/包数）
 *
 * @param bpfObjPath BPF 对象文件路径（通常为 "build/flow_rate.bpf.o"）
 * @return true  初始化成功
 *         false 初始化失败（libbpf 不可用、共享 fd 不可用、独立加载失败）
 */
bool ProcessNetProfiler::init(const std::string& bpfObjPath) {
    stateSupport_.setState(EbpfMonitorState::Initializing, false, "loading BPF object");
#if !HAVE_LIBBPF
    LOG_INFO(LogModule::NETWORK, "ProcessNetProfiler: BPF not available (no libbpf)");
    available_ = false;
    initialized_ = true;
    return false;
#else
    // 优先从 TrafficAnalyzer 共享 map fd（避免重复加载 flow_rate.bpf.o 导致 kprobe 冲突）
    // TrafficAnalyzer 已加载 flow_rate.bpf.o 并 attach 了所有 kprobe，
    // 再次加载会导致 kprobe attach 失败，process_stats map 永远为空。
    auto analyzer = NetTrafficAnalyzer::getInstance();
    if (analyzer->initForInterface("") && analyzer->getProcessStatsFd() >= 0) {
        impl_->process_stats_fd = analyzer->getProcessStatsFd();
        impl_->owns_obj = false;
        available_ = true;
        initialized_ = true;
        LOG_INFO(LogModule::NETWORK, "ProcessNetProfiler: using shared process_stats fd from TrafficAnalyzer");
        return true;
    }

    // 回退：独立加载 BPF（TrafficAnalyzer 不可用时）
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
    impl_->owns_obj = true;
    available_ = true;
    initialized_ = true;

    LOG_INFO(LogModule::NETWORK, "ProcessNetProfiler: initialized successfully");
    stateSupport_.setState(EbpfMonitorState::Attached, true, "BPF probes attached");
    stateSupport_.recordProbeAttached();
    stateSupport_.recordProbeAttached();
    stateSupport_.recordProbeAttached();
    return true;
#endif
}

/**
 * @brief 停止进程级网络画像器
 *
 * 注意：仅当 owns_obj=true（自己加载了 BPF）时才 close BPF 对象，
 * 共享 TrafficAnalyzer 的 fd 不关闭（否则会导致 TrafficAnalyzer 崩溃）
 */
void ProcessNetProfiler::stop() {
    stateSupport_.setState(EbpfMonitorState::Stopped, false, "stopped");
#if HAVE_LIBBPF
    if (impl_->link_retrans) { bpf_link__destroy(impl_->link_retrans); impl_->link_retrans = nullptr; }
    if (impl_->link_xmit) { bpf_link__destroy(impl_->link_xmit); impl_->link_xmit = nullptr; }
    if (impl_->link_udp) { bpf_link__destroy(impl_->link_udp); impl_->link_udp = nullptr; }
#endif
    // 仅在自己加载了 BPF 对象时才 close（共享 TrafficAnalyzer 的 fd 不关闭）
    if (impl_->owns_obj && impl_->obj) {
        bpf_object__close(impl_->obj);
        impl_->obj = nullptr;
        LOG_INFO(LogModule::NETWORK, "ProcessNetProfiler: stopped");
    }
    impl_->process_stats_fd = -1;
    available_ = false;
}

/**
 * @brief 获取所有有网络活动的进程列表
 *
 * 遍历 process_stats Map，将内核态统计转为用户态 ProcessNetInfo 结构。
 * 仅返回 comm 非空的条目（过滤内核线程等匿名进程）。
 *
 * @return 进程网络信息列表
 */
std::vector<ProcessNetInfo> ProcessNetProfiler::getProcesses() {
    std::vector<ProcessNetInfo> result;
#if HAVE_LIBBPF
    if (impl_->process_stats_fd < 0) {
        stateSupport_.recordReadFailure("process_stats map unavailable");
        return result;
    }

    auto started = std::chrono::steady_clock::now();
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
            if (!info.comm.empty()) result.push_back(info);  // 过滤匿名内核线程
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
 * @brief 获取顶 N 带宽占用进程（按 txBytes 降序）
 * @param topN 返回的最大进程数
 * @return 按发送字节数降序排列的进程列表
 */
std::vector<ProcessNetInfo> ProcessNetProfiler::getTopBandwidth(size_t topN) {
    auto all = getProcesses();
    std::sort(all.begin(), all.end(),
        [](const ProcessNetInfo& a, const ProcessNetInfo& b) {
            return a.txBytes > b.txBytes;  // 按发送字节数降序
        });
    if (all.size() > topN) all.resize(topN);
    return all;
}

/**
 * @brief 获取顶 N 重传最多进程（按 retransCount 降序）
 * @param topN 返回的最大进程数
 * @return 按 TCP 重传次数降序排列的进程列表
 */
std::vector<ProcessNetInfo> ProcessNetProfiler::getTopRetransmit(size_t topN) {
    auto all = getProcesses();
    std::sort(all.begin(), all.end(),
        [](const ProcessNetInfo& a, const ProcessNetInfo& b) {
            return a.retransCount > b.retransCount;  // 按重传次数降序
        });
    if (all.size() > topN) all.resize(topN);
    return all;
}

/**
 * @brief 查询指定 pid 的网络统计
 * @param pid 进程 ID（tgid）
 * @param out 输出参数，成功时写入 ProcessNetInfo
 * @return true 查询成功；false Map 不可用或 pid 不存在
 */
bool ProcessNetProfiler::getProcess(uint32_t pid, ProcessNetInfo* out) {
#if HAVE_LIBBPF
    auto started = std::chrono::steady_clock::now();
    if (impl_->process_stats_fd < 0 || !out) {
        stateSupport_.recordReadFailure("process_stats map unavailable or invalid output");
        return false;
    }
    process_net_stats stats = {};
    if (bpf_map_lookup_elem(impl_->process_stats_fd, &pid, &stats) != 0) {
        stateSupport_.recordReadFailure("process_stats map lookup failed");
        return false;
    }
    out->pid = pid;
    out->comm = stats.comm;
    out->txBytes = stats.tx_bytes;
    out->txPackets = stats.tx_packets;
    out->retransCount = stats.retrans_count;
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
    stateSupport_.recordReadSuccess(static_cast<uint64_t>(elapsed));
    return true;
#else
    (void)pid; (void)out;
    return false;
#endif
}

}  // namespace weaknet_dbus

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
#include "ebpf_log_bridge.hpp"
#include "utils/top_n_recent.hpp"

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

// 把内核 tracepoint 中 sockaddr_storage.sin_addr（big-endian 网络序）按
// 点分十进制字符串输出。bpf 侧写入的是网络序 uint32_t，这里要 inet_ntop 的
// network-order s_addr 语义，因此直接传入原值即可（inet_ntop 期望 s_addr 为
// 网络序，无需 ntohl）。
std::string formatIpv4(uint32_t addr_network_order) {
    char buf[INET_ADDRSTRLEN] = {0};
    struct in_addr in;
    in.s_addr = addr_network_order;
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
    installLibbpfLogBridge();
    clearLibbpfError();
    impl_->obj = bpf_object__open_file(bpfObjPath.c_str(), nullptr);
    if (!impl_->obj) {
        LOG_WARNING(LogModule::TCP_RTT, "TcpRttMonitor: failed to open BPF object: "
                    << bpfObjPath << takeLibbpfError());
        impl_->state = EbpfMonitorState::Fallback;
        return false;
    }

    weaknet::applyMapSizingPlan(impl_->obj, plan);

    if (bpf_object__load(impl_->obj) != 0) {
        LOG_WARNING(LogModule::TCP_RTT, "TcpRttMonitor: failed to load BPF object"
                    << takeLibbpfError());
        bpf_object__close(impl_->obj);
        impl_->obj = nullptr;
        impl_->state = EbpfMonitorState::Fallback;
        return false;
    }

    struct bpf_program* prog = bpf_object__find_program_by_name(impl_->obj, "trace_tcp_probe");
    if (!prog) {
        LOG_WARNING(LogModule::TCP_RTT, "TcpRttMonitor: program trace_tcp_probe not found");
        bpf_object__close(impl_->obj);
        impl_->obj = nullptr;
        impl_->state = EbpfMonitorState::Fallback;
        return false;
    }

    impl_->link = bpf_program__attach(prog);
    if (!impl_->link) {
        LOG_WARNING(LogModule::TCP_RTT, "TcpRttMonitor: failed to attach tracepoint trace_tcp_probe");
        bpf_object__close(impl_->obj);
        impl_->obj = nullptr;
        impl_->state = EbpfMonitorState::Fallback;
        return false;
    }

    impl_->statsMapFd = bpf_object__find_map_fd_by_name(impl_->obj, "tcp_rtt_stats");
    impl_->globalMapFd = bpf_object__find_map_fd_by_name(impl_->obj, "tcp_rtt_global");
    impl_->selfPidMapFd = bpf_object__find_map_fd_by_name(impl_->obj, "tcp_rtt_self_pid");

    if (impl_->statsMapFd < 0 || impl_->globalMapFd < 0) {
        LOG_WARNING(LogModule::TCP_RTT, "TcpRttMonitor: essential BPF maps not found");
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
    LOG_INFO(LogModule::TCP_RTT, "TcpRttMonitor: eBPF attached successfully (trace_tcp_probe)");
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
    //
    // 【为什么遍历时不构造明细】
    //
    // 这个 map 在板端会定标到 65536 条（config.yaml 的 map_sizing_mode: auto，
    // 按内存 0.5% 分摊），实测常见 1500~2300 条常驻。一次遍历曾为**每一条**
    // 都做两件事：inet_ntop + 两次 std::string 构造（各一次堆分配），然后
    // 对全量做两次 std::sort。
    //
    // 实测（2026-11-15 板端，2258 条）：单次 getStats() 47 ms，而这段时间
    // 全程持有 impl_->mutex —— 同一个监控器的 isAvailable() / health() /
    // metrics() 全部被阻塞，上行遥测采样器也在等。CPU 只是次要代价。
    //
    // 但**不能**因此缩小遍历范围：TcpRttEvaluator 用 activeConns（连接总数）
    // 和全量 RTT 分位数判定健康，少看一条就是判定失真。所以仍然逐条 lookup，
    // 只是把「物化」推迟到筛选之后。
    //
    // 这里只累积两类东西：
    //   - all_rtts：全量 srtt，分位数必须基于全体连接；
    //   - top：按 lastTimestampNs 维护「最近采样」的 kTopConnLimit 条。
    //
    // top 用小顶堆而非全量 vector + sort：堆里始终只有 N 条，插入 O(log N)，
    // 总体 O(n log N) 且没有 2258 条明细的构造/析构成本。
    TcpConnKeyBpf key = {}, next_key = {};
    TcpRttStatBpf val = {};
    std::vector<uint32_t> all_rtts;

    // 对外暴露的连接明细上限。
    //
    // 取 32 是因为两个消费者都不需要更多：上行导出器自己又 resize(10)，
    // D-Bus 的 GetTcpRttStats 是诊断接口（CLI 用），32 条足够定位问题。
    // 之前「有多少给多少」让一条 1200 条连接的上行报文膨胀到数百 KB，
    // 而对任何消费方都没有增加信息量。
    constexpr size_t kTopConnLimit = 32;
    weaknet::TopNRecent<TcpRttConnItem> top(kTopConnLimit);

    size_t conn_count = 0;
    bool has_key = (bpf_map_get_next_key(impl_->statsMapFd, nullptr, &next_key) == 0);
    while (has_key) {
        key = next_key;
        // 先取下一个 key 再 lookup：lookup 失败不该打断遍历。
        has_key = (bpf_map_get_next_key(impl_->statsMapFd, &key, &next_key) == 0);

        if (bpf_map_lookup_elem(impl_->statsMapFd, &key, &val) != 0) {
            continue;
        }

        all_rtts.push_back(val.srtt_us);
        ++conn_count;

        // 堆已满且当前这条比堆顶还旧 → 它进不了 top-N，跳过全部字符串构造。
        // 这是本函数最热的分支：正常情况下绝大多数条目走这里。
        if (top.full() && val.last_timestamp_ns <= top.oldest().lastTimestampNs) {
            continue;
        }

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
        top.offer(std::move(item));
    }

    summary.activeConns = conn_count;

    // 3. 计算分位数 (P50, P90, P99)
    if (!all_rtts.empty()) {
        std::sort(all_rtts.begin(), all_rtts.end());
        size_t n = all_rtts.size();
        summary.p50RttUs = all_rtts[n * 50 / 100];
        summary.p90RttUs = all_rtts[n * 90 / 100];
        summary.p99RttUs = all_rtts[std::min(n - 1, n * 99 / 100)];
    }

    // 4. 明细按最近采样时间降序（输出顺序与原先的全量 sort 一致）
    summary.topConns = top.take();

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

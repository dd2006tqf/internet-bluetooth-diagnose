/**
 * @file tcp_connect_monitor.cpp
 * @brief TCP 建连可观测性 — 用户态实现
 *
 * 处理模型：
 *   内核侧上报的是**状态迁移**（SYN_SENT / ESTABLISHED / CLOSE），
 *   用户态按四元组把迁移归并成一次"建连尝试"：
 *     SYN_SENT                 → 建立 attempt（记录起始时间）
 *     ESTABLISHED（有 attempt）→ 该次建连成功
 *     CLOSE（有 attempt）      → 该次建连失败
 *     终态但无 attempt         → 计入 unmatched_terminal（证据不完整，
 *                               不伪造结论，由 Evidence Quality 处理）
 *
 * 内存域与字节序与 DNS 捕获保持一致：
 *   地址为原始网络序 4 字节，用户态统一 ntohl()；
 *   端口由 tracepoint 以主机序给出。
 */

#include "tcp_connect_monitor.hpp"
#include "logger.hpp"

#include <cerrno>
#include <vector>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <unordered_map>
#include <deque>
#include <mutex>
#include <atomic>
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

namespace {

/// 与 tcp_connect.bpf.c 的 struct tcp_connect_event 一一对应
struct tcp_connect_event {
    __u32 saddr;
    __u32 daddr;
    __u16 sport;
    __u16 dport;
    __u8  old_state;
    __u8  new_state;
    __u8  is_ipv4;
    __u8  reserved;
    __u64 timestamp_ns;
};

/// 与 tcp_connect.bpf.c 的 enum tcp_connect_stat 一一对应
enum tcp_connect_stat {
    TCP_CONN_STAT_ENTER = 0,
    TCP_CONN_STAT_NON_IPV4,
    TCP_CONN_STAT_SYN_SENT,
    TCP_CONN_STAT_ESTABLISHED,
    TCP_CONN_STAT_CLOSED_FROM_SYN_SENT,
    TCP_CONN_STAT_EMITTED,
    TCP_CONN_STAT_EMIT_FAIL,
    TCP_CONN_STAT_OTHER_TRANSITION,
    TCP_CONN_STAT_MAX
};

constexpr uint8_t kTcpSynSent = 2;
constexpr uint8_t kTcpEstablished = 1;
constexpr uint8_t kTcpClose = 7;

struct TcpConnKey {
    uint32_t saddr;
    uint32_t daddr;
    uint16_t sport;
    uint16_t dport;
    bool operator==(const TcpConnKey& o) const {
        return saddr == o.saddr && daddr == o.daddr &&
               sport == o.sport && dport == o.dport;
    }
};

struct TcpConnKeyHash {
    size_t operator()(const TcpConnKey& k) const noexcept {
        size_t h = std::hash<uint32_t>{}(k.saddr);
        h ^= std::hash<uint32_t>{}(k.daddr) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= (static_cast<size_t>(k.sport) << 16) | k.dport;
        return h;
    }
};

struct DrainStats {
    uint64_t poll_calls{0};
    uint64_t poll_records{0};
    uint64_t poll_errors{0};
    uint64_t sample_callbacks{0};
    uint64_t lost_events{0};
};

}  // namespace

/**
 * @brief Pimpl：持有 libbpf 句柄与窗口状态
 */
struct TcpConnectMonitor::Impl {
    int counters_fd = -1;
    struct bpf_object* obj = nullptr;
    struct bpf_link* link = nullptr;
    struct perf_buffer* events = nullptr;

    uint64_t lost_events = 0;
    DrainStats drain_stats{};
    std::atomic<uint64_t> last_counters[TCP_CONN_STAT_MAX];

    /// 进行中的建连尝试（key = 四元组）
    std::unordered_map<TcpConnKey, std::chrono::steady_clock::time_point, TcpConnKeyHash> inflight;

    /// 窗口内的建连结论
    std::deque<TcpConnectObservation> observations;

    /// 计数（窗口增量）
    uint64_t attempts = 0;
    uint64_t successes = 0;
    uint64_t failures = 0;
    uint64_t unmatched_terminal = 0;

    mutable std::mutex mutex;

    Impl() {
        for (int i = 0; i < TCP_CONN_STAT_MAX; ++i) last_counters[i].store(0);
    }
};

namespace {

/**
 * @brief perf sample 回调：把状态迁移归并成建连尝试/结论
 *
 * 只改变 Impl 内部状态，不做任何 SLE 判断——评价在 evaluator 中完成。
 */
void on_tcp_event(void* ctx, int /*cpu*/, void* data, __u32 size) {
    auto* impl = static_cast<TcpConnectMonitor::Impl*>(ctx);
    if (!impl || size < sizeof(tcp_connect_event)) return;

    const auto* ev = static_cast<const tcp_connect_event*>(data);
    TcpConnKey key{ev->saddr, ev->daddr, ev->sport, ev->dport};
    const auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->drain_stats.sample_callbacks++;

    if (ev->new_state == kTcpSynSent && ev->old_state != kTcpSynSent) {
        // 建连开始
        impl->inflight[key] = now;
        impl->attempts++;
        return;
    }

    const bool is_terminal = (ev->new_state == kTcpEstablished || ev->new_state == kTcpClose);
    if (!is_terminal) return;

    auto it = impl->inflight.find(key);
    if (it == impl->inflight.end()) {
        // 终态但未见 SYN_SENT：证据不完整，如实计入而不是伪造一次尝试
        impl->unmatched_terminal++;
        return;
    }

    TcpConnectObservation obs;
    obs.saddr = ev->saddr;
    obs.daddr = ev->daddr;
    obs.sport = ev->sport;
    obs.dport = ev->dport;
    obs.attempted = true;
    obs.success = (ev->new_state == kTcpEstablished);
    obs.observed_at = now;
    obs.latency_ms = std::chrono::duration_cast<std::chrono::microseconds>(now - it->second).count() / 1000.0;
    if (obs.latency_ms < 0.0) obs.latency_ms = 0.0;

    impl->inflight.erase(it);
    if (obs.success) impl->successes++;
    else impl->failures++;
    impl->observations.push_back(obs);
}

void on_tcp_lost(void* ctx, int /*cpu*/, __u64 lost) {
    auto* impl = static_cast<TcpConnectMonitor::Impl*>(ctx);
    if (!impl) return;
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->drain_stats.lost_events += lost;
    impl->lost_events += lost;
}

}  // namespace

TcpConnectMonitor::TcpConnectMonitor()
    : impl_(std::make_unique<Impl>()) {}

TcpConnectMonitor::~TcpConnectMonitor() {
    stop();
}

bool TcpConnectMonitor::init(const std::string& bpfObjPath, uint32_t capture_pages) {
    stateSupport_.setState(EbpfMonitorState::Initializing, false, "loading BPF object");
#if !HAVE_LIBBPF
    LOG_INFO(LogModule::NETWORK, "TcpConnectMonitor: BPF not available (no libbpf)");
    available_ = false;
    initialized_ = true;
    stateSupport_.setState(EbpfMonitorState::Fallback, false, "libbpf unavailable");
    return false;
#else
    LOG_INFO(LogModule::NETWORK, "TcpConnectMonitor: loading BPF object from " << bpfObjPath);

    LIBBPF_OPTS(bpf_object_open_opts, opts);
    struct bpf_object* obj = bpf_object__open_file(bpfObjPath.c_str(), &opts);
    if (!obj) {
        LOG_ERROR(LogModule::NETWORK, "TcpConnectMonitor: failed to open BPF object: " << bpfObjPath);
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Error, false, "failed to open BPF object");
        return false;
    }

    if (bpf_object__load(obj) != 0) {
        LOG_ERROR(LogModule::NETWORK, "TcpConnectMonitor: failed to load BPF object");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Error, false, "failed to load BPF object");
        return false;
    }

    impl_->counters_fd = bpf_object__find_map_fd_by_name(obj, "tcp_connect_counters");

    struct bpf_program* prog = bpf_object__find_program_by_name(obj, "trace_tcp_connect");
    if (!prog) {
        LOG_ERROR(LogModule::NETWORK, "TcpConnectMonitor: BPF program not found");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Error, false, "BPF program not found");
        return false;
    }

    impl_->link = bpf_program__attach(prog);
    if (libbpf_get_error(impl_->link)) {
        LOG_ERROR(LogModule::NETWORK, "TcpConnectMonitor: attach tracepoint/sock/inet_sock_set_state failed");
        impl_->link = nullptr;
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Fallback, false, "attach failed");
        return false;
    }
    stateSupport_.recordProbeAttached();

    impl_->obj = obj;

    auto events_fd = bpf_object__find_map_fd_by_name(obj, "tcp_connect_events");
    if (events_fd >= 0) {
        impl_->events = perf_buffer__new(events_fd, capture_pages, on_tcp_event, on_tcp_lost,
                                         impl_.get(), nullptr);
        if (!impl_->events) {
            LOG_WARNING(LogModule::NETWORK, "TcpConnectMonitor: perf buffer unavailable");
        } else {
            LOG_INFO(LogModule::NETWORK, "TcpConnectMonitor: perf buffer pages=" << capture_pages);
        }
    }

    available_ = true;
    initialized_ = true;
    stateSupport_.setState(EbpfMonitorState::Attached, true, "tracepoint attached");
    LOG_INFO(LogModule::NETWORK, "TcpConnectMonitor: initialized successfully");
    return true;
#endif
}

void TcpConnectMonitor::stop() {
    stateSupport_.setState(EbpfMonitorState::Stopped, false, "stopped");
#if HAVE_LIBBPF
    if (impl_->events) { perf_buffer__free(impl_->events); impl_->events = nullptr; }
    if (impl_->link) { bpf_link__destroy(impl_->link); impl_->link = nullptr; }
#endif
    if (impl_->obj) {
        bpf_object__close(impl_->obj);
        impl_->obj = nullptr;
        LOG_INFO(LogModule::NETWORK, "TcpConnectMonitor: stopped");
    }
    impl_->counters_fd = -1;
    available_ = false;
}

size_t TcpConnectMonitor::drain(std::chrono::milliseconds window_duration) {
    if (!impl_->events) return 0;
    impl_->drain_stats.poll_calls++;

    // 与 DNS 捕获同样的有界排空：突发建连不应因单趟 poll 而丢事件
    constexpr int kMaxPollRounds = 64;
    uint64_t total = 0;
    int last_ret = 0;
    for (int round = 0; round < kMaxPollRounds; ++round) {
        last_ret = perf_buffer__poll(impl_->events, 0);
        if (last_ret <= 0) break;
        total += static_cast<uint64_t>(last_ret);
    }
    if (last_ret < 0 && last_ret != -EINTR) {
        impl_->drain_stats.poll_errors++;
        stateSupport_.recordReadFailure("perf poll failed");
        LOG_WARNING(LogModule::NETWORK, "TcpConnectMonitor: perf poll failed: " << last_ret);
        return 0;
    }
    impl_->drain_stats.poll_records += total;
    stateSupport_.recordReadSuccess(0, total > 0);

    // 窗口老化：只保留窗口内的结论
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        while (!impl_->observations.empty()) {
            const auto& oldest = impl_->observations.front();
            if (now >= oldest.observed_at && (now - oldest.observed_at) > window_duration) {
                // 老化时同步扣减对应计数，保证 stats() 与观测集一致
                if (oldest.success) {
                    if (impl_->successes > 0) impl_->successes--;
                } else {
                    if (impl_->failures > 0) impl_->failures--;
                }
                impl_->observations.pop_front();
            } else {
                break;
            }
        }
        // 长时间无终态的进行中尝试不计入结论，避免内存无界增长
        constexpr size_t kMaxInflight = 8192;
        if (impl_->inflight.size() > kMaxInflight) {
            impl_->inflight.clear();
            LOG_WARNING(LogModule::NETWORK, "TcpConnectMonitor: inflight overflow cleared");
        }
    }
    return static_cast<size_t>(total);
}

std::vector<TcpConnectObservation> TcpConnectMonitor::recentObservations() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return std::vector<TcpConnectObservation>(impl_->observations.begin(), impl_->observations.end());
}

TcpConnectStats TcpConnectMonitor::stats() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    TcpConnectStats s;
    s.attempts = impl_->attempts;
    s.successes = impl_->successes;
    s.failures = impl_->failures;
    s.unmatched_terminal = impl_->unmatched_terminal;
    return s;
}

uint64_t TcpConnectMonitor::consumeLostEvents() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const uint64_t v = impl_->lost_events;
    impl_->lost_events = 0;
    return v;
}

std::string TcpConnectMonitor::getCaptureDiagnostics() {
#if HAVE_LIBBPF
    // 读 per-CPU 计数器并求总和
    uint64_t counters[TCP_CONN_STAT_MAX] = {};
    if (impl_->counters_fd >= 0) {
        const __u32 ncpus = libbpf_num_possible_cpus();
        if (ncpus > 0 && ncpus <= 512) {
            for (int k = 0; k < TCP_CONN_STAT_MAX; ++k) {
                std::vector<__u64> per_cpu(ncpus, 0);
                if (bpf_map_lookup_elem(impl_->counters_fd, &k, per_cpu.data()) == 0) {
                    uint64_t sum = 0;
                    for (__u32 c = 0; c < ncpus; ++c) sum += per_cpu[c];
                    counters[k] = sum;
                }
            }
        }
    }

    DrainStats ds;
    TcpConnectStats st;
    size_t inflight = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        ds = impl_->drain_stats;
        inflight = impl_->inflight.size();
        st.attempts = impl_->attempts;
        st.successes = impl_->successes;
        st.failures = impl_->failures;
        st.unmatched_terminal = impl_->unmatched_terminal;
    }

    std::ostringstream json;
    json << "{"
         << "\"capture\":{"
         << "\"enter\":" << counters[TCP_CONN_STAT_ENTER] << ","
         << "\"non_ipv4\":" << counters[TCP_CONN_STAT_NON_IPV4] << ","
         << "\"syn_sent\":" << counters[TCP_CONN_STAT_SYN_SENT] << ","
         << "\"established\":" << counters[TCP_CONN_STAT_ESTABLISHED] << ","
         << "\"closed_from_syn_sent\":" << counters[TCP_CONN_STAT_CLOSED_FROM_SYN_SENT] << ","
         << "\"other_transition\":" << counters[TCP_CONN_STAT_OTHER_TRANSITION] << ","
         << "\"emitted\":" << counters[TCP_CONN_STAT_EMITTED] << ","
         << "\"emit_fail\":" << counters[TCP_CONN_STAT_EMIT_FAIL]
         << "},\"drain\":{"
         << "\"poll_calls\":" << ds.poll_calls << ","
         << "\"poll_records\":" << ds.poll_records << ","
         << "\"poll_errors\":" << ds.poll_errors << ","
         << "\"sample_callbacks\":" << ds.sample_callbacks << ","
         << "\"lost_events\":" << ds.lost_events
         << "},\"window\":{"
         << "\"attempts\":" << st.attempts << ","
         << "\"successes\":" << st.successes << ","
         << "\"failures\":" << st.failures << ","
         << "\"unmatched_terminal\":" << st.unmatched_terminal << ","
         << "\"inflight\":" << inflight
         << "}}";
    return json.str();
#else
    return "{}";
#endif
}

}  // namespace weaknet_dbus

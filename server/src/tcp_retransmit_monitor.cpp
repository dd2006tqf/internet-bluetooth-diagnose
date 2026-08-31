/**
 * @file tcp_retransmit_monitor.cpp
 * @brief TCP 重传追踪监控器 - 用户态实现
 *
 * 监控指标：
 *   - 每 TCP 连接的重传率：total_retrans / total_segs × 100%
 *   - 全局 TCP 重传率（computeLossRate）
 *   - 重传事件：含 pid/tgid/timestamp/tcp 状态
 *   - 顶 N 重传最多连接（getTopRetransConnections）
 *
 * 数据源（eBPF）：
 *   - BPF 对象文件名：tcp_retransmit_monitor.bpf.o
 *   - 探针类型：kprobe（内核函数入口）
 *     - kprobe/tcp_retransmit_skb → trace_tcp_retransmit（捕获 TCP 重传事件，更新 retrans_stats Map）
 *     - kprobe/tcp_sendmsg         → trace_tcp_sendmsg（捕获正常 TCP 发送，累计 total_segs）
 *   - 数据通道：BPF Map（Hash 类型）
 *     - retrans_events：重传事件 Map（键 = tcp_conn_key 四元组，值 = tcp_retrans_event{pid/timestamp/segs_out/segs_retrans/sstate}）
 *     - retrans_stats：重传统计 Map（键 = tcp_conn_key 四元组，值 = tcp_retrans_stats{total_retrans/total_segs/last_state}）
 *   - 用户态通过 bpf_map_get_next_key + bpf_map_lookup_elem 遍历 retrans_stats Map
 *
 * 线程模型：
 *   - 本类本身不创建独立线程，由外部周期性调用 getStats() / computeLossRate()
 *   - 无锁：eBPF Map 读取对并发安全
 *
 * 重传归因分析：
 *   - 重传率高 + TCP 状态为 CLOSE_WAIT → 对端问题（服务器没处理完就关了连接）
 *   - 重传率高 + TCP 状态为 ESTABLISHED → 链路问题（网络丢包）
 *   - 重传率高 + TCP 状态为 TIME_WAIT → 正常关闭的连接不应该有重传，检查异常
 */

#include "tcp_retransmit_monitor.hpp"
#include "logger.hpp"

#include <cstring>
#include <cstdio>
#include <sstream>
#include <algorithm>
#include <arpa/inet.h>
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
 * @brief TCP 连接四元组键（与 BPF 端 retrans_events/retrans_stats Map 的 key 一致）
 */
struct tcp_conn_key {
    __u32 saddr;   // 源 IP（网络字节序）
    __u32 daddr;   // 目的 IP（网络字节序）
    __u16 sport;   // 源端口
    __u16 dport;   // 目的端口
} __attribute__((packed));

/**
 * @brief TCP 重传事件记录（与 BPF 端 retrans_events Map 的 value 一致）
 */
struct tcp_retrans_event {
    __u32 pid;              // 进程 ID
    __u32 tgid;             // 线程组 ID（= pid）
    __u64 timestamp_ns;     // 事件时间戳（纳秒，CLOCK_MONOTONIC）
    __u32 segs_out;         // 该连接累计发送段数（来自 tcp_sock->segs_out）
    __u32 segs_retrans;     // 该连接累计重传段数（来自 tcp_sock->segs_retrans）
    __u32 sstate;           // TCP 连接状态（tcp_states 枚举：ESTABLISHED/TIME_WAIT/CLOSE_WAIT 等）
};

/**
 * @brief TCP 重传统计（与 BPF 端 retrans_stats Map 的 value 一致）
 */
struct tcp_retrans_stats {
    __u64 total_retrans;   // 累计重传次数
    __u64 total_segs;      // 累计发送段数（正常发送 + 重传）
    __u32 last_state;      // 最后一次捕获时的 TCP 状态
};

// ---- 辅助函数 ----

/**
 * @brief 将 tcp_conn_key 格式化为可读字符串 "src:port -> dst:port"
 * @param key TCP 连接四元组键
 * @return 格式化后的字符串
 */
std::string connKeyToString(const tcp_conn_key& key) {
    struct in_addr sa{key.saddr}, da{key.daddr};
    char src_buf[INET_ADDRSTRLEN], dst_buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sa, src_buf, sizeof(src_buf));
    inet_ntop(AF_INET, &da, dst_buf, sizeof(dst_buf));
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s:%u -> %s:%u",
        src_buf, ntohs(key.sport),
        dst_buf, ntohs(key.dport));
    return std::string(buf);
}

/**
 * @brief TcpConnKey 比较运算符（用于 std::map 的有序排序）
 */
bool TcpConnKey::operator<(const TcpConnKey& other) const {
    if (saddr != other.saddr) return saddr < other.saddr;
    if (daddr != other.daddr) return daddr < other.daddr;
    if (sport != other.sport) return sport < other.sport;
    return dport < other.dport;
}

bool TcpConnKey::operator==(const TcpConnKey& other) const {
    return saddr == other.saddr && daddr == other.daddr &&
           sport == other.sport && dport == other.dport;
}

// ---- 实现 ----

/**
 * @brief Pimpl 实现结构体
 */
struct TcpRetransMonitor::Impl {
    int retrans_events_fd = -1;  ///< retrans_events Map fd（重传事件明细）
    int retrans_stats_fd = -1;   ///< retrans_stats Map fd（重传统计聚合）
    struct bpf_object *obj = nullptr;
    struct bpf_link *link_retrans = nullptr;  ///< kprobe/tcp_retransmit_skb BPF link（捕获重传）
    struct bpf_link *link_send = nullptr;     ///< kprobe/tcp_sendmsg BPF link（正常发送，用于累计 total_segs）
};

TcpRetransMonitor::TcpRetransMonitor()
    : impl_(std::make_unique<Impl>()) {}

TcpRetransMonitor::~TcpRetransMonitor() {
    stop();
}

/**
 * @brief 初始化 TCP 重传监控器
 *
 * 挂载 2 个 kprobe：
 *   - kprobe/tcp_retransmit_skb → trace_tcp_retransmit（捕获重传事件，更新 total_retrans）
 *   - kprobe/tcp_sendmsg         → trace_tcp_sendmsg（捕获正常 TCP 发送，更新 total_segs）
 *
 * @param bpfObjPath BPF 对象文件路径（通常为 "build/tcp_retransmit_monitor.bpf.o"）
 * @return true  初始化成功（至少一个 kprobe 挂载成功）
 *         false 初始化失败
 */
bool TcpRetransMonitor::init(const std::string& bpfObjPath) {
    stateSupport_.setState(EbpfMonitorState::Initializing, false, "loading BPF object");
#if !HAVE_LIBBPF
    LOG_INFO(LogModule::TCP_LOSS, "TcpRetransMonitor: BPF not available (no libbpf)");
    available_ = false;
    initialized_ = true;
    stateSupport_.setState(EbpfMonitorState::Fallback, false, "libbpf unavailable");
    return false;
#else
    LOG_INFO(LogModule::TCP_LOSS, "TcpRetransMonitor: loading BPF object from " << bpfObjPath);

    // 打开 BPF 对象
    LIBBPF_OPTS(bpf_object_open_opts, opts);
    struct bpf_object *obj = bpf_object__open_file(bpfObjPath.c_str(), &opts);
    if (!obj) {
        LOG_ERROR(LogModule::TCP_LOSS, "TcpRetransMonitor: failed to open BPF object: " << bpfObjPath);
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Error, false, "failed to open BPF object");
        return false;
    }

    // 加载 BPF 程序到内核
    if (bpf_object__load(obj) != 0) {
        LOG_ERROR(LogModule::TCP_LOSS, "TcpRetransMonitor: failed to load BPF object");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Error, false, "failed to load BPF object");
        return false;
    }

    // 查找 retrans_events Map（重传事件明细）和 retrans_stats Map（重传统计聚合）
    impl_->retrans_events_fd = bpf_object__find_map_fd_by_name(obj, "retrans_events");
    if (impl_->retrans_events_fd < 0) {
        LOG_ERROR(LogModule::TCP_LOSS, "TcpRetransMonitor: retrans_events map not found");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Error, false, "retrans_events map not found");
        return false;
    }

    impl_->retrans_stats_fd = bpf_object__find_map_fd_by_name(obj, "retrans_stats");
    if (impl_->retrans_stats_fd < 0) {
        LOG_ERROR(LogModule::TCP_LOSS, "TcpRetransMonitor: retrans_stats map not found");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Error, false, "retrans_stats map not found");
        return false;
    }

    // attach 探针到 kprobe/tcp_retransmit_skb 和 kprobe/tcp_sendmsg
    // 前者捕获重传事件，后者用于累计总发送段数（分母）
    struct bpf_program *retrans_prog = bpf_object__find_program_by_name(obj, "trace_tcp_retransmit");
    struct bpf_program *send_prog = bpf_object__find_program_by_name(obj, "trace_tcp_sendmsg");
    if (!retrans_prog || !send_prog) {
        LOG_ERROR(LogModule::TCP_LOSS, "TcpRetransMonitor: BPF program not found");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Error, false, "BPF program not found");
        return false;
    }

    impl_->link_retrans = bpf_program__attach(retrans_prog);
    if (libbpf_get_error(impl_->link_retrans)) {
        LOG_ERROR(LogModule::TCP_LOSS, "TcpRetransMonitor: attach tcp_retransmit_skb failed");
        impl_->link_retrans = nullptr;
    }
    impl_->link_send = bpf_program__attach(send_prog);
    if (libbpf_get_error(impl_->link_send)) {
        LOG_ERROR(LogModule::TCP_LOSS, "TcpRetransMonitor: attach tcp_sendmsg failed");
        impl_->link_send = nullptr;
    }

    if (!impl_->link_retrans && !impl_->link_send) {
        LOG_ERROR(LogModule::TCP_LOSS, "TcpRetransMonitor: both probes failed to attach");
        bpf_object__close(obj);
        impl_->link_retrans = impl_->link_send = nullptr;
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Fallback, false, "all BPF probes failed to attach");
        return false;
    }

    impl_->obj = obj;
    available_ = true;
    initialized_ = true;

    LOG_INFO(LogModule::TCP_LOSS, "TcpRetransMonitor: initialized successfully");
    stateSupport_.setState(EbpfMonitorState::Attached, true, "BPF probes attached");
    stateSupport_.recordProbeAttached();
    stateSupport_.recordProbeAttached();
    return true;
#endif
}

/**
 * @brief 停止 TCP 重传监控器：销毁 BPF link 和 BPF 对象
 */
void TcpRetransMonitor::stop() {
    stateSupport_.setState(EbpfMonitorState::Stopped, false, "stopped");
#if HAVE_LIBBPF
    if (impl_->link_retrans) { bpf_link__destroy(impl_->link_retrans); impl_->link_retrans = nullptr; }
    if (impl_->link_send) { bpf_link__destroy(impl_->link_send); impl_->link_send = nullptr; }
#endif
    if (impl_->obj) {
        bpf_object__close(impl_->obj);
        impl_->obj = nullptr;
        LOG_INFO(LogModule::TCP_LOSS, "TcpRetransMonitor: stopped");
    }
    impl_->retrans_events_fd = -1;
    impl_->retrans_stats_fd = -1;
    available_ = false;
}

/**
 * @brief 从 retrans_stats Map 获取所有连接的重传统计
 *
 * 遍历 retrans_stats Map，将内核态 tcp_conn_key + tcp_retrans_stats
 * 转为用户态 TcpConnKey + TcpRetransStats 结构。
 *
 * @return 以 TcpConnKey 为键的重传统计 Map
 */
std::map<TcpConnKey, TcpRetransStats> TcpRetransMonitor::getStats() {
    std::map<TcpConnKey, TcpRetransStats> result;
#if HAVE_LIBBPF
    if (impl_->retrans_stats_fd < 0) {
        stateSupport_.recordReadFailure("retrans_stats map unavailable");
        return result;
    }

    auto started = std::chrono::steady_clock::now();
    // 遍历 retrans_stats Map（Hash 类型，逐 key 遍历）
    tcp_conn_key cur_key = {}, next_key = {};
    while (bpf_map_get_next_key(impl_->retrans_stats_fd, &cur_key, &next_key) == 0) {
        tcp_retrans_stats stats = {};
        if (bpf_map_lookup_elem(impl_->retrans_stats_fd, &next_key, &stats) == 0) {
            TcpConnKey key{next_key.saddr, next_key.daddr, next_key.sport, next_key.dport};
            TcpRetransStats val{stats.total_retrans, stats.total_segs, stats.last_state};
            result[key] = val;
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
 * @brief 计算全局 TCP 重传率
 *
 * 将所有连接的 total_retrans 和 total_segs 分别累加，
 * 然后计算加权重传率（避免单连接 total_segs 为 0 导致的除零问题）。
 *
 * 计算公式：lossRate = Σ(total_retrans) / Σ(total_segs) × 100%
 *
 * @return 全局 TCP 重传率（%）；无数据时返回 0.0
 */
double TcpRetransMonitor::computeLossRate() {
    auto stats = getStats();
    uint64_t totalRetrans = 0, totalSegs = 0;
    for (const auto& [key, val] : stats) {
        totalRetrans += val.totalRetrans;  // 累加所有连接的重传次数
        totalSegs += val.totalSegs;        // 累加所有连接的发送段数
    }
    if (totalSegs == 0) return 0.0;         // 无流量时返回 0
    return (totalRetrans * 100.0) / totalSegs;  // 重传率 = 重传次数 / 总段数 × 100%
}

/**
 * @brief 获取顶 N 重传最多的连接（按 totalRetrans 降序）
 *
 * @param topN 返回的最大连接数
 * @return 按重传次数降序排列的连接统计列表
 */
std::vector<std::pair<TcpConnKey, TcpRetransStats>> TcpRetransMonitor::getTopRetransConnections(size_t topN) {
    auto stats = getStats();
    std::vector<std::pair<TcpConnKey, TcpRetransStats>> vec(stats.begin(), stats.end());
    std::sort(vec.begin(), vec.end(),
        [](const auto& a, const auto& b) {
            return a.second.totalRetrans > b.second.totalRetrans;  // 按重传次数降序
        });
    if (vec.size() > topN) vec.resize(topN);
    return vec;
}

}  // namespace weaknet_dbus

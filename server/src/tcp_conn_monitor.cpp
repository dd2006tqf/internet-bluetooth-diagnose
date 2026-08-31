/**
 * @file tcp_conn_monitor.cpp
 * @brief TCP 连接生命周期监控器 — 用户态实现
 *
 * 监控指标：
 *   - 入向连接 accept/close 计数与当前活跃连接数
 *   - accept 失败次数（客户端 SYN 后立即中止等）
 *   - 连接时长分布（<100ms / <1s / <10s / <1min / <5min / <1h / >=1h）
 *   - 每端口 accept/close 计数（定位哪个服务在接收连接，如 sshd:22）
 *
 * 数据源（eBPF）：
 *   - BPF 对象文件名：tcp_conn_stats.bpf.o
 *   - 探针类型：kretprobe + kprobe
 *     - kretprobe/inet_csk_accept → trace_inet_csk_accept（入向连接建立）
 *     - kprobe/tcp_close          → trace_tcp_close（被跟踪连接关闭）
 *   - 数据通道：BPF Map
 *     - conn_start：连接起始时间（LRU_HASH，key = sock 指针）
 *     - conn_stats：全局聚合统计（HASH 单条目）
 *     - conn_ports：每端口 accept/close 计数（LRU_HASH）
 *
 * 线程模型：
 *   - 本类本身不创建独立线程，由外部（server.cpp）周期性调用 getStats()
 *   - 与其他 eBPF 监控器一致：init 失败路径统一置 Error/Fallback 状态
 */

#include "tcp_conn_monitor.hpp"
#include "logger.hpp"

#include <chrono>
#include <cerrno>
#include <cstdio>
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

namespace {

// ---- 数据结构映射（与 BPF 端 C 结构体一一对应，必须保持字段一致） ----

/**
 * @brief 全局聚合统计（与 BPF 端 conn_stats Map 的 value 一致）
 */
struct tcp_conn_global {
    int64_t total_accepts;         // 累计入向连接 accept 数
    uint64_t total_accept_failures; // accept 返回错误指针次数
    uint64_t accepts_v4;           // IPv4 入向连接数
    uint64_t accepts_v6;           // IPv6 入向连接数
    uint64_t total_closes;         // 累计被跟踪入向连接关闭数
    int64_t active_inbound;        // 当前活跃入向连接数
    uint64_t total_duration_ns;    // 已关闭连接时长总和
    uint64_t max_duration_ns;      // 已关闭连接最大时长
    uint64_t duration_count;       // 已完成时长统计的连接数
    uint64_t hist[7];              // 连接时长分桶计数
};

/**
 * @brief 每端口 accept/close 计数（与 BPF 端 conn_ports Map 的 value 一致）
 */
struct tcp_conn_port_value {
    uint64_t accepts;
    uint64_t closes;
};

// conn_ports 遍历的安全上限（map max_entries=128，防御异常返回导致死循环）
constexpr int kPortIterMax = 128;
// getStats 输出的端口明细上限（按 accept 数降序截断）
constexpr size_t kTopPortsMax = 8;

}  // namespace

// ---- 实现 ----

/**
 * @brief Pimpl 实现结构体，持有 libbpf 句柄
 *
 * 采用 Pimpl 模式隔离 libbpf 依赖，避免在没有 libbpf 的编译环境中暴露 BPF 类型
 */
struct TcpConnMonitor::Impl {
    int conn_start_fd = -1;            ///< conn_start Map fd（连接起始时间）
    int conn_stats_fd = -1;            ///< conn_stats Map fd（全局聚合）
    int conn_ports_fd = -1;            ///< conn_ports Map fd（每端口计数）
    struct bpf_object *obj = nullptr;  ///< BPF 对象实例
    struct bpf_link *link_accept = nullptr;  ///< kretprobe/inet_csk_accept 的 BPF link
    struct bpf_link *link_close = nullptr;   ///< kprobe/tcp_close 的 BPF link
};

TcpConnMonitor::TcpConnMonitor() : impl_(std::make_unique<Impl>()) {}
TcpConnMonitor::~TcpConnMonitor() { stop(); }

/**
 * @brief 初始化：加载 BPF 对象、查找 map/程序、挂载探针、预创建聚合条目
 *
 * 初始化流程：
 *   1. 打开并加载 BPF 对象文件（tcp_conn_stats.bpf.o）
 *   2. 查找 3 个 Map 的 fd：conn_start / conn_stats / conn_ports
 *   3. 找到 2 个 BPF 程序：trace_inet_csk_accept / trace_tcp_close
 *   4. attach 两个探针（libbpf 根据 SEC 名自动选择 kprobe/kretprobe）
 *   5. 预创建 conn_stats 的 key=0 聚合条目（BPF 侧虽也有 lookup-or-create，
 *      但预创建保证 getStats() 在空闲期读取零值而不是 ENOENT）
 *
 * @param bpfObjPath BPF 对象文件路径
 * @return true  初始化成功（两个探针均挂载）
 *         false 初始化失败
 */
bool TcpConnMonitor::init(const std::string& bpfObjPath) {
    stateSupport_.setState(EbpfMonitorState::Initializing, false, "loading BPF object");
#if !HAVE_LIBBPF
    LOG_INFO(LogModule::TCP_LOSS, "TcpConnMonitor: BPF not available (no libbpf)");
    available_ = false;
    initialized_ = true;
    stateSupport_.setState(EbpfMonitorState::Fallback, false, "libbpf unavailable");
    return false;
#else
    LOG_INFO(LogModule::TCP_LOSS, "TcpConnMonitor: loading BPF object from " << bpfObjPath);

    LIBBPF_OPTS(bpf_object_open_opts, opts);
    struct bpf_object *obj = bpf_object__open_file(bpfObjPath.c_str(), &opts);
    if (!obj) {
        LOG_ERROR(LogModule::TCP_LOSS, "TcpConnMonitor: failed to open BPF object: " << bpfObjPath);
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Error, false, "failed to open BPF object");
        return false;
    }

    if (bpf_object__load(obj) != 0) {
        LOG_ERROR(LogModule::TCP_LOSS, "TcpConnMonitor: failed to load BPF object");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Error, false, "failed to load BPF object");
        return false;
    }

    impl_->conn_start_fd = bpf_object__find_map_fd_by_name(obj, "conn_start");
    if (impl_->conn_start_fd < 0) {
        LOG_ERROR(LogModule::TCP_LOSS, "TcpConnMonitor: conn_start map not found");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Error, false, "conn_start map not found");
        return false;
    }

    impl_->conn_stats_fd = bpf_object__find_map_fd_by_name(obj, "conn_stats");
    if (impl_->conn_stats_fd < 0) {
        LOG_ERROR(LogModule::TCP_LOSS, "TcpConnMonitor: conn_stats map not found");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Error, false, "conn_stats map not found");
        return false;
    }

    impl_->conn_ports_fd = bpf_object__find_map_fd_by_name(obj, "conn_ports");
    if (impl_->conn_ports_fd < 0) {
        LOG_ERROR(LogModule::TCP_LOSS, "TcpConnMonitor: conn_ports map not found");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Error, false, "conn_ports map not found");
        return false;
    }

    struct bpf_program *accept_prog = bpf_object__find_program_by_name(obj, "trace_inet_csk_accept");
    struct bpf_program *close_prog = bpf_object__find_program_by_name(obj, "trace_tcp_close");
    if (!accept_prog || !close_prog) {
        LOG_ERROR(LogModule::TCP_LOSS, "TcpConnMonitor: BPF program not found"
                  << (!accept_prog ? " trace_inet_csk_accept" : "")
                  << (!close_prog ? " trace_tcp_close" : ""));
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Error, false, "BPF program not found");
        return false;
    }

    // attach：libbpf 根据 SEC 名（kretprobe/kprobe）自动选择挂载方式
    impl_->link_accept = bpf_program__attach(accept_prog);
    long err_accept = libbpf_get_error(impl_->link_accept);
    if (err_accept) {
        LOG_ERROR(LogModule::TCP_LOSS, "TcpConnMonitor: attach inet_csk_accept failed err=" << err_accept);
        impl_->link_accept = nullptr;
    }

    impl_->link_close = bpf_program__attach(close_prog);
    long err_close = libbpf_get_error(impl_->link_close);
    if (err_close) {
        LOG_ERROR(LogModule::TCP_LOSS, "TcpConnMonitor: attach tcp_close failed err=" << err_close);
        impl_->link_close = nullptr;
    }

    if (!impl_->link_accept && !impl_->link_close) {
        LOG_ERROR(LogModule::TCP_LOSS, "TcpConnMonitor: both probes failed to attach");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Fallback, false, "all BPF probes failed to attach");
        return false;
    }

    impl_->obj = obj;
    available_ = true;
    initialized_ = true;

    // 预创建 conn_stats 的 key=0 聚合条目：BPF 侧虽有 lookup-or-create，
    // 但预创建保证空闲期 getStats() 读到零值而不是 ENOENT（DnsMonitor 的教训）
    {
        __u32 zero_key = 0;
        tcp_conn_global empty_record = {};
        if (bpf_map_update_elem(impl_->conn_stats_fd, &zero_key, &empty_record, BPF_ANY) != 0) {
            LOG_WARNING(LogModule::TCP_LOSS,
                        "TcpConnMonitor: pre-create conn_stats entry failed (errno=" << errno
                        << "), getStats will treat ENOENT as empty stats");
        }
    }

    LOG_INFO(LogModule::TCP_LOSS, "TcpConnMonitor: initialized successfully");
    stateSupport_.setState(EbpfMonitorState::Attached, true, "BPF probes attached");
    stateSupport_.recordProbeAttached();
    stateSupport_.recordProbeAttached();
    return true;
#endif
}

/**
 * @brief 读取 TCP 连接生命周期聚合统计
 *
 * 读取流程：
 *   1. bpf_map_lookup_elem 查询 conn_stats key=0 的全局聚合条目
 *   2. bpf_map_get_next_key 遍历 conn_ports（上限 128 次），收集每端口计数
 *   3. 时长纳秒转毫秒，端口表按 accept 数降序取前 8
 *
 * @return TcpConnAggStats 聚合统计结果；失败时返回零值结构体
 */
TcpConnAggStats TcpConnMonitor::getStats() {
    TcpConnAggStats result = {};
#if HAVE_LIBBPF
    if (!available_ || impl_->conn_stats_fd < 0)
        return result;

    auto started = std::chrono::steady_clock::now();
    bool ok = false;

    __u32 key = 0;
    tcp_conn_global stats = {};
    if (bpf_map_lookup_elem(impl_->conn_stats_fd, &key, &stats) == 0) {
        ok = true;
        result.totalAccepts = static_cast<uint64_t>(stats.total_accepts);
        result.totalAcceptFailures = stats.total_accept_failures;
        result.acceptsV4 = stats.accepts_v4;
        result.acceptsV6 = stats.accepts_v6;
        result.totalCloses = stats.total_closes;
        result.activeInbound = stats.active_inbound;
        result.durationCount = stats.duration_count;
        for (int i = 0; i < kTcpConnHistBuckets; ++i)
            result.hist[i] = stats.hist[i];

        result.avgDurationMs = (stats.duration_count > 0)
            ? static_cast<uint64_t>(stats.total_duration_ns / stats.duration_count / 1000000)
            : 0;
        result.maxDurationMs = stats.max_duration_ns / 1000000;

        // 遍历每端口计数表（get_next_key + lookup；上限防御异常返回）
        std::vector<TcpConnPortStats> ports;
        __u16 port_key = 0, next_key = 0;
        for (int i = 0; i < kPortIterMax; ++i) {
            if (bpf_map_get_next_key(impl_->conn_ports_fd, &port_key, &next_key) != 0)
                break;
            tcp_conn_port_value pv = {};
            if (bpf_map_lookup_elem(impl_->conn_ports_fd, &next_key, &pv) == 0) {
                ports.push_back(TcpConnPortStats{next_key, pv.accepts, pv.closes});
            }
            port_key = next_key;
        }
        std::sort(ports.begin(), ports.end(),
                  [](const TcpConnPortStats& a, const TcpConnPortStats& b) {
                      return a.accepts > b.accepts;
                  });
        if (ports.size() > kTopPortsMax)
            ports.resize(kTopPortsMax);
        result.topPorts = std::move(ports);
    } else if (errno == ENOENT) {
        // 聚合条目尚未创建（理论上 init 已预创建；此处兜底）——“暂无数据”
        // 不是监控故障，记为一次成功读取并返回零值
        ok = true;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
    if (ok) {
        stateSupport_.recordReadSuccess(static_cast<uint64_t>(elapsed));
    } else {
        stateSupport_.recordReadFailure("conn_stats map lookup failed");
    }
#endif
    return result;
}

/**
 * @brief 停止 TCP 连接监控器：销毁 BPF link 和 BPF 对象
 *
 * 释放所有 libbpf 资源，将状态置为 Stopped，available 置为 false
 */
void TcpConnMonitor::stop() {
    stateSupport_.setState(EbpfMonitorState::Stopped, false, "stopped");
#if HAVE_LIBBPF
    if (impl_->link_accept) { bpf_link__destroy(impl_->link_accept); impl_->link_accept = nullptr; }
    if (impl_->link_close) { bpf_link__destroy(impl_->link_close); impl_->link_close = nullptr; }
    if (impl_->obj) { bpf_object__close(impl_->obj); impl_->obj = nullptr; }
    impl_->conn_start_fd = impl_->conn_stats_fd = impl_->conn_ports_fd = -1;
    available_ = false;
#endif
}

}  // namespace weaknet_dbus

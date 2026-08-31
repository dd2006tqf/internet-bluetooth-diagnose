/**
 * @file http_latency_monitor.cpp
 * @brief HTTP 请求级延迟监控器 - 用户态实现
 *
 * 监控指标：
 *   - HTTP TTFB（Time To First Byte）：从发送 HTTP 请求到收到首字节的时间（ns）
 *   - 每主机延迟统计：P50 / P95 / P99 / Max TTFB
 *   - 全局延迟分析：基于 P99 TTFB 的应用慢/正常判断
 *   - HTTP 事务级明细：请求/响应字节数、状态码、源/目的 IP 端口
 *
 * 数据源（eBPF）：
 *   - BPF 对象文件名：http_latency_monitor.bpf.o
 *   - 探针类型：kprobe（内核函数入口）+ kretprobe（内核函数返回）
 *     - kprobe/tcp_sendmsg              → probe_http_req（出站 HTTP 请求，记录 send_ns）
 *     - kprobe/tcp_recvmsg_locked 入口 → trace_recvmsg_entry（保存 sk+msg 到 BPF_MAP）
 *     - kretprobe/tcp_recvmsg_locked   → trace_recvmsg_return（从 BPF_MAP 取回 sk+msg，记录 recv_ns）
 *     - 内核兼容：Linux 5.19+ tcp_recvmsg_locked 已重命名为 tcp_recvmsg，程序会自动回退
 *   - 数据通道：BPF Map（Hash 类型）
 *     - http_txn_stats：HTTP 事务统计 Map（键 = tcp_conn_key 四元组，值 = http_txn_record）
 *   - 用户态通过 bpf_map_get_next_key + bpf_map_lookup_elem 遍历 Map
 *
 * 线程模型：
 *   - 本类本身不创建独立线程，由外部周期性调用 getRecentTxns() / getGlobalStats()
 *   - 内部 percentile() / getByDstHost() / getGlobalStats() 均为纯计算，无锁
 */

#include "http_latency_monitor.hpp"
#include "logger.hpp"

#include <cstring>
#include <algorithm>
#include <chrono>
#include <arpa/inet.h>
#include <cmath>

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
 * @brief TCP 连接四元组键（与 BPF 端 http_txn_stats Map 的 key 一致）
 */
struct tcp_conn_key {
    __u32 saddr;   // 源 IP（网络字节序）
    __u32 daddr;   // 目的 IP（网络字节序）
    __u16 sport;   // 源端口
    __u16 dport;   // 目的端口
};

/**
 * @brief HTTP 事务记录（与 BPF 端 http_txn_stats Map 的 value 一致）
 *
 * 一个 TCP 连接上可能有多个 HTTP 事务（Keep-Alive 场景），
 * BPF 程序通过 send_ns/recv_ns 差值近似为当前事务的 TTFB
 */
struct http_txn_record {
    __u64 send_ns;       // HTTP 请求发送时间戳（纳秒，CLOCK_MONOTONIC）
    __u64 recv_ns;       // HTTP 响应首字节接收时间戳（纳秒）
    __u32 req_bytes;     // HTTP 请求体大小
    __u32 resp_bytes;    // HTTP 响应体大小
    __u16 status_code;   // HTTP 状态码（200/301/404/500 等）
    __u8  is_request;    // 0=响应侧记录，1=请求侧记录（用于区分 send/recv）
    __u8  padding;       // 对齐填充
};

// ---- 实现 ----

/**
 * @brief Pimpl 实现结构体，持有 libbpf 句柄
 */
struct HttpLatencyMonitor::Impl {
    int http_txn_stats_fd = -1;       ///< http_txn_stats Map fd（HTTP 事务统计）
    struct bpf_object *obj = nullptr;  ///< BPF 对象实例
    struct bpf_link *link_send = nullptr;   ///< kprobe/tcp_sendmsg 的 BPF link（请求侧）
    struct bpf_link *link_entry = nullptr;  ///< kprobe/tcp_recvmsg_locked 入口 BPF link（响应侧入口）
    struct bpf_link *link_recv = nullptr;  ///< kretprobe/tcp_recvmsg_locked 返回 BPF link（响应侧返回）
};

HttpLatencyMonitor::HttpLatencyMonitor()
    : impl_(std::make_unique<Impl>()) {}

HttpLatencyMonitor::~HttpLatencyMonitor() {
    stop();
}

/**
 * @brief 初始化 HTTP 延迟监控器：加载 BPF 对象并挂载探针
 *
 * 初始化流程：
 *   1. 打开并加载 BPF 对象文件（http_latency_monitor.bpf.o）
 *   2. 查找 http_txn_stats Map 的 fd
 *   3. 找到 3 个 BPF 程序：probe_http_req / trace_recvmsg_entry / trace_recvmsg_return
 *   4. attach 请求侧：bpf_program__attach 自动识别 kprobe/tcp_sendmsg
 *   5. attach 响应侧：手动指定 kprobe 函数名，兼容 tcp_recvmsg_locked（旧内核）和 tcp_recvmsg（新内核）
 *
 * 注意：响应侧使用 entry kprobe + retprobe 配对，
 * 因为 kretprobe 的 PT_REGS_PARM1 是返回值不是入口参数，
 * 需要在入口探针保存 sk+msg 到 BPF_MAP，返回探针再取回。
 *
 * @param bpfObjPath BPF 对象文件路径（通常为 "build/http_latency_monitor.bpf.o"）
 * @return true  初始化成功
 *         false 初始化失败（libbpf 不可用、文件不存在、所有探针 attach 失败等）
 */
bool HttpLatencyMonitor::init(const std::string& bpfObjPath) {
    stateSupport_.setState(EbpfMonitorState::Initializing, false, "loading BPF object");
#if !HAVE_LIBBPF
    LOG_INFO(LogModule::NETWORK, "HttpLatencyMonitor: BPF not available (no libbpf)");
    available_ = false;
    initialized_ = true;
    stateSupport_.setState(EbpfMonitorState::Fallback, false, "libbpf unavailable");
    return false;
#else
    LOG_INFO(LogModule::NETWORK, "HttpLatencyMonitor: loading BPF object from " << bpfObjPath);

    LIBBPF_OPTS(bpf_object_open_opts, opts);
    struct bpf_object *obj = bpf_object__open_file(bpfObjPath.c_str(), &opts);
    if (!obj) {
        LOG_ERROR(LogModule::NETWORK, "HttpLatencyMonitor: failed to open BPF object: " << bpfObjPath);
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Error, false, "failed to open BPF object");
        return false;
    }

    if (bpf_object__load(obj) != 0) {
        LOG_ERROR(LogModule::NETWORK, "HttpLatencyMonitor: failed to load BPF object");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Error, false, "failed to load BPF object");
        return false;
    }

    impl_->http_txn_stats_fd = bpf_object__find_map_fd_by_name(obj, "http_txn_stats");
    if (impl_->http_txn_stats_fd < 0) {
        LOG_ERROR(LogModule::NETWORK, "HttpLatencyMonitor: http_txn_stats map not found");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Error, false, "http_txn_stats map not found");
        return false;
    }

    // attach 探针到 kprobe/tcp_sendmsg (出站请求) 和 kprobe/kretprobe/tcp_recvmsg_locked (响应)
    struct bpf_program *send_prog = bpf_object__find_program_by_name(obj, "probe_http_req");
    struct bpf_program *entry_prog = bpf_object__find_program_by_name(obj, "trace_recvmsg_entry");
    struct bpf_program *ret_prog = bpf_object__find_program_by_name(obj, "trace_recvmsg_return");
    if (!send_prog || !entry_prog || !ret_prog) {
        LOG_ERROR(LogModule::NETWORK, "HttpLatencyMonitor: BPF program not found"
                  << (!send_prog ? " probe_http_req" : "")
                  << (!entry_prog ? " trace_recvmsg_entry" : "")
                  << (!ret_prog ? " trace_recvmsg_return" : ""));
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Error, false, "BPF program not found");
        return false;
    }

    // 请求侧：自动 attach（libbpf 会自动识别 kprobe/tcp_sendmsg）
    impl_->link_send = bpf_program__attach(send_prog);
    long err_send = libbpf_get_error(impl_->link_send);
    if (err_send) {
        LOG_ERROR(LogModule::NETWORK, "HttpLatencyMonitor: attach tcp_sendmsg failed err=" << err_send);
        impl_->link_send = nullptr;
    }

    // 响应侧：entry kprobe + retprobe 配对
    // entry probe 保存 sk+msg 到 BPF_MAP，retprobe 读取（kretprobe 的 PT_REGS_PARM1 是返回值不是入口参数）
    // 注意：tcp_recvmsg_locked 在 Linux 5.19+ 内核已重命名为 tcp_recvmsg
    // 优先尝试 tcp_recvmsg_locked，失败则回退到 tcp_recvmsg
    struct bpf_link *l_entry = nullptr;
    struct bpf_link *l_ret = nullptr;

    // 尝试 tcp_recvmsg_locked（旧内核）
    l_entry = bpf_program__attach_kprobe(entry_prog, false, "tcp_recvmsg_locked");
    long err_entry = libbpf_get_error(l_entry);
    if (err_entry) {
        // 回退到 tcp_recvmsg（新内核）
        l_entry = bpf_program__attach_kprobe(entry_prog, false, "tcp_recvmsg");
        err_entry = libbpf_get_error(l_entry);
    }

    // 尝试 kretprobe/tcp_recvmsg_locked（第二个参数 true 表示 retprobe）
    l_ret = bpf_program__attach_kprobe(ret_prog, true, "tcp_recvmsg_locked");
    long err_ret = libbpf_get_error(l_ret);
    if (err_ret) {
        // 回退到 kretprobe/tcp_recvmsg（新内核）
        l_ret = bpf_program__attach_kprobe(ret_prog, true, "tcp_recvmsg");
        err_ret = libbpf_get_error(l_ret);
    }
    if (err_entry || err_ret) {
        LOG_ERROR(LogModule::NETWORK, "HttpLatencyMonitor: attach entry/retprobe failed"
                  << " entry_err=" << err_entry << " ret_err=" << err_ret);
        // 修复：如果成功 attach 了其中一个，需要销毁它（避免资源泄漏）
        if (l_entry && !err_entry) {
            LOG_INFO(LogModule::NETWORK, "HttpLatencyMonitor: destroying successful entry probe");
            bpf_link__destroy(l_entry);
        }
        if (l_ret && !err_ret) {
            LOG_INFO(LogModule::NETWORK, "HttpLatencyMonitor: destroying successful ret probe");
            bpf_link__destroy(l_ret);
        }
        impl_->link_entry = nullptr;
        impl_->link_recv = nullptr;
    } else {
        impl_->link_entry = l_entry;
        impl_->link_recv = l_ret;
        LOG_INFO(LogModule::NETWORK, "HttpLatencyMonitor: recv probe attached as entry+retprobe(tcp_recvmsg_locked)");
    }

    if (!impl_->link_send && !impl_->link_recv) {
        LOG_ERROR(LogModule::NETWORK, "HttpLatencyMonitor: both probes failed to attach");
        bpf_object__close(obj);
        impl_->link_send = impl_->link_recv = nullptr;
        available_ = false;
        initialized_ = true;
        stateSupport_.setState(EbpfMonitorState::Fallback, false, "all BPF probes failed to attach");
        return false;
    }

    impl_->obj = obj;
    available_ = true;
    initialized_ = true;

    LOG_INFO(LogModule::NETWORK, "HttpLatencyMonitor: initialized successfully");
    stateSupport_.setState(EbpfMonitorState::Attached, true, "BPF probes attached");
    stateSupport_.recordProbeAttached();
    stateSupport_.recordProbeAttached();
    stateSupport_.recordProbeAttached();
    return true;
#endif
}

/**
 * @brief 停止 HTTP 延迟监控器：销毁所有 BPF link 和 BPF 对象
 *
 * 释放所有 libbpf 资源（包括 send/entry/recv 三个 link）
 */
void HttpLatencyMonitor::stop() {
    stateSupport_.setState(EbpfMonitorState::Stopped, false, "stopped");
#if HAVE_LIBBPF
    if (impl_->link_send) { bpf_link__destroy(impl_->link_send); impl_->link_send = nullptr; }
    if (impl_->link_entry) { bpf_link__destroy(impl_->link_entry); impl_->link_entry = nullptr; }
    if (impl_->link_recv) { bpf_link__destroy(impl_->link_recv); impl_->link_recv = nullptr; }
#endif
    if (impl_->obj) {
        bpf_object__close(impl_->obj);
        impl_->obj = nullptr;
        LOG_INFO(LogModule::NETWORK, "HttpLatencyMonitor: stopped");
    }
    impl_->http_txn_stats_fd = -1;
    available_ = false;
}

/**
 * @brief 从 http_txn_stats Map 读取最近的 HTTP 事务记录
 *
 * 遍历 BPF Map 的所有条目（最多 32 次迭代，避免遍历过大），
 * 筛选已完成的事务（send_ns > 0 && recv_ns > 0），
 * 计算每条事务的 TTFB = recv_ns - send_ns。
 * 结果按 TTFB 降序排列，取前 limit 个。
 *
 * @param limit 最多返回的事务数
 * @return HTTP 事务信息列表，按 TTFB 降序排列
 */
std::vector<HttpTxnInfo> HttpLatencyMonitor::getRecentTxns(size_t limit) {
    std::vector<HttpTxnInfo> result;
#if HAVE_LIBBPF
    if (impl_->http_txn_stats_fd < 0) {
        stateSupport_.recordReadFailure("http_txn_stats map unavailable");
        return result;
    }

    auto started = std::chrono::steady_clock::now();
    static constexpr int MAX_ITER = 32;  // 最多遍历 32 个条目，防止阻塞
    int count = 0;
    tcp_conn_key cur_key = {}, next_key = {};
    while (count < MAX_ITER && bpf_map_get_next_key(impl_->http_txn_stats_fd, &cur_key, &next_key) == 0) {
        http_txn_record record = {};
        if (bpf_map_lookup_elem(impl_->http_txn_stats_fd, &next_key, &record) == 0) {
            // 只取已完成的事务（有响应：recv_ns > 0 且 send_ns > 0）
            if (record.recv_ns > 0 && record.send_ns > 0) {
                HttpTxnInfo info;
                char src_buf[INET_ADDRSTRLEN], dst_buf[INET_ADDRSTRLEN];
                struct in_addr sa{next_key.saddr}, da{next_key.daddr};
                inet_ntop(AF_INET, &sa, src_buf, sizeof(src_buf));
                inet_ntop(AF_INET, &da, dst_buf, sizeof(dst_buf));
                info.srcIp = src_buf;
                info.dstIp = dst_buf;
                info.srcPort = ntohs(next_key.sport);
                info.dstPort = ntohs(next_key.dport);
                info.ttfbNs = record.recv_ns - record.send_ns;  // TTFB = 响应首字节时间 - 请求发送时间
                info.reqBytes = record.req_bytes;
                info.respBytes = record.resp_bytes;
                info.statusCode = record.status_code;
                result.push_back(info);
            }
        }
        cur_key = next_key;
        count++;
    }
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
    stateSupport_.recordReadSuccess(static_cast<uint64_t>(elapsed), !result.empty());
    // 按 TTFB 降序排列，取前 limit 个（优先返回最慢的事务）
    std::sort(result.begin(), result.end(),
        [](const HttpTxnInfo& a, const HttpTxnInfo& b) {
            return a.ttfbNs > b.ttfbNs;
        });
    if (result.size() > limit) result.resize(limit);
    return result;
#else
    (void)limit;
    return result;
#endif
}

/**
 * @brief 计算百分位数（P50/P95/P99）
 *
 * 使用线性插值法（简化版：ceil(p/100 * n) - 1），
 * 对排序后的值取对应位置。
 *
 * @param values 原始数值列表（会被内部复制排序，不修改原列表）
 * @param p 百分位（0~100）
 * @return 对应百分位的值
 */
uint64_t HttpLatencyMonitor::percentile(const std::vector<uint64_t>& values, double p) {
    if (values.empty()) return 0;
    std::vector<uint64_t> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    // 百分位索引计算：ceil(p/100 * n) - 1，保证覆盖所有元素
    size_t idx = static_cast<size_t>(std::ceil(p / 100.0 * sorted.size())) - 1;
    if (idx >= sorted.size()) idx = sorted.size() - 1;  // 边界保护
    return sorted[idx];
}

/**
 * @brief 按目的 IP 聚合 HTTP 延迟统计
 *
 * 将所有 HTTP 事务按 dstIp 分组，分别计算每个主机的 P50/P95/P99/Max TTFB，
 * 并给出应用慢/正常的定性分析。
 *
 * @return 以目的 IP 为键的延迟统计 Map
 */
std::map<std::string, HttpLatencyStats> HttpLatencyMonitor::getByDstHost() {
    std::map<std::string, std::vector<uint64_t>> ttfbByHost;
    auto txns = getRecentTxns(65536);
    for (const auto& txn : txns) {
        ttfbByHost[txn.dstIp].push_back(txn.ttfbNs);
    }
    std::map<std::string, HttpLatencyStats> result;
    for (const auto& [host, values] : ttfbByHost) {
        HttpLatencyStats s;
        s.totalTxns = values.size();
        s.p50Ns = percentile(values, 50);
        s.p95Ns = percentile(values, 95);
        s.p99Ns = percentile(values, 99);
        s.maxNs = *std::max_element(values.begin(), values.end());
        // 基于 P99 TTFB 的延迟分析：
        //   P99 > 500ms → 主要应用慢
        //   P95 > 200ms → 轻度延迟
        //   其他 → 正常
        if (s.p99Ns > 500000000ULL)
            s.analysis = "主要应用慢 (P99 TTFB > 500ms)";
        else if (s.p95Ns > 200000000ULL)
            s.analysis = "轻度延迟 (P95 TTFB > 200ms)";
        else
            s.analysis = "正常";
        result[host] = s;
    }
    return result;
}

/**
 * @brief 获取全局 HTTP 延迟统计（聚合所有目的主机）
 *
 * 与 getByDstHost 逻辑类似，但将所有主机的事务合并到一个列表计算全局百分位。
 *
 * @return 全局 HTTP 延迟统计
 */
HttpLatencyStats HttpLatencyMonitor::getGlobalStats() {
    auto byHost = getByDstHost();
    HttpLatencyStats global;
    uint64_t total = 0;
    std::vector<uint64_t> all;
    for (const auto& [host, stats] : byHost) {
        total += stats.totalTxns;
        // 无法精确聚合，用各主机的 P50 作为全局近似（跳过此步，直接取全部事务）
    }
    // 直接拉取所有事务，避免多主机聚合的精度损失
    auto txns = getRecentTxns(65536);
    for (const auto& txn : txns)
        all.push_back(txn.ttfbNs);
    global.totalTxns = all.size();
    global.p50Ns = percentile(all, 50);
    global.p95Ns = percentile(all, 95);
    global.p99Ns = percentile(all, 99);
    global.maxNs = all.empty() ? 0 : *std::max_element(all.begin(), all.end());

    if (global.p99Ns > 500000000ULL)
        global.analysis = "主要应用慢 (P99 TTFB > 500ms)";
    else if (global.p95Ns > 200000000ULL)
        global.analysis = "轻度延迟 (P95 TTFB > 200ms)";
    else
        global.analysis = "正常";
    return global;
}

}  // namespace weaknet_dbus

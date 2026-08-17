// http_latency_monitor.cpp
// HTTP 请求级延迟监控器 - 用户态实现
// 从 BPF Map 读取 HTTP 事务记录，计算 TTFB

#include "http_latency_monitor.hpp"
#include "logger.hpp"

#include <cstring>
#include <algorithm>
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

// ---- 数据结构映射（与 BPF 端一致） ----

struct tcp_conn_key {
    __u32 saddr;
    __u32 daddr;
    __u16 sport;
    __u16 dport;
};

struct http_txn_record {
    __u64 send_ns;
    __u64 recv_ns;
    __u32 req_bytes;
    __u32 resp_bytes;
    __u16 status_code;
    __u8  is_request;
    __u8  padding;
};

// ---- 实现 ----

struct HttpLatencyMonitor::Impl {
    int http_txn_stats_fd = -1;
    struct bpf_object *obj = nullptr;
    struct bpf_link *link_send = nullptr;
    struct bpf_link *link_entry = nullptr;   // entry kprobe link
    struct bpf_link *link_recv = nullptr;
};

HttpLatencyMonitor::HttpLatencyMonitor()
    : impl_(std::make_unique<Impl>()) {}

HttpLatencyMonitor::~HttpLatencyMonitor() {
    stop();
}

bool HttpLatencyMonitor::init(const std::string& bpfObjPath) {
#if !HAVE_LIBBPF
    LOG_INFO(LogModule::NETWORK, "HttpLatencyMonitor: BPF not available (no libbpf)");
    available_ = false;
    initialized_ = true;
    return false;
#else
    LOG_INFO(LogModule::NETWORK, "HttpLatencyMonitor: loading BPF object from " << bpfObjPath);

    LIBBPF_OPTS(bpf_object_open_opts, opts);
    struct bpf_object *obj = bpf_object__open_file(bpfObjPath.c_str(), &opts);
    if (!obj) {
        LOG_ERROR(LogModule::NETWORK, "HttpLatencyMonitor: failed to open BPF object: " << bpfObjPath);
        available_ = false;
        initialized_ = true;
        return false;
    }

    if (bpf_object__load(obj) != 0) {
        LOG_ERROR(LogModule::NETWORK, "HttpLatencyMonitor: failed to load BPF object");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        return false;
    }

    impl_->http_txn_stats_fd = bpf_object__find_map_fd_by_name(obj, "http_txn_stats");
    if (impl_->http_txn_stats_fd < 0) {
        LOG_ERROR(LogModule::NETWORK, "HttpLatencyMonitor: http_txn_stats map not found");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        return false;
    }

    // attach 探针到 kprobe/tcp_sendmsg (出站请求) 和 kprobe/tcp_recvmsg_locked (响应)
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
        return false;
    }

    impl_->link_send = bpf_program__attach(send_prog);
    long err_send = libbpf_get_error(impl_->link_send);
    if (err_send) {
        LOG_ERROR(LogModule::NETWORK, "HttpLatencyMonitor: attach tcp_sendmsg failed err=" << err_send);
        impl_->link_send = nullptr;
    }

    // 响应侧：entry kprobe + retprobe 配对
    // entry probe 保存 sk+msg 到 BPF_MAP，retprobe 读取（kretprobe 的 PT_REGS_PARM1 是返回值不是入口参数）
    struct bpf_link *l_entry = bpf_program__attach_kprobe(entry_prog, false, "tcp_recvmsg_locked");
    struct bpf_link *l_ret = bpf_program__attach_kprobe(ret_prog, true, "tcp_recvmsg_locked");
    long err_entry = libbpf_get_error(l_entry);
    long err_ret = libbpf_get_error(l_ret);
    if (err_entry || err_ret) {
        LOG_ERROR(LogModule::NETWORK, "HttpLatencyMonitor: attach entry/retprobe failed"
                  << " entry_err=" << err_entry << " ret_err=" << err_ret);
        if (l_entry && !err_entry) bpf_link__destroy(l_entry);
        if (l_ret && !err_ret) bpf_link__destroy(l_ret);
        impl_->link_recv = nullptr;
    } else {
        impl_->link_entry = l_entry;
        impl_->link_recv = l_ret;
        // entry probe 的 link 也需要保存以避免泄漏，但不单独跟踪
        LOG_INFO(LogModule::NETWORK, "HttpLatencyMonitor: recv probe attached as entry+retprobe(tcp_recvmsg_locked)");
    }

    if (!impl_->link_send && !impl_->link_recv) {
        LOG_ERROR(LogModule::NETWORK, "HttpLatencyMonitor: both probes failed to attach");
        bpf_object__close(obj);
        impl_->link_send = impl_->link_recv = nullptr;
        available_ = false;
        initialized_ = true;
        return false;
    }

    impl_->obj = obj;
    available_ = true;
    initialized_ = true;

    LOG_INFO(LogModule::NETWORK, "HttpLatencyMonitor: initialized successfully");
    return true;
#endif
}

void HttpLatencyMonitor::stop() {
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

std::vector<HttpTxnInfo> HttpLatencyMonitor::getRecentTxns(size_t limit) {
    std::vector<HttpTxnInfo> result;
#if HAVE_LIBBPF
    if (!available_ || impl_->http_txn_stats_fd < 0)
        return result;

    tcp_conn_key cur_key = {}, next_key = {};
    while (bpf_map_get_next_key(impl_->http_txn_stats_fd, &cur_key, &next_key) == 0) {
        http_txn_record record = {};
        if (bpf_map_lookup_elem(impl_->http_txn_stats_fd, &next_key, &record) == 0) {
            // 只取已完成的事务（有响应）
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
                info.ttfbNs = record.recv_ns - record.send_ns;
                info.reqBytes = record.req_bytes;
                info.respBytes = record.resp_bytes;
                info.statusCode = record.status_code;
                result.push_back(info);
            }
        }
        cur_key = next_key;
    }
#endif
    // 按 TTFB 降序排列，取前 limit 个
    std::sort(result.begin(), result.end(),
        [](const HttpTxnInfo& a, const HttpTxnInfo& b) {
            return a.ttfbNs > b.ttfbNs;
        });
    if (result.size() > limit) result.resize(limit);
    return result;
}

uint64_t HttpLatencyMonitor::percentile(const std::vector<uint64_t>& values, double p) {
    if (values.empty()) return 0;
    std::vector<uint64_t> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    size_t idx = static_cast<size_t>(std::ceil(p / 100.0 * sorted.size())) - 1;
    if (idx >= sorted.size()) idx = sorted.size() - 1;
    return sorted[idx];
}

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
        // 分析：TTFB P99 > 500ms 说明应用慢，P99 < 200ms 说明正常
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

HttpLatencyStats HttpLatencyMonitor::getGlobalStats() {
    auto byHost = getByDstHost();
    HttpLatencyStats global;
    uint64_t total = 0;
    std::vector<uint64_t> all;
    for (const auto& [host, stats] : byHost) {
        total += stats.totalTxns;
        // 无法精确聚合，用各主机的 P50 作为全局近似
    }
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
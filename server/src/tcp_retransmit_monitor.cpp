// tcp_retransmit_monitor.cpp
// TCP 重传追踪监控器 - 用户态实现
// 从 BPF Map 读取重传事件和统计，替代 net_tcp.cpp 的 netlink dump

#include "tcp_retransmit_monitor.hpp"
#include "logger.hpp"

#include <cstring>
#include <cstdio>
#include <sstream>
#include <algorithm>
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

// ---- 数据结构映射（与 BPF 端一致） ----

struct tcp_conn_key {
    __u32 saddr;
    __u32 daddr;
    __u16 sport;
    __u16 dport;
} __attribute__((packed));

struct tcp_retrans_event {
    __u32 pid;
    __u32 tgid;
    __u64 timestamp_ns;
    __u32 segs_out;
    __u32 segs_retrans;
    __u32 sstate;
};

struct tcp_retrans_stats {
    __u64 total_retrans;
    __u64 total_segs;
    __u32 last_state;
};

// ---- 辅助函数 ----

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

struct TcpRetransMonitor::Impl {
    int retrans_events_fd = -1;  // retrans_events Map fd
    int retrans_stats_fd = -1;   // retrans_stats Map fd
    struct bpf_object *obj = nullptr;
    struct bpf_link *link_retrans = nullptr;
    struct bpf_link *link_send = nullptr;
};

TcpRetransMonitor::TcpRetransMonitor()
    : impl_(std::make_unique<Impl>()) {}

TcpRetransMonitor::~TcpRetransMonitor() {
    stop();
}

bool TcpRetransMonitor::init(const std::string& bpfObjPath) {
#if !HAVE_LIBBPF
    LOG_INFO(LogModule::TCP_LOSS, "TcpRetransMonitor: BPF not available (no libbpf)");
    available_ = false;
    initialized_ = true;
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
        return false;
    }

    // 加载 BPF 程序
    if (bpf_object__load(obj) != 0) {
        LOG_ERROR(LogModule::TCP_LOSS, "TcpRetransMonitor: failed to load BPF object");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        return false;
    }

    // 查找 Map
    impl_->retrans_events_fd = bpf_object__find_map_fd_by_name(obj, "retrans_events");
    if (impl_->retrans_events_fd < 0) {
        LOG_ERROR(LogModule::TCP_LOSS, "TcpRetransMonitor: retrans_events map not found");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        return false;
    }

    impl_->retrans_stats_fd = bpf_object__find_map_fd_by_name(obj, "retrans_stats");
    if (impl_->retrans_stats_fd < 0) {
        LOG_ERROR(LogModule::TCP_LOSS, "TcpRetransMonitor: retrans_stats map not found");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
        return false;
    }

    // attach 探针到 kprobe/tcp_retransmit_skb 和 kprobe/tcp_sendmsg
    struct bpf_program *retrans_prog = bpf_object__find_program_by_name(obj, "trace_tcp_retransmit");
    struct bpf_program *send_prog = bpf_object__find_program_by_name(obj, "trace_tcp_sendmsg");
    if (!retrans_prog || !send_prog) {
        LOG_ERROR(LogModule::TCP_LOSS, "TcpRetransMonitor: BPF program not found");
        bpf_object__close(obj);
        available_ = false;
        initialized_ = true;
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
        return false;
    }

    impl_->obj = obj;
    available_ = true;
    initialized_ = true;

    LOG_INFO(LogModule::TCP_LOSS, "TcpRetransMonitor: initialized successfully");
    return true;
#endif
}

void TcpRetransMonitor::stop() {
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

std::map<TcpConnKey, TcpRetransStats> TcpRetransMonitor::getStats() {
    std::map<TcpConnKey, TcpRetransStats> result;
#if HAVE_LIBBPF
    if (!available_ || impl_->retrans_stats_fd < 0)
        return result;

    // 遍历 retrans_stats Map
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
#endif
    return result;
}

double TcpRetransMonitor::computeLossRate() {
    auto stats = getStats();
    uint64_t totalRetrans = 0, totalSegs = 0;
    for (const auto& [key, val] : stats) {
        totalRetrans += val.totalRetrans;
        totalSegs += val.totalSegs;
    }
    if (totalSegs == 0) return 0.0;
    return (totalRetrans * 100.0) / totalSegs;
}

std::vector<std::pair<TcpConnKey, TcpRetransStats>> TcpRetransMonitor::getTopRetransConnections(size_t topN) {
    auto stats = getStats();
    std::vector<std::pair<TcpConnKey, TcpRetransStats>> vec(stats.begin(), stats.end());
    std::sort(vec.begin(), vec.end(),
        [](const auto& a, const auto& b) {
            return a.second.totalRetrans > b.second.totalRetrans;
        });
    if (vec.size() > topN) vec.resize(topN);
    return vec;
}

}  // namespace weaknet_dbus
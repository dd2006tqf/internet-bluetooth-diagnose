/**
 * @file bpf_map_sizing.cpp
 * @brief eBPF Map 容量运行时定标 —— 规格表与 libbpf 应用层
 *
 * 本文件分三部分：
 *   1) 各监控器的 MapSizeSpec 规格表（default_entries 严格等于 v1 编译期常量）
 *   2) resolveMonitorMaps / resolveScopePlan：按配置与物理内存解算容量
 *   3) applyMapSizingPlan：在 load 前把解算结果写入 bpf_object
 *
 * 缺省行为与 v1 完全一致：ram_bytes=0、预算为 0 或 mode 非法 → 全部 default_entries。
 */

#include "utils/bpf_map_sizing.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cstring>

#if defined(__linux__)
#  include <sys/sysinfo.h>
#endif

#if defined(__has_include)
#  if __has_include(<bpf/libbpf.h>)
#    define WEAKNET_HAVE_LIBBPF 1
extern "C" {
#    include <bpf/libbpf.h>
}
#  else
#    define WEAKNET_HAVE_LIBBPF 0
#  endif
#else
#  define WEAKNET_HAVE_LIBBPF 0
#endif

namespace weaknet {

namespace {

/// tcp_retransmit.bpf.o
const std::vector<MapSizeSpec> kTcpRetransSpecs = {
    {"retrans_stats",  12, 24,   8192,  65536, 262144},
    {"retrans_events", 12, 32,    256,   1024,  16384},
};

/// flow_rate.bpf.o（current_sec + process_stats）
/// default_entries 必须逐一等于 flow_rate.bpf.c 中该 map 的编译期 max_entries
/// （current_sec=65536、process_stats=65536），否则"未配置时与 v1 零变化"不成立。
const std::vector<MapSizeSpec> kProcessProfilerSpecs = {
    {"current_sec",   13, 24,   8192, 65536, 262144},
    {"process_stats",  4, 40,   2048, 65536, 262144},
};

/// http_latency.bpf.o
const std::vector<MapSizeSpec> kHttpLatencySpecs = {
    {"http_txn_stats",  36, 32,  2048, 8192, 65536},
    {"recvmsg_ctx_map",  4, 16,   512, 1024,  8192},
};

/// dns_monitor.bpf.o
const std::vector<MapSizeSpec> kDnsSpecs = {
    {"fd_resolvers",       16,  8,  512, 1024, 8192},
    {"pending_recv",        8, 48,  512, 1024, 8192},
    {"dns_self_endpoints",  8,  8,  128,  256, 2048},
};

/// tcp_conn_stats.bpf.o
const std::vector<MapSizeSpec> kTcpConnSpecs = {
    {"conn_start",  8,  8, 2048, 8192, 65536},
    {"conn_ports",  2, 16,   64,  128,  1024},
};

/// skb_drop.bpf.o
const std::vector<MapSizeSpec> kSkbDropSpecs = {
    {"drop_stats_map", 8, 16, 128, 256, 2048},
};

/// a2dp_media.bpf.o
const std::vector<MapSizeSpec> kBluetoothSpecs = {
    {"active_sessions", 8, 32, 32, 64, 512},
    {"bt_traffic",      8, 32, 32, 64, 512},
};

} // namespace

const std::vector<MapSizeSpec>& getMapSizingSpecs(MapSizingScope scope) {
    switch (scope) {
        case MapSizingScope::TcpRetrans:      return kTcpRetransSpecs;
        case MapSizingScope::ProcessProfiler: return kProcessProfilerSpecs;
        case MapSizingScope::HttpLatency:     return kHttpLatencySpecs;
        case MapSizingScope::Dns:             return kDnsSpecs;
        case MapSizingScope::TcpConn:         return kTcpConnSpecs;
        case MapSizingScope::SkbDrop:         return kSkbDropSpecs;
        case MapSizingScope::Bluetooth:       return kBluetoothSpecs;
    }
    return kSkbDropSpecs;
}

size_t totalPhysicalRamBytes() {
#if defined(__linux__)
    struct sysinfo info;
    if (::sysinfo(&info) != 0) return 0;
    return static_cast<size_t>(info.totalram) * static_cast<size_t>(info.mem_unit);
#else
    return 0;
#endif
}

std::vector<ResolvedMapSize> resolveMonitorMaps(const std::vector<MapSizeSpec>& specs,
                                                const MapSizingConfig& cfg,
                                                size_t ram_bytes) {
    std::vector<ResolvedMapSize> out;
    out.reserve(specs.size());

    auto clamp_to = [](unsigned long long v, uint32_t lo, uint32_t hi) -> uint32_t {
        if (v < lo) return lo;
        if (v > hi) return hi;
        return static_cast<uint32_t>(v);
    };
    auto fill_default = [&]() {
        out.clear();
        for (const auto& spec : specs) {
            out.push_back({spec.map_name, spec.default_entries, MapSizeSource::Default});
        }
    };

    // 模式可用性：非法 mode、RAM 不可用、预算为 0、fixed 未给条数 → 一律回落默认
    const bool fixed_usable = (cfg.mode == "fixed") && cfg.entries > 0;
    const bool auto_usable = (cfg.mode == "auto") && ram_bytes > 0 && cfg.ram_budget_bp > 0;
    if (!fixed_usable && !auto_usable) {
        fill_default();
        return out;
    }

    if (fixed_usable) {
        for (const auto& spec : specs) {
            out.push_back({spec.map_name,
                           clamp_to(cfg.entries, spec.min_entries, spec.max_entries),
                           MapSizeSource::Fixed});
        }
        return out;
    }

    // auto：总预算按 default_entries × 每 entry 字节数加权分摊到该监控器各 map
    auto per_entry_bytes = [](const MapSizeSpec& spec) -> unsigned long long {
        return static_cast<unsigned long long>(spec.key_size_bytes) +
               spec.value_size_bytes + kMapEntryOverheadBytes;
    };

    unsigned long long total_weight = 0;
    for (const auto& spec : specs) {
        total_weight += static_cast<unsigned long long>(spec.default_entries) * per_entry_bytes(spec);
    }
    const unsigned long long budget =
        static_cast<unsigned long long>(ram_bytes) * cfg.ram_budget_bp / 10000ULL;
    if (total_weight == 0 || budget == 0) {
        fill_default();
        return out;
    }

    for (const auto& spec : specs) {
        const unsigned long long weight =
            static_cast<unsigned long long>(spec.default_entries) * per_entry_bytes(spec);
        // 该 map 分摊到的字节数 ÷ 每 entry 字节数 = 目标条数
        const unsigned long long entries =
            budget * weight / total_weight / per_entry_bytes(spec);
        out.push_back({spec.map_name,
                       clamp_to(entries, spec.min_entries, spec.max_entries),
                       MapSizeSource::Auto});
    }
    return out;
}

MapSizingPlan resolveScopePlan(MapSizingScope scope, const MapSizingConfig& cfg,
                               size_t ram_bytes) {
    return resolveMonitorMaps(getMapSizingSpecs(scope), cfg, ram_bytes);
}

bool applyMapSizingPlan(bpf_object* obj, const std::vector<ResolvedMapSize>& plan) {
#if WEAKNET_HAVE_LIBBPF
    if (!obj) return false;
    bool all_ok = true;
    for (const auto& item : plan) {
        struct bpf_map* map = bpf_object__find_map_by_name(obj, item.map_name);
        if (!map) continue;  // 该对象不含此 map：跳过而非报错
        // set_max_entries 失败（如 map 已 load、值越界）时内核保留编译期容量；
        // 若静默吞掉，调用方会按计划值做水位判定而前提不成立。
        if (bpf_map__set_max_entries(map, item.max_entries) != 0) {
            LOG_WARNING(weaknet_dbus::LogModule::NETWORK,
                        "applyMapSizingPlan: set_max_entries failed for map "
                            << item.map_name << " (target=" << item.max_entries
                            << ")，内核将保留编译期容量");
            all_ok = false;
        }
    }
    return all_ok;
#else
    (void)obj;
    (void)plan;
    return false;
#endif
}

} // namespace weaknet

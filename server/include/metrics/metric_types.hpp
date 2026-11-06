#pragma once

#include <string>
#include <chrono>
#include <cstdint>

namespace weaknet {

enum class MetricId {
    RTT_MS,
    REACHABILITY_SUCCESS,
    JITTER_MS,
    TCP_LOSS_RATE,
    WIFI_LOSS_RATE,
    RSSI_DBM,
    TRAFFIC_BPS,
    TRAFFIC_PPS,
    TRAFFIC_FLOWS,
    BAND_CONFLICT,
    DNS_QUERIES,
    DNS_FAILURE_RATIO,
    DNS_LATENCY_P50_MS,
    DNS_INFLIGHT,
    TCP_RETRANS_LOSS_PCT,
    TCP_CONN_ACTIVE,
    TCP_CONN_ACCEPT_RATE,

    /// 采集端自身 CPU 压力（HOST 域，0~1）。
    ///
    /// 存在的理由不是"多一个指标"，而是回答一个现有证据链答不出的问题：
    /// eBPF 掉证据（capture_emit_failure_ratio 高）到底是**网络真的坏了**，
    /// 还是**板子 CPU 被打满、采集线程抢不到 CPU**。二者在旧证据上完全同形，
    /// 会把采集端过载误判成网络故障。
    ///
    /// 本指标把"观测者自身状态"变成一等证据，使归因可以往下分辨一层。
    /// 数据来源是 kmod/weaknet_cpu_stat（4 ms 粒度的 per-CPU 累计时间）。
    CPU_BUSY_RATIO
};

inline const char* metricIdToString(MetricId id) {
    switch (id) {
        case MetricId::RTT_MS: return "rtt_ms";
        case MetricId::REACHABILITY_SUCCESS: return "reachability_success";
        case MetricId::JITTER_MS: return "jitter_ms";
        case MetricId::TCP_LOSS_RATE: return "tcp_loss_rate";
        case MetricId::WIFI_LOSS_RATE: return "wifi_loss_rate";
        case MetricId::RSSI_DBM: return "rssi_dbm";
        case MetricId::TRAFFIC_BPS: return "traffic_bps";
        case MetricId::TRAFFIC_PPS: return "traffic_pps";
        case MetricId::TRAFFIC_FLOWS: return "traffic_flows";
        case MetricId::BAND_CONFLICT: return "band_conflict";
        case MetricId::DNS_QUERIES: return "dns_queries";
        case MetricId::DNS_FAILURE_RATIO: return "dns_failure_ratio";
        case MetricId::DNS_LATENCY_P50_MS: return "dns_latency_p50_ms";
        case MetricId::DNS_INFLIGHT: return "dns_inflight";
        case MetricId::TCP_RETRANS_LOSS_PCT: return "tcp_retrans_loss_pct";
        case MetricId::TCP_CONN_ACTIVE: return "tcp_conn_active";
        case MetricId::TCP_CONN_ACCEPT_RATE: return "tcp_conn_accept_rate";
        case MetricId::CPU_BUSY_RATIO: return "cpu_busy_ratio";
        default: return "unknown";
    }
}

enum class MetricState {
    VALID,
    STALE,
    UNAVAILABLE,
    NOT_APPLICABLE,
    ERROR
};

inline const char* metricStateToString(MetricState state) {
    switch (state) {
        case MetricState::VALID: return "VALID";
        case MetricState::STALE: return "STALE";
        case MetricState::UNAVAILABLE: return "UNAVAILABLE";
        case MetricState::NOT_APPLICABLE: return "NOT_APPLICABLE";
        case MetricState::ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

enum class MetricUnit {
    MILLISECONDS,
    PERCENT,
    DBM,
    BYTES_PER_SEC,
    PACKETS_PER_SEC,
    COUNT,
    BOOLEAN
};

enum class MetricScope {
    INTERFACE,
    GLOBAL,
    HOST,
    RESOLVER
};

enum class SourceSemantics {
    GAUGE,
    EVENT,
    INTERVAL_DELTA_RATE,
    CUMULATIVE_COUNTER
};

enum class PublishedSemantics {
    GAUGE,
    EVENT,
    INTERVAL_RATE
};

struct MetricDescriptor {
    MetricId id;
    std::string name;
    MetricUnit unit;
    MetricScope scope;
    SourceSemantics source_semantics;
    PublishedSemantics published_semantics;
    std::chrono::milliseconds expected_interval;
    std::chrono::milliseconds freshness;
};

inline MetricDescriptor getMetricDescriptor(MetricId id) {
    using namespace std::chrono_literals;
    switch (id) {
        case MetricId::RTT_MS:
            return {MetricId::RTT_MS, "rtt_ms", MetricUnit::MILLISECONDS, MetricScope::INTERFACE,
                    SourceSemantics::GAUGE, PublishedSemantics::GAUGE, 10000ms, 30000ms};
        case MetricId::REACHABILITY_SUCCESS:
            return {MetricId::REACHABILITY_SUCCESS, "reachability_success", MetricUnit::BOOLEAN, MetricScope::INTERFACE,
                    SourceSemantics::EVENT, PublishedSemantics::EVENT, 10000ms, 30000ms};
        case MetricId::JITTER_MS:
            return {MetricId::JITTER_MS, "jitter_ms", MetricUnit::MILLISECONDS, MetricScope::INTERFACE,
                    SourceSemantics::GAUGE, PublishedSemantics::GAUGE, 10000ms, 30000ms};
        case MetricId::TCP_LOSS_RATE:
            return {MetricId::TCP_LOSS_RATE, "tcp_loss_rate", MetricUnit::PERCENT, MetricScope::INTERFACE,
                    SourceSemantics::INTERVAL_DELTA_RATE, PublishedSemantics::INTERVAL_RATE, 10000ms, 30000ms};
        case MetricId::WIFI_LOSS_RATE:
            return {MetricId::WIFI_LOSS_RATE, "wifi_loss_rate", MetricUnit::PERCENT, MetricScope::INTERFACE,
                    SourceSemantics::CUMULATIVE_COUNTER, PublishedSemantics::INTERVAL_RATE, 10000ms, 30000ms};
        case MetricId::RSSI_DBM:
            return {MetricId::RSSI_DBM, "rssi_dbm", MetricUnit::DBM, MetricScope::INTERFACE,
                    SourceSemantics::GAUGE, PublishedSemantics::GAUGE, 10000ms, 30000ms};
        case MetricId::TRAFFIC_BPS:
            return {MetricId::TRAFFIC_BPS, "traffic_bps", MetricUnit::BYTES_PER_SEC, MetricScope::INTERFACE,
                    SourceSemantics::GAUGE, PublishedSemantics::GAUGE, 10000ms, 30000ms};
        case MetricId::TRAFFIC_PPS:
            return {MetricId::TRAFFIC_PPS, "traffic_pps", MetricUnit::PACKETS_PER_SEC, MetricScope::INTERFACE,
                    SourceSemantics::GAUGE, PublishedSemantics::GAUGE, 10000ms, 30000ms};
        case MetricId::TRAFFIC_FLOWS:
            return {MetricId::TRAFFIC_FLOWS, "traffic_flows", MetricUnit::COUNT, MetricScope::INTERFACE,
                    SourceSemantics::GAUGE, PublishedSemantics::GAUGE, 10000ms, 30000ms};
        case MetricId::BAND_CONFLICT:
            return {MetricId::BAND_CONFLICT, "band_conflict", MetricUnit::BOOLEAN, MetricScope::INTERFACE,
                    SourceSemantics::GAUGE, PublishedSemantics::GAUGE, 10000ms, 30000ms};
        case MetricId::DNS_QUERIES:
            return {MetricId::DNS_QUERIES, "dns_queries", MetricUnit::COUNT, MetricScope::GLOBAL,
                    SourceSemantics::EVENT, PublishedSemantics::EVENT, 10000ms, 30000ms};
        case MetricId::DNS_FAILURE_RATIO:
            return {MetricId::DNS_FAILURE_RATIO, "dns_failure_ratio", MetricUnit::PERCENT, MetricScope::GLOBAL,
                    SourceSemantics::GAUGE, PublishedSemantics::GAUGE, 10000ms, 30000ms};
        case MetricId::DNS_LATENCY_P50_MS:
            return {MetricId::DNS_LATENCY_P50_MS, "dns_latency_p50_ms", MetricUnit::MILLISECONDS, MetricScope::GLOBAL,
                    SourceSemantics::GAUGE, PublishedSemantics::GAUGE, 10000ms, 30000ms};
        case MetricId::DNS_INFLIGHT:
            return {MetricId::DNS_INFLIGHT, "dns_inflight", MetricUnit::COUNT, MetricScope::GLOBAL,
                    SourceSemantics::GAUGE, PublishedSemantics::GAUGE, 10000ms, 30000ms};
        case MetricId::TCP_RETRANS_LOSS_PCT:
            return {MetricId::TCP_RETRANS_LOSS_PCT, "tcp_retrans_loss_pct", MetricUnit::PERCENT, MetricScope::GLOBAL,
                    SourceSemantics::CUMULATIVE_COUNTER, PublishedSemantics::INTERVAL_RATE, 10000ms, 30000ms};
        case MetricId::TCP_CONN_ACTIVE:
            return {MetricId::TCP_CONN_ACTIVE, "tcp_conn_active", MetricUnit::COUNT, MetricScope::GLOBAL,
                    SourceSemantics::GAUGE, PublishedSemantics::GAUGE, 10000ms, 30000ms};
        case MetricId::TCP_CONN_ACCEPT_RATE:
            return {MetricId::TCP_CONN_ACCEPT_RATE, "tcp_conn_accept_rate", MetricUnit::COUNT, MetricScope::GLOBAL,
                    SourceSemantics::CUMULATIVE_COUNTER, PublishedSemantics::INTERVAL_RATE, 10000ms, 30000ms};
        case MetricId::CPU_BUSY_RATIO:
            /*
             * HOST 域首个投入使用的指标（MetricScope::HOST 在此之前只有
             * metrics_registry.hpp 的 scopeKey() 拼接逻辑，从无指标使用）。
             *
             * 语义：全系统 CPU 忙碌占比 0~1（多核按累计时间加权，不是算术平均，
             * 避免单核打满被 8 核平均稀释掉）。
             *
             * GAUGE：源数据是 /dev/weaknet_cpu_stat 的**累计**纳秒，但本驱动的
             * 插件内部已经做了区间差值并只发布比率，因此对外粒度是瞬时值。
             * 采样周期与 quality 线程对齐（15s），freshness 给两个周期，
             * 容许一次抖动而不至于让下游立刻判 STALE。
             */
            return {MetricId::CPU_BUSY_RATIO, "cpu_busy_ratio", MetricUnit::PERCENT, MetricScope::HOST,
                    SourceSemantics::CUMULATIVE_COUNTER, PublishedSemantics::GAUGE, 15000ms, 30000ms};
        default:
            return {id, "unknown", MetricUnit::COUNT, MetricScope::INTERFACE,
                    SourceSemantics::GAUGE, PublishedSemantics::GAUGE, 10000ms, 30000ms};
    }
}

} // namespace weaknet

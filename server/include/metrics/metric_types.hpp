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
    CPU_BUSY_RATIO,

    /// 收包软中断在**全部软中断**中所占的构成比（HOST 域，0~1）。
    ///
    /// 存在的理由：CPU_BUSY_RATIO 只能回答"板子忙不忙"，回答不了"忙在哪"。
    /// 采集端被打满时，两种成因的处置方向完全相反：
    ///   (a) 收包软中断占主导 → CPU 是在处理被观测链路的流量，
    ///       证据丢失与**观测到的流量本身**相关，不是无关进程捣乱；
    ///   (b) 其它软中断/用户态占主导 → 与网络无关的本机负载，
    ///       属于真正意义上的"自伤"。
    ///
    /// 数值边界（务必记住，否则会把结论读反）：
    ///   本指标是**软中断事件数的构成比**，不是时间占比，也不是饱和率。
    ///   板端实测空载时 NET_RX 仅占约 0.3%（TIMER/SCHED/RCU 这些定频
    ///   软中断在空闲时照常触发，把分母撑得很大）；线速小包洪水可到 99%。
    ///   因此"高"是按数量级判的，不是"过半数"。
    ///
    /// 单独看没有意义：一台跑大流量的机器 NET_RX 占比天然就高。它唯一的
    /// 用法是作为 CPU_BUSY_RATIO 的**限定词**，只在"CPU 已饱和"这个分支里
    /// 被咨询。因此它不参与任何评分与 veto，只进归因证据。
    ///
    /// 数据来源是 kmod/weaknet_softirq（per-CPU 十类软中断累计触发次数）。
    SOFTIRQ_NET_RX_SHARE
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
        case MetricId::SOFTIRQ_NET_RX_SHARE: return "softirq_net_rx_share";
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
        case MetricId::SOFTIRQ_NET_RX_SHARE:
            /*
             * 与 CPU_BUSY_RATIO 同为 HOST 域，但语义完全不同：那个是**时间**
             * 占比，这个是**事件计数**的构成比。同一个 8 核板子上两者会被
             * 一起读，绝不能因为单位同名就混用。
             *
             * GAUGE：源数据是 /dev/weaknet_softirq 的累计次数，本驱动的插件
             * 内部已做区间差值并只发布构成比，对外是瞬时值。
             *
             * 采样周期 15s、freshness 30s，与 CPU_BUSY_RATIO 严格对齐——
             * 两者要在同一轮评估里配对使用，若节奏不同就会出现「用 30 秒前
             * 的软中断构成比去解释 15 秒前的 CPU 忙碌比」这种错配。
             */
            return {MetricId::SOFTIRQ_NET_RX_SHARE, "softirq_net_rx_share", MetricUnit::PERCENT, MetricScope::HOST,
                    SourceSemantics::CUMULATIVE_COUNTER, PublishedSemantics::GAUGE, 15000ms, 30000ms};
        default:
            return {id, "unknown", MetricUnit::COUNT, MetricScope::INTERFACE,
                    SourceSemantics::GAUGE, PublishedSemantics::GAUGE, 10000ms, 30000ms};
    }
}

} // namespace weaknet

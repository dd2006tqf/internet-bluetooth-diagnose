#pragma once

/**
 * @file tcp_rtt_evaluator.hpp
 * @brief 被动 TCP RTT SLE — 无状态纯函数评估
 *
 * 回答的问题：真实业务 TCP 连接的内核平滑往返时延（srtt）处于什么水平？
 *
 * 语义边界（与既有 SLE 不重叠）：
 *   - Responsiveness SLE → 主动 ICMP/UDP 探测固定目标（测本地链路/网关）
 *   - TCP Connect SLE    → 三次握手建连成败与建连耗时
 *   - 本 SLE             → 已建立连接在数据传输阶段的内核 srtt 分位数
 *
 * 作为被动观测（目的地由用户业务决定，可能跨洲或连向慢速远端），
 * capability_level_negative 恒为 false，无权单独否决 INTERNET_ACCESS。
 */

#include "assurance/health_state.hpp"
#include "tcp_rtt_monitor.hpp"

namespace weaknet {

struct TcpRttEvaluatorConfig {
    double p50_degraded_ms{80.0};    // p50 ≥ 80ms  → DEGRADED
    double p50_bad_ms{200.0};        // p50 ≥ 200ms → BAD
    double p90_degraded_ms{200.0};   // p90 ≥ 200ms → DEGRADED
    double p90_bad_ms{500.0};        // p90 ≥ 500ms → BAD
    uint64_t min_samples{8};         // 最小 probe 样本门禁
    uint64_t min_connections{1};     // 最小活跃连接数门禁
};

class TcpRttEvaluator {
public:
    using Config = TcpRttEvaluatorConfig;

    static SleResult evaluate(const weaknet_dbus::TcpRttSummary& summary,
                              const Config& cfg = Config()) {
        SleResult res;
        res.applicability = Applicability::APPLICABLE;
        res.capability_level_negative = false;

        // 1. 样本门禁：证据不足 → UNKNOWN，绝不伪造 GOOD
        if (summary.totalSamples == 0 || summary.activeConns < cfg.min_connections) {
            res.state = HealthState::UNKNOWN;
            res.coverage = Coverage::NONE;
            res.reason = "no_tcp_rtt_observations";
            return res;
        }
        if (summary.totalSamples < cfg.min_samples) {
            res.state = HealthState::UNKNOWN;
            res.coverage = Coverage::PARTIAL;
            res.reason = "insufficient_tcp_rtt_samples";
            return res;
        }

        const double avg_ms = static_cast<double>(summary.avgRttUs) / 1000.0;
        const double p50_ms = static_cast<double>(summary.p50RttUs) / 1000.0;
        const double p90_ms = static_cast<double>(summary.p90RttUs) / 1000.0;
        const double p99_ms = static_cast<double>(summary.p99RttUs) / 1000.0;

        res.evidence.push_back({"p50_tcp_rtt_ms", p50_ms, "Passive TCP srtt p50 (ms)"});
        res.evidence.push_back({"p90_tcp_rtt_ms", p90_ms, "Passive TCP srtt p90 (ms)"});
        res.evidence.push_back({"p99_tcp_rtt_ms", p99_ms, "Passive TCP srtt p99 (ms)"});
        res.evidence.push_back({"avg_tcp_rtt_ms", avg_ms, "Passive TCP srtt average (ms)"});
        res.evidence.push_back({"active_tcp_connections",
                                static_cast<double>(summary.activeConns),
                                "Tracked TCP connections in LRU map"});

        res.coverage = Coverage::FULL_FOR_PROFILE;

        // 2. 状态阶梯（负面证据优先）
        if (p50_ms >= cfg.p50_bad_ms || p90_ms >= cfg.p90_bad_ms) {
            res.state = HealthState::BAD;
            res.reason = (p50_ms >= cfg.p50_bad_ms) ? "excessive_passive_tcp_rtt_p50"
                                                    : "excessive_passive_tcp_rtt_p90";
            return res;
        }
        if (p50_ms >= cfg.p50_degraded_ms || p90_ms >= cfg.p90_degraded_ms) {
            res.state = HealthState::DEGRADED;
            res.reason = (p50_ms >= cfg.p50_degraded_ms) ? "elevated_passive_tcp_rtt_p50"
                                                         : "elevated_passive_tcp_rtt_p90";
            return res;
        }

        res.state = HealthState::GOOD;
        res.reason = "passive_tcp_rtt_healthy";
        return res;
    }
};

}  // namespace weaknet

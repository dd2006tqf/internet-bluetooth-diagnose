#pragma once

/**
 * @file tcp_connect_evaluator.hpp
 * @brief TCP 建连 SLE — 无状态纯函数评估
 *
 * 回答的问题：DNS 已经解析出目标地址之后，客户端**能不能成功建立 TCP 连接**？
 *
 * 语义边界（与既有 SLE 不重叠）：
 *   - TCP 重传率/丢包 → Reliability SLE（不在此重复评价）
 *   - DNS 解析        → DNS Service SLE
 *   本 SLE 只看建连结果：成功率、建连时延、证据质量。
 *
 * Coverage 与 Evidence Quality 遵循与 DNS Service 相同的规则：
 *   - 直接坏证据（真实观察到 connect 失败）可在 PARTIAL coverage 下成立
 *   - 证据不足 → UNKNOWN，绝不伪造 GOOD
 *   - 观测不可靠 → UNKNOWN，绝不把观测故障嫁祸给网络
 */

#include "assurance/health_state.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace weaknet {

/// 单次建连观测（与 TcpConnectMonitor 的输出结构语义一致，避免跨层耦合）
struct TcpConnectSample {
    bool   success{false};
    double latency_ms{0.0};
};

struct TcpConnectEvaluatorConfig {
    double failure_ratio_degraded{0.05};          // 失败率 ≥5% → DEGRADED
    double failure_ratio_bad{0.20};               // 失败率 ≥20% → BAD
    double median_latency_degraded_ms{300.0};     // 建连中位时延 ≥300ms → DEGRADED
    double median_latency_bad_ms{1000.0};         // ≥1000ms → BAD
    uint64_t min_samples{5};                      // 最小样本门禁

    // Evidence Quality：观测器自身不可信时降 Coverage，不判业务故障
    double max_unmatched_ratio{0.10};             // 终态无对应 attempt 的比例上限
    double max_capture_loss_ratio{0.02};          // perf 投递丢失率上限
    uint64_t observer_min_events{20};             // 观测质量门禁最小样本量
};

/**
 * @brief TCP 建连 SLE 评估器
 *
 * 决策顺序与 DNS Service 保持一致：
 *   1. 样本门禁（不足 → UNKNOWN）
 *   2. Observer 质量（不可靠 → 有直接坏证据仍可判，否则 UNKNOWN）
 *   3. 失败率 / 中位时延阶梯
 */
class TcpConnectEvaluator {
public:
    using Config = TcpConnectEvaluatorConfig;

    struct Input {
        std::vector<TcpConnectSample> samples;
        uint64_t unmatched_terminal{0};   ///< 终态但无对应 attempt（证据不完整）
        uint64_t capture_events{0};       ///< 内核侧上报的事件总数
        uint64_t capture_lost{0};         ///< perf 投递丢失
    };

    static SleResult evaluate(const Input& in, const Config& cfg = Config()) {
        SleResult res;
        res.applicability = Applicability::APPLICABLE;

        // 1. 样本门禁：证据不足绝不伪造结论
        if (in.samples.size() < cfg.min_samples) {
            res.state = HealthState::UNKNOWN;
            res.coverage = in.samples.empty() ? Coverage::NONE : Coverage::PARTIAL;
            res.reason = in.samples.empty() ? "no_tcp_connect_observations"
                                            : "insufficient_tcp_connect_samples";
            return res;
        }

        uint64_t ok = 0, fail = 0;
        std::vector<double> latencies;
        latencies.reserve(in.samples.size());
        for (const auto& s : in.samples) {
            if (s.success) {
                ok++;
                latencies.push_back(s.latency_ms);
            } else {
                fail++;
            }
        }
        const uint64_t total = ok + fail;
        const double failure_ratio = total == 0 ? 0.0
            : static_cast<double>(fail) / static_cast<double>(total);
        const bool has_direct_negative = (fail > 0);

        res.evidence.push_back({"failure_ratio", failure_ratio * 100.0,
                                "TCP connect failure ratio %"});

        // 2. Observer 质量
        std::string observer_reason;
        const double unmatched_ratio = (total + in.unmatched_terminal) > 0
            ? static_cast<double>(in.unmatched_terminal) /
              static_cast<double>(total + in.unmatched_terminal)
            : 0.0;
        const double loss_ratio = (in.capture_events + in.capture_lost) > 0
            ? static_cast<double>(in.capture_lost) /
              static_cast<double>(in.capture_events + in.capture_lost)
            : 0.0;

        if (in.capture_events >= cfg.observer_min_events) {
            if (loss_ratio > cfg.max_capture_loss_ratio) {
                observer_reason = "observer_unreliable_event_loss";
            } else if (unmatched_ratio > cfg.max_unmatched_ratio) {
                observer_reason = "observer_unreliable_pairing_incomplete";
            }
        }

        if (!observer_reason.empty()) {
            res.evidence.push_back({"unmatched_ratio", unmatched_ratio * 100.0,
                                    "Terminal without matching attempt %"});
            res.evidence.push_back({"capture_loss_ratio", loss_ratio * 100.0,
                                    "Perf delivery loss %"});
            // 观测不可靠且无直接坏证据 → 不知道，绝不说好
            if (!has_direct_negative) {
                res.state = HealthState::UNKNOWN;
                res.coverage = Coverage::PARTIAL;
                res.reason = observer_reason;
                return res;
            }
        }

        // coverage：观测不可靠但有直接坏证据时降为 PARTIAL
        res.coverage = observer_reason.empty() ? Coverage::FULL_FOR_PROFILE : Coverage::PARTIAL;

        // 3. 时延中位数
        double median_latency = 0.0;
        if (!latencies.empty()) {
            std::sort(latencies.begin(), latencies.end());
            const size_t mid = latencies.size() / 2;
            median_latency = (latencies.size() % 2 == 0)
                ? (latencies[mid - 1] + latencies[mid]) / 2.0
                : latencies[mid];
            res.evidence.push_back({"median_connect_latency_ms", median_latency,
                                    "Median TCP connect latency"});
        }

        // 4. 状态阶梯（负面证据优先）
        if (failure_ratio >= cfg.failure_ratio_bad) {
            res.state = HealthState::BAD;
            res.reason = "high_connect_failure_rate";
            return res;
        }
        if (median_latency >= cfg.median_latency_bad_ms) {
            res.state = HealthState::BAD;
            res.reason = "excessive_connect_latency";
            return res;
        }
        if (failure_ratio >= cfg.failure_ratio_degraded) {
            res.state = HealthState::DEGRADED;
            res.reason = "elevated_connect_failure_rate";
            return res;
        }
        if (median_latency >= cfg.median_latency_degraded_ms) {
            res.state = HealthState::DEGRADED;
            res.reason = "elevated_connect_latency";
            return res;
        }

        res.state = HealthState::GOOD;
        res.reason = "tcp_connect_healthy";
        return res;
    }
};

}  // namespace weaknet

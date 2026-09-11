#pragma once

#include "assurance/health_state.hpp"
#include "assurance/dns_types.hpp"
#include <chrono>
#include <algorithm>
#include <numeric>

namespace weaknet {

struct DnsEvaluatorConfig {
    double error_ratio_degraded{0.05};               // 失败率 >= 5% -> DEGRADED
    double error_ratio_bad{0.20};                    // 失败率 >= 20% -> BAD
    double median_latency_degraded_ms{150.0};        // 中位数时延 >= 150ms -> DEGRADED
    double median_latency_bad_ms{500.0};             // 中位数时延 >= 500ms -> BAD
    uint64_t min_terminals_for_evaluation{5};        // 最少终态事务门禁
    double min_classification_coverage{0.70};        // SR-8: 分类覆盖率门禁 (evaluable / (evaluable + other + truncated))

    // SR-9 突发连续超时单杀规则（修正 #1）
    size_t critical_timeout_count{3};                // 连续 3 次超时
    std::chrono::milliseconds critical_window{15000};// 15s 窗口内
};

/**
 * @brief DNS 服务健康 SLE 评估器 (无状态纯函数，SR-2, SR-9, SR-10, SR-14)
 *
 * 核心逻辑：
 * 1. Missingness 三态判定 (SR-13 物理依据):
 *    - query_started == 0 && inflight == 0 -> UNKNOWN(no_dns_observations) [绝不伪造 GOOD]
 *    - inflight > 0 && evaluable_terminals == 0 -> UNKNOWN(awaiting_inflight)
 *    - terminals > 0 但不足门禁 -> UNKNOWN(insufficient_terminal_observations)
 * 2. SR-9 突发连续严重失败单杀判定 (修正 #1):
 *    - 同一 binding_epoch
 *    - >= 3 个独立事务 TIMEOUT_EXPIRED
 *    - 全部在 critical_window (15s) 内
 *    - 首尾两个 critical timeout 之间无任何 known_success terminal
 * 3. 失败率与中位数时延评估:
 *    - evaluable_terminals = known_success (NOERROR+NXDOMAIN) + known_failure (SERVFAIL+REFUSED+TIMEOUT)
 *    - failure_ratio = known_failure / evaluable_terminals
 *    - OTHER 和 TRUNCATED 不稀释 failure_ratio (SR-1, SR-14)
 *    - 计算有效时延的中位数 (p50)
 * 4. Tracking Quality 降级门禁 (SR-8):
 *    - classification_coverage 过低 -> UNKNOWN (insufficient_classified_outcomes)
 */
class DnsServiceEvaluator {
public:
    using Config = DnsEvaluatorConfig;

    static SleResult evaluate(const DnsMetricWindow& window,
                             const std::vector<DnsTransactionRecord>& recent_terminals,
                             const Config& cfg = Config(),
                             bool* out_is_critical_bypass = nullptr) {
        SleResult res;
        res.applicability = Applicability::APPLICABLE;
        if (out_is_critical_bypass) *out_is_critical_bypass = false;

        // 1. Missingness 三态检查
        if (window.queries_started == 0 && window.current_inflight == 0) {
            res.state = HealthState::UNKNOWN;
            res.coverage = Coverage::NONE;
            res.reason = "no_dns_observations";
            return res;
        }

        uint64_t evaluable = window.evaluableTerminals();

        if (window.current_inflight > 0 && evaluable == 0) {
            res.state = HealthState::UNKNOWN;
            res.coverage = Coverage::PARTIAL;
            res.reason = "awaiting_inflight";
            return res;
        }

        // 2. SR-9 突发严重失败 Bypass 判定 (修正 #1: 连续 3 次超时且无成功穿插)
        if (checkCriticalTimeoutBypass(recent_terminals, window.binding_epoch, cfg.critical_timeout_count, cfg.critical_window, window.evaluation_cutoff)) {
            res.state = HealthState::BAD;
            res.coverage = Coverage::FULL_FOR_PROFILE;
            res.reason = "critical_burst_timeouts";
            res.evidence.push_back({"burst_timeouts", static_cast<double>(cfg.critical_timeout_count), "Burst consecutive timeouts detected without success"});
            if (out_is_critical_bypass) *out_is_critical_bypass = true;
            return res;
        }

        // 3. 终态门禁检查
        if (evaluable < cfg.min_terminals_for_evaluation) {
            res.state = HealthState::UNKNOWN;
            res.coverage = Coverage::PARTIAL;
            res.reason = "insufficient_terminal_observations";
            return res;
        }

        // 4. Tracking Quality 覆盖度检查 (SR-8)
        double coverage_ratio = window.classificationCoverage();
        if (coverage_ratio < cfg.min_classification_coverage) {
            res.state = HealthState::UNKNOWN;
            res.coverage = Coverage::PARTIAL;
            res.reason = "insufficient_classified_outcomes";
            res.evidence.push_back({"classification_coverage", coverage_ratio, "Ratio of classified outcomes is below threshold"});
            return res;
        }

        res.coverage = Coverage::FULL_FOR_PROFILE;

        // 5. 失败率与时延中位数计算
        double fail_ratio = window.failureRatio();
        res.evidence.push_back({"failure_ratio", fail_ratio, "Ratio of failed terminals over evaluable terminals"});

        double median_latency = 0.0;
        if (!window.latencies_ms.empty()) {
            std::vector<double> sorted_lat = window.latencies_ms;
            std::sort(sorted_lat.begin(), sorted_lat.end());
            size_t mid = sorted_lat.size() / 2;
            if (sorted_lat.size() % 2 == 0) {
                median_latency = (sorted_lat[mid - 1] + sorted_lat[mid]) / 2.0;
            } else {
                median_latency = sorted_lat[mid];
            }
            res.evidence.push_back({"median_latency_ms", median_latency, "Median transaction latency"});
        }

        // 6. 状态阶梯裁决
        if (fail_ratio >= cfg.error_ratio_bad) {
            res.state = HealthState::BAD;
            res.reason = "high_failure_rate";
            return res;
        }
        if (median_latency >= cfg.median_latency_bad_ms) {
            res.state = HealthState::BAD;
            res.reason = "excessive_dns_latency";
            return res;
        }

        if (fail_ratio >= cfg.error_ratio_degraded) {
            res.state = HealthState::DEGRADED;
            res.reason = "elevated_failure_rate";
            return res;
        }
        if (median_latency >= cfg.median_latency_degraded_ms) {
            res.state = HealthState::DEGRADED;
            res.reason = "elevated_dns_latency";
            return res;
        }

        res.state = HealthState::GOOD;
        res.reason = "dns_service_healthy";
        return res;
    }

private:
    static bool checkCriticalTimeoutBypass(const std::vector<DnsTransactionRecord>& terminals,
                                           uint64_t target_epoch,
                                           size_t burst_count,
                                           std::chrono::milliseconds window_dur,
                                           std::chrono::steady_clock::time_point cutoff) {
        if (terminals.size() < burst_count) return false;

        // 从最新的记录往回看
        size_t consecutive_timeouts = 0;
        std::chrono::steady_clock::time_point newest_timeout;
        std::chrono::steady_clock::time_point oldest_timeout;

        for (auto it = terminals.rbegin(); it != terminals.rend(); ++it) {
            const auto& rec = *it;
            if (rec.binding_epoch != target_epoch) {
                continue; // 跨 epoch 隔离
            }

            if (rec.state == DnsTransactionState::TIMEOUT_EXPIRED) {
                if (consecutive_timeouts == 0) {
                    newest_timeout = rec.completed_at;
                }
                oldest_timeout = rec.completed_at;
                consecutive_timeouts++;

                if (consecutive_timeouts >= burst_count) {
                    // 检查全部在 window_dur 且距离 cutoff 不超时
                    if (cutoff >= newest_timeout && (cutoff - oldest_timeout) <= window_dur) {
                        return true;
                    }
                    // 超出窗口则不满足
                    break;
                }
            } else if (rec.state == DnsTransactionState::NOERROR || rec.state == DnsTransactionState::NXDOMAIN) {
                // 首尾两个 critical timeout 之间若出现 known_success，打断突发连续判定
                break;
            }
        }

        return false;
    }
};

} // namespace weaknet

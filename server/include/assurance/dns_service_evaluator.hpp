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

    // Observer 质量门禁：观测器自身丢证据时不允许把观测缺陷伪装成 DNS 故障。
    // 直接负面证据（SERVFAIL/REFUSED）不受观测丢失影响，仍可成立；只有
    // absence-derived 的超时判定会被观测丢失污染。阈值由真机负载矩阵校准得出。
    double max_capture_emit_failure_ratio{0.02};     // BPF capture 输出失败率上限
    double max_perf_delivery_loss_ratio{0.02};       // perf buffer 投递丢失率上限
    double max_ambiguity_ratio{0.10};                // 匹配歧义率上限
    double max_insert_failure_ratio{0.02};           // Tracker 容量溢出率上限
    uint64_t observer_min_capture_attempts{20};      // 观测质量门禁的最小样本量
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
 *
 * capability_level_negative（是否具备否决 Internet Access 的资格）：
 *   只有"本机名字解析能力失效"的证据才置位 —— 即
 *     - SR-9 连续超时（解析器完全不响应），或
 *     - 失败以 TIMEOUT 为主（解析器答不出来），或
 *     - 解析普遍极慢
 *   单域名/单次 SERVFAIL、REFUSED 不置位：那可能是域名自身或策略侧问题，
 *   无权把"某个域名解析失败"升级为"Internet 不可用"。
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
        // DNS 证据来自真实 DNS 事务（被动观测），scope 是本机解析能力。
        res.source = EvidenceSource::PASSIVE_REAL_TRAFFIC;
        res.scope = EvidenceScope::HOST_RESOLVER_CAPABILITY;
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
            // 连续 3 个独立事务无任何响应且无成功穿插 → 解析器能力级故障
            res.capability_level_negative = true;
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

        // 4b. Observer 质量评估。注意：这里**不立即返回**。
        //     直接负面证据（SERVFAIL/REFUSED 等真实捕获的错误响应）即使观测覆盖不完美也成立，
        //     只有"缺席推导"的故障（TIMEOUT）才会被观测丢失污染。
        //     因此观测不可靠时：有直接坏证据 -> 继续裁决但降 coverage；
        //                       无直接坏证据 -> UNKNOWN，绝不把观测缺陷伪装成 DNS 故障。
        std::string observer_reason = checkObserverQuality(window, cfg);
        const bool observer_unreliable = !observer_reason.empty();
        const bool has_direct_negative = window.directNegativeEvidence() > 0;
        if (observer_unreliable) {
            res.evidence.push_back({"capture_emit_failure_ratio", window.captureEmitFailureRatio(),
                                    "BPF capture emit failure ratio"});
            res.evidence.push_back({"perf_delivery_loss_ratio", window.perfDeliveryLossRatio(),
                                    "Perf buffer delivery loss ratio"});
            res.evidence.push_back({"ambiguity_ratio", window.ambiguityRatio(),
                                    "Transaction matching ambiguity ratio"});
            res.evidence.push_back({"insert_failure_ratio", window.insertFailureRatio(),
                                    "Tracker capacity overflow ratio"});
        }
        if (observer_unreliable && !has_direct_negative) {
            res.state = HealthState::UNKNOWN;
            res.coverage = Coverage::PARTIAL;
            res.reason = observer_reason;
            return res;
        }

        // coverage：观测不可靠但有直接坏证据时降级为 PARTIAL，故障结论仍成立
        res.coverage = observer_unreliable ? Coverage::PARTIAL : Coverage::FULL_FOR_PROFILE;
        if (observer_unreliable) {
            res.evidence.push_back({"observer_warning", 1.0,
                                    "Direct negative evidence is trustworthy, but observer quality is degraded"});
        }

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
        // 失败率按**类型**区分是否具备 host-level 否决权。
        //
        // 关键区分：解析器"答不答"与"答什么"是两件事。
        //   TIMEOUT  —— 解析器完全没响应 => 本机解析能力失效（capability 级）
        //   SERVFAIL —— 解析器答了，但上游/权威侧解析失败。可能是该域名自身
        //               或权威侧问题，不能自动等同于本机能力故障
        //   REFUSED  —— 明确的策略拒绝，几乎总是域名/策略侧问题
        //
        // 因此不能用聚合 failure_ratio 触发否决权：小样本下单个域名的
        // SERVFAIL 就可能超过 20% 阈值，从而把一个域名的问题升级成
        // "Internet 不可用"。否决权只由 timeout 占比决定。
        const double timeout_ratio = evaluable > 0
            ? static_cast<double>(window.timeouts) / static_cast<double>(evaluable)
            : 0.0;

        if (fail_ratio >= cfg.error_ratio_bad) {
            res.state = HealthState::BAD;
            res.reason = "high_failure_rate";
            // 仅当失败以"无响应"为主时，才认定是本机解析能力故障
            res.capability_level_negative = (timeout_ratio >= cfg.error_ratio_bad);
            res.evidence.push_back({"timeout_ratio", timeout_ratio * 100.0,
                                    "Timeout share of evaluable terminals %"});
            return res;
        }
        if (median_latency >= cfg.median_latency_bad_ms) {
            res.state = HealthState::BAD;
            // 解析普遍变慢同样是本机解析能力的退化
            res.capability_level_negative = true;
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
    /**
     * @brief 观测质量检查。返回空字符串表示观测可信；否则返回机器可读的降级原因。
     *
     * 两级传输分别计量（BPF capture 与 perf buffer），不合并成单一数字——
     * 两者是否描述同一轮拥塞尚未验证，合并会掩盖真实边界。
     * 样本不足时不做判定，避免小样本误判。
     */
    static std::string checkObserverQuality(const DnsMetricWindow& window, const Config& cfg) {
        if (window.capture_attempts < cfg.observer_min_capture_attempts &&
            window.delivered_events == 0) {
            return {}; // 样本不足，不判定观测质量
        }
        if (window.captureEmitFailureRatio() > cfg.max_capture_emit_failure_ratio) {
            return "observer_unreliable_capture_emit_failure";
        }
        if (window.perfDeliveryLossRatio() > cfg.max_perf_delivery_loss_ratio) {
            return "observer_unreliable_event_loss";
        }
        if (window.ambiguityRatio() > cfg.max_ambiguity_ratio) {
            return "observer_unreliable_ambiguity";
        }
        if (window.insertFailureRatio() > cfg.max_insert_failure_ratio) {
            return "tracker_capacity_overflow";
        }
        return {};
    }

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

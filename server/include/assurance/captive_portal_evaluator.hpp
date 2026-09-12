#pragma once

/**
 * @file captive_portal_evaluator.hpp
 * @brief Captive Portal SLE — 无状态纯函数评估
 *
 * 为什么需要独立维度：
 *   Captive Portal（认证门户）的表现是"HTTP 能通、但拿到的是认证页而不是
 *   预期内容"。它既不是 DNS 故障、也不是 TCP 故障、也不是 HTTP 传输故障，
 *   必须与它们区分，否则根因会被错误归到别处。
 *
 * 判定依据（显式证据，不靠"HTTP 不成功"模糊推测）：
 *   1. 明确的门户特征重定向：3xx 且 Location 指向门户/认证路径
 *   2. 探测预期得到固定结果，却持续得到认证页特征响应
 *   3. 有网络层证据支持：IP 可达、DNS 可解析、TCP 可建连
 *      —— 否则应先怀疑底层故障而非门户
 *
 * 语义边界：本 SLE 只回答"是否被认证门户拦截"。
 *   若证据不足或特征不明确 → UNKNOWN，绝不猜测。
 */

#include "assurance/health_state.hpp"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace weaknet {

/// 一次 HTTP 探测的原始事实（供门户判定）
struct CaptivePortalProbe {
    uint16_t status_code{0};        ///< 0 表示无响应
    bool     redirect_to_portal{false}; ///< 3xx 且 Location 具备门户特征
    bool     expected_content{false};   ///< 是否拿到预期内容（而非门户页）
};

struct CaptivePortalEvaluatorConfig {
    uint64_t min_samples{3};              ///< 最小探测样本
    double portal_ratio_threshold{0.60};  ///< 门户特征占比达到该阈值即判定
};

class CaptivePortalEvaluator {
public:
    using Config = CaptivePortalEvaluatorConfig;

    struct Input {
        std::vector<CaptivePortalProbe> probes;
        /// 网络层前置条件：仅当底层可用时，门户判定才有意义
        bool ip_reachable{false};
        bool dns_resolvable{false};
        bool tcp_connectable{false};
        uint64_t capture_events{0};
        uint64_t capture_lost{0};
    };

    static SleResult evaluate(const Input& in, const Config& cfg = Config()) {
        SleResult res;
        res.applicability = Applicability::APPLICABLE;

        // 1. 样本门禁
        if (in.probes.size() < cfg.min_samples) {
            res.state = HealthState::UNKNOWN;
            res.coverage = in.probes.empty() ? Coverage::NONE : Coverage::PARTIAL;
            res.reason = in.probes.empty() ? "no_captive_portal_probes"
                                           : "insufficient_portal_probes";
            return res;
        }

        // 2. Observer 质量
        std::string observer_reason;
        if (in.capture_events >= 20) {
            const double loss = static_cast<double>(in.capture_lost) /
                static_cast<double>(in.capture_events + in.capture_lost);
            if (loss > 0.02) observer_reason = "observer_unreliable_event_loss";
        }
        if (!observer_reason.empty()) {
            res.state = HealthState::UNKNOWN;
            res.coverage = Coverage::PARTIAL;
            res.reason = observer_reason;
            return res;
        }
        res.coverage = Coverage::FULL_FOR_PROFILE;

        const uint64_t total = in.probes.size();
        uint64_t portal_signals = 0;
        uint64_t expected = 0;
        for (const auto& p : in.probes) {
            if (p.redirect_to_portal) portal_signals++;
            if (p.expected_content) expected++;
        }
        const double portal_ratio = static_cast<double>(portal_signals) / static_cast<double>(total);
        const double expected_ratio = static_cast<double>(expected) / static_cast<double>(total);

        res.evidence.push_back({"portal_signal_ratio", portal_ratio * 100.0, "Captive portal signal %"});
        res.evidence.push_back({"expected_content_ratio", expected_ratio * 100.0, "Expected content %"});

        // 3. 判定
        //    有明确门户特征 + 底层链路可用 → 确认为门户
        const bool link_ok = in.ip_reachable && in.dns_resolvable && in.tcp_connectable;
        if (portal_ratio >= cfg.portal_ratio_threshold) {
            if (!link_ok) {
                // 门户特征存在但底层链路本身有问题：不应归因门户，
                // 交由更底层 SLE 解释，此处不猜测。
                res.state = HealthState::UNKNOWN;
                res.coverage = Coverage::PARTIAL;
                res.reason = "portal_signal_without_healthy_underlay";
                return res;
            }
            res.state = HealthState::BAD;
            res.reason = "captive_portal_detected";
            return res;
        }

        // 拿到预期内容 → 无门户
        if (expected_ratio >= cfg.portal_ratio_threshold) {
            res.state = HealthState::GOOD;
            res.reason = "no_captive_portal";
            return res;
        }

        // 特征不明确：不猜测
        res.state = HealthState::UNKNOWN;
        res.coverage = Coverage::PARTIAL;
        res.reason = "portal_evidence_inconclusive";
        return res;
    }
};

}  // namespace weaknet

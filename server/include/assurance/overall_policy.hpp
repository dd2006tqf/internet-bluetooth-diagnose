#pragma once

#include "assurance/health_state.hpp"
#include "assurance/network_experience.hpp"

namespace weaknet {

/**
 * @brief 综合网络体验策略决策引擎 (HR-4)
 *
 * 核心三维度：IP Reachability, Reliability, Responsiveness
 * Advisory 维度：RF Health（不直接降低 Overall Experience；NOT_APPLICABLE 时完全忽略）
 *
 * 决策顺序（负面证据优先，coverage gate 仅保护 GOOD）：
 *   1. Reachability BAD        → BAD
 *   2. Reliability BAD         → BAD
 *   3. Responsiveness BAD      → DEGRADED
 *   4. 任一核心 SLE DEGRADED   → DEGRADED（负面证据可在 PARTIAL coverage 下成立）
 *   5. Minimum Core Coverage 不满足 → UNKNOWN
 *      （= Reachability known AND (Reliability known OR Responsiveness known)）
 *   6. 否则                    → GOOD
 *
 * NOT_APPLICABLE 的 SLE 不参与 vote、coverage 惩罚、warning 与 primary_issue。
 * Warnings 的唯一事实源即本函数产出的 NetworkExperience，下游（DbusService 等）
 * 不得再做二次业务判断。
 */
class OverallPolicy {
public:
    // 兼容 Phase 1 的重载 (无 dns 参数)
    static NetworkExperience decide(const std::string& iface,
                                   const SleResult& reach,
                                   const SleResult& resp,
                                   const SleResult& rel,
                                   const SleResult& rf,
                                   bool band_conflict = false) {
        return decide(iface, reach, resp, rel, rf, SleResult{}, AssessmentProfile::INTERNET_ACCESS, band_conflict);
    }

    // Phase 2A 完整签名
    static NetworkExperience decide(const std::string& iface,
                                   const SleResult& reach,
                                   const SleResult& resp,
                                   const SleResult& rel,
                                   const SleResult& rf,
                                   const SleResult& dns,
                                   AssessmentProfile profile = AssessmentProfile::INTERNET_ACCESS,
                                   bool band_conflict = false) {
        return decide(iface, reach, resp, rel, rf, dns, SleResult{}, profile, band_conflict);
    }

    // Stage 2：在 INTERNET_ACCESS 下把 TCP Connect 纳入 required service 链
    static NetworkExperience decide(const std::string& iface,
                                   const SleResult& reach,
                                   const SleResult& resp,
                                   const SleResult& rel,
                                   const SleResult& rf,
                                   const SleResult& dns,
                                   const SleResult& tcp_connect,
                                   AssessmentProfile profile,
                                   bool band_conflict = false) {
        return decide(iface, reach, resp, rel, rf, dns, tcp_connect, SleResult{}, SleResult{},
                      profile, band_conflict);
    }

    // Stage 2 完整签名：DNS → TCP → HTTP → Captive Portal
    static NetworkExperience decide(const std::string& iface,
                                   const SleResult& reach,
                                   const SleResult& resp,
                                   const SleResult& rel,
                                   const SleResult& rf,
                                   const SleResult& dns,
                                   const SleResult& tcp_connect,
                                   const SleResult& http_access,
                                   const SleResult& captive_portal,
                                   AssessmentProfile profile,
                                   bool band_conflict = false) {
        NetworkExperience exp;
        exp.iface = iface;
        exp.assessment_profile = profile;
        exp.ip_reachability = reach;
        exp.responsiveness = resp;
        exp.reliability = rel;
        exp.rf_health = rf;
        exp.dns_service = dns;
        exp.tcp_connect = tcp_connect;
        exp.http_access = http_access;
        exp.captive_portal = captive_portal;

        const bool reach_app = (reach.applicability == Applicability::APPLICABLE);
        const bool resp_app = (resp.applicability == Applicability::APPLICABLE);
        const bool rel_app = (rel.applicability == Applicability::APPLICABLE);
        const bool dns_app = (dns.applicability == Applicability::APPLICABLE &&
                              profile == AssessmentProfile::INTERNET_ACCESS &&
                              (dns.state != HealthState::UNKNOWN || dns.coverage != Coverage::NONE));
        const bool tcp_app = (tcp_connect.applicability == Applicability::APPLICABLE &&
                              profile == AssessmentProfile::INTERNET_ACCESS &&
                              (tcp_connect.state != HealthState::UNKNOWN || tcp_connect.coverage != Coverage::NONE));
        const bool http_app = (http_access.applicability == Applicability::APPLICABLE &&
                               profile == AssessmentProfile::INTERNET_ACCESS &&
                               (http_access.state != HealthState::UNKNOWN || http_access.coverage != Coverage::NONE));
        const bool portal_app = (captive_portal.applicability == Applicability::APPLICABLE &&
                                 profile == AssessmentProfile::INTERNET_ACCESS &&
                                 captive_portal.state != HealthState::UNKNOWN);

        // 计算整体 coverage：在适用的核心 SLE 上求值
        size_t core_applicable = 0, core_full = 0, core_none = 0;
        const SleResult* cores[6] = {&reach, &resp, &rel, &dns, &tcp_connect, &http_access};
        const bool core_apps[6] = {reach_app, resp_app, rel_app, dns_app, tcp_app, http_app};
        for (size_t i = 0; i < 6; ++i) {
            if (!core_apps[i]) continue;
            core_applicable++;
            if (cores[i]->coverage == Coverage::FULL_FOR_PROFILE) core_full++;
            if (cores[i]->coverage == Coverage::NONE) core_none++;
        }
        if (core_applicable == 0 || core_none == core_applicable) {
            exp.overall_coverage = Coverage::NONE;
        } else if (core_full == core_applicable) {
            exp.overall_coverage = Coverage::FULL_FOR_PROFILE;
        } else {
            exp.overall_coverage = Coverage::PARTIAL;
        }

        // Advisory 告警生成
        if (rf.applicability == Applicability::APPLICABLE &&
            (rf.state == HealthState::DEGRADED || rf.state == HealthState::BAD)) {
            exp.warnings.push_back("RF Health is suboptimal (weak signal)");
        }
        if (band_conflict) {
            exp.warnings.push_back("Potential 2.4GHz Wi-Fi and Bluetooth coexistence conflict detected");
        }
        if (profile == AssessmentProfile::NETWORK_ONLY && dns.applicability == Applicability::APPLICABLE &&
            (dns.state == HealthState::DEGRADED || dns.state == HealthState::BAD)) {
            exp.warnings.push_back("DNS service issue detected (Advisory under NETWORK_ONLY profile)");
        }

        // 1. Critical BAD (Reachability 或 Reliability 故障，一票否决)
        // SR-5 & 交接文档第 11 节优先级：
        // Reachability BAD + DNS BAD -> Overall BAD, Primary=IP_PATH_FAILURE, DNS=symptom
        if (reach_app && reach.state == HealthState::BAD) {
            exp.overall = HealthState::BAD;
            exp.primary_issue = "IP path is unreachable";
            if (dns_app && dns.state == HealthState::BAD) {
                exp.warnings.push_back("DNS failure suspected to be caused by IP path failure");
            }
            exp.display_score = 15;
            return exp;
        }
        if (rel_app && rel.state == HealthState::BAD) {
            exp.overall = HealthState::BAD;
            exp.primary_issue = "Severe packet loss or retransmission";
            exp.display_score = 30;
            return exp;
        }

        // Reachability GOOD + DNS BAD -> Overall BAD, Primary=DNS_SERVICE_FAILURE (仅在 INTERNET_ACCESS 下一票否决)
        if (dns_app && dns.state == HealthState::BAD) {
            exp.overall = HealthState::BAD;
            exp.primary_issue = "DNS service resolution failure";
            exp.display_score = 25;
            return exp;
        }

        // DNS 之后的下一层：TCP 建连失败（DNS 正常但连不上对端）
        if (tcp_app && tcp_connect.state == HealthState::BAD) {
            exp.overall = HealthState::BAD;
            exp.primary_issue = "TCP connection failure to remote endpoint";
            exp.display_score = 20;
            return exp;
        }

        // 明确被认证门户拦截：这是确定性事实，优先于 HTTP 服务质量归因
        if (portal_app && captive_portal.state == HealthState::BAD) {
            exp.overall = HealthState::BAD;
            exp.primary_issue = "Captive portal interception detected";
            exp.display_score = 20;
            return exp;
        }

        // HTTP 层失败（DNS/TCP 正常但应用层不可用）
        if (http_app && http_access.state == HealthState::BAD) {
            exp.overall = HealthState::BAD;
            exp.primary_issue = "HTTP service access failure";
            exp.display_score = 22;
            return exp;
        }

        // 2. Major BAD (Responsiveness 严重劣化 → DEGRADED)
        if (resp_app && resp.state == HealthState::BAD) {
            exp.overall = HealthState::DEGRADED;
            exp.primary_issue = "High latency or excessive jitter";
            exp.display_score = 55;
            return exp;
        }

        // 3. 任一核心 SLE DEGRADED（负面证据优先）
        bool reach_degraded = (reach_app && reach.state == HealthState::DEGRADED);
        bool rel_degraded = (rel_app && rel.state == HealthState::DEGRADED);
        bool resp_degraded = (resp_app && resp.state == HealthState::DEGRADED);
        bool dns_degraded = (dns_app && dns.state == HealthState::DEGRADED);
        bool tcp_degraded = (tcp_app && tcp_connect.state == HealthState::DEGRADED);
        bool http_degraded = (http_app && http_access.state == HealthState::DEGRADED);
        if (reach_degraded || rel_degraded || resp_degraded || dns_degraded || tcp_degraded || http_degraded) {
            exp.overall = HealthState::DEGRADED;
            if (reach_degraded) exp.primary_issue = "Intermittent probe failure";
            else if (rel_degraded) exp.primary_issue = "Elevated network packet loss";
            else if (dns_degraded) exp.primary_issue = "Elevated DNS resolution failure or latency";
            else if (tcp_degraded) exp.primary_issue = "Elevated TCP connection failure or latency";
            else if (http_degraded) exp.primary_issue = "Elevated HTTP access failure or latency";
            else exp.primary_issue = "Latency fluctuation detected";
            exp.display_score = 65;
            return exp;
        }

        // 4. Coverage Gate Minimum Core Coverage：无负面证据但核心 telemetry 不足时输出 UNKNOWN
        bool reach_known = (reach_app && reach.state != HealthState::UNKNOWN);
        bool rel_known = (rel_app && rel.state != HealthState::UNKNOWN);
        bool resp_known = (resp_app && resp.state != HealthState::UNKNOWN);
        if (!reach_known || (!rel_known && !resp_known)) {
            exp.overall = HealthState::UNKNOWN;
            exp.primary_issue = "Insufficient telemetry for network assessment";
            exp.display_score = 50;
            return exp;
        }

        // 5. Overall GOOD
        exp.overall = HealthState::GOOD;
        bool rf_clean = (rf.applicability == Applicability::NOT_APPLICABLE) ||
                        (rf.state == HealthState::GOOD);
        exp.display_score = rf_clean ? 95 : 85;
        return exp;
    }
};

} // namespace weaknet

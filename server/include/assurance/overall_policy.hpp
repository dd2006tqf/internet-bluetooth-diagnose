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
    static NetworkExperience decide(const std::string& iface,
                                   const SleResult& reach,
                                   const SleResult& resp,
                                   const SleResult& rel,
                                   const SleResult& rf,
                                   bool band_conflict = false) {
        NetworkExperience exp;
        exp.iface = iface;
        exp.ip_reachability = reach;
        exp.responsiveness = resp;
        exp.reliability = rel;
        exp.rf_health = rf;

        const bool reach_app = (reach.applicability == Applicability::APPLICABLE);
        const bool resp_app = (resp.applicability == Applicability::APPLICABLE);
        const bool rel_app = (rel.applicability == Applicability::APPLICABLE);

        // 计算整体 coverage：仅在适用（APPLICABLE）的核心 SLE 上求值，
        // NOT_APPLICABLE 不参与 coverage 惩罚。
        size_t core_applicable = 0, core_full = 0, core_none = 0;
        const SleResult* cores[3] = {&reach, &resp, &rel};
        const bool core_apps[3] = {reach_app, resp_app, rel_app};
        for (size_t i = 0; i < 3; ++i) {
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

        // Advisory 告警生成：NOT_APPLICABLE 的维度不产生 warning。
        if (rf.applicability == Applicability::APPLICABLE &&
            (rf.state == HealthState::DEGRADED || rf.state == HealthState::BAD)) {
            exp.warnings.push_back("RF Health is suboptimal (weak signal)");
        }
        if (band_conflict) {
            exp.warnings.push_back("Potential 2.4GHz Wi-Fi and Bluetooth coexistence conflict detected");
        }

        // 1. Critical BAD (Reachability 或 Reliability 任一严重故障，一票否决)
        if (reach_app && reach.state == HealthState::BAD) {
            exp.overall = HealthState::BAD;
            exp.primary_issue = "IP path is unreachable";
            exp.display_score = 15;
            return exp;
        }
        if (rel_app && rel.state == HealthState::BAD) {
            exp.overall = HealthState::BAD;
            exp.primary_issue = "Severe packet loss or retransmission";
            exp.display_score = 30;
            return exp;
        }

        // 2. Major BAD (Responsiveness 严重劣化 → DEGRADED)
        if (resp_app && resp.state == HealthState::BAD) {
            exp.overall = HealthState::DEGRADED; // Responsiveness 严重不佳时判 DEGRADED
            exp.primary_issue = "High latency or excessive jitter";
            exp.display_score = 55;
            return exp;
        }

        // 3. 任一核心 SLE DEGRADED（负面证据优先于 coverage gate，可在 PARTIAL 下成立）
        bool reach_degraded = (reach_app && reach.state == HealthState::DEGRADED);
        bool rel_degraded = (rel_app && rel.state == HealthState::DEGRADED);
        bool resp_degraded = (resp_app && resp.state == HealthState::DEGRADED);
        if (reach_degraded || rel_degraded || resp_degraded) {
            exp.overall = HealthState::DEGRADED;
            if (reach_degraded) exp.primary_issue = "Intermittent probe failure";
            else if (rel_degraded) exp.primary_issue = "Elevated network packet loss";
            else exp.primary_issue = "Latency fluctuation detected";
            exp.display_score = 68;
            return exp;
        }

        // 4. Coverage Gate Minimum Core Coverage：无负面证据但核心 telemetry 不足时，
        //    不允许输出 GOOD，只能 UNKNOWN。
        bool reach_known = (reach_app && reach.state != HealthState::UNKNOWN);
        bool rel_known = (rel_app && rel.state != HealthState::UNKNOWN);
        bool resp_known = (resp_app && resp.state != HealthState::UNKNOWN);
        if (!reach_known || (!rel_known && !resp_known)) {
            exp.overall = HealthState::UNKNOWN;
            exp.primary_issue = "Insufficient telemetry for network assessment";
            exp.display_score = 50;
            return exp;
        }

        // 5. Overall GOOD (RF 不佳仅产生 Warning，不杀死 Experience；
        //    NOT_APPLICABLE（如有线 RF）视为无射频负面影响)
        exp.overall = HealthState::GOOD;
        bool rf_clean = (rf.applicability == Applicability::NOT_APPLICABLE) ||
                        (rf.state == HealthState::GOOD);
        exp.display_score = rf_clean ? 95 : 85;
        return exp;
    }
};

} // namespace weaknet

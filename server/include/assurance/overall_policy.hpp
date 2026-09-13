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
        return decide(iface, reach, resp, rel, rf, dns, tcp_connect, http_access,
                      captive_portal, SleResult{}, SleResult{}, SleResult{}, SleResult{},
                      profile, band_conflict);
    }

    // Stage 2 + Active Probe：受控能力证据（唯一有资格判定 Internet 的
    // 服务层来源）与被动 observed experience 分开传入。
    static NetworkExperience decide(const std::string& iface,
                                   const SleResult& reach,
                                   const SleResult& resp,
                                   const SleResult& rel,
                                   const SleResult& rf,
                                   const SleResult& dns,
                                   const SleResult& tcp_connect,
                                   const SleResult& http_access,
                                   const SleResult& captive_portal,
                                   const SleResult& active_dns,
                                   const SleResult& active_tcp,
                                   const SleResult& active_https,
                                   const SleResult& active_portal,
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
        exp.active_dns = active_dns;
        exp.active_tcp = active_tcp;
        exp.active_https = active_https;
        exp.active_portal = active_portal;

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

        // DNS BAD：仅当证据属 "host-level resolver capability" 故障时才否决
        // INTERNET_ACCESS。本机解析不了名字，上网体验确实受损。
        // 若只是单域名 SERVFAIL/REFUSED（capability_level_negative=false），
        // 那可能是域名或策略侧问题，无权把 "某域名解析失败" 升级为
        // "Internet 不可用" —— 降为警告与证据，不改变 Overall。
        if (dns_app && dns.state == HealthState::BAD) {
            if (dns.capability_level_negative) {
                exp.overall = HealthState::BAD;
                exp.primary_issue = "DNS service resolution failure";
                exp.display_score = 25;
                return exp;
            }
            exp.warnings.push_back(
                "DNS resolution failures observed (per-domain, not resolver capability)");
        }

        // Active TCP capability：受控目标（由我们选定、应稳定可达）全部失败。
        // 这测的是 host-level Internet 能力，不是任意业务端点，
        // 因此**有资格**判定 INTERNET_ACCESS。
        const bool active_tcp_app = (active_tcp.applicability == Applicability::APPLICABLE &&
                                     profile == AssessmentProfile::INTERNET_ACCESS &&
                                     active_tcp.capability_level_negative);
        if (active_tcp_app && active_tcp.state == HealthState::BAD) {
            exp.overall = HealthState::BAD;
            exp.primary_issue = "Internet transport capability failed (all controlled targets)";
            exp.display_score = 18;
            return exp;
        }

        // Active DNS capability：所有受控目标名称解析失败
        const bool active_dns_app = (active_dns.applicability == Applicability::APPLICABLE &&
                                     profile == AssessmentProfile::INTERNET_ACCESS &&
                                     active_dns.capability_level_negative);
        if (active_dns_app && active_dns.state == HealthState::BAD) {
            exp.overall = HealthState::BAD;
            exp.primary_issue = "Internet name resolution capability failed (all controlled targets)";
            exp.display_score = 20;
            return exp;
        }

        // Active Captive Portal：受控 oracle 在 ≥2 个独立故障域上给出一致信号。
        //
        // 排在 Active HTTPS 之前，因为门户拦截本身会导致 TLS 证书校验失败
        // （被中间人替换的证书）。若先判 HTTPS 能力失败，门户场景就会被
        // 误报成"加密通道不可用"，根因定位错误。
        //
        // 无受控 oracle（scope == NO_CAPABILITY）时无权决策，
        // 只能由证据链展示 UNKNOWN。
        const bool active_portal_app =
            (active_portal.applicability == Applicability::APPLICABLE &&
             profile == AssessmentProfile::INTERNET_ACCESS &&
             active_portal.scope != EvidenceScope::NO_CAPABILITY &&
             active_portal.capability_level_negative);
        if (active_portal_app && active_portal.state == HealthState::BAD) {
            exp.overall = HealthState::BAD;
            exp.primary_issue = "Captive portal interception detected (controlled oracle)";
            exp.display_score = 20;
            return exp;
        }

        // Active HTTPS capability：受控目标全部无法完成 TLS + HTTP。
        //
        // 注意这里判的是"全部失败"，单个受控目标返回 404/500 不构成失败 ——
        // 收到任意合法 HTTP 状态码即证明 HTTPS transport 存在，
        // endpoint 自身的业务状态不是 Internet 的判决依据。
        const bool active_https_app =
            (active_https.applicability == Applicability::APPLICABLE &&
             profile == AssessmentProfile::INTERNET_ACCESS &&
             active_https.capability_level_negative);
        if (active_https_app && active_https.state == HealthState::BAD) {
            exp.overall = HealthState::BAD;
            exp.primary_issue =
                (active_https.reason == "active_https_cert_verification_failed")
                    ? "HTTPS certificate verification failed on all controlled targets"
                    : "Internet HTTPS capability failed (all controlled targets)";
            exp.display_score = 20;
            return exp;
        }

        // Portal 观测到信号但未达 quorum（单端点异常）：仅提示，不改变状态
        if (active_portal.reason == "portal_suspected_single_endpoint") {
            exp.warnings.push_back(
                "A controlled portal-check endpoint behaved unexpectedly, but the "
                "signal did not reach quorum across independent failure domains; "
                "not treated as a captive portal");
        }

        // Passive TCP 建连失败：**non-blocking observed service**。
        //
        // 被动 TCP 观测的是"本机连向任意目的地的结果"，而目的地由用户业务决定，
        // 可能本来就该失败（已下线服务器、被防火墙拦截的端口、不存在的服务）。
        // 因此它无权单独把 INTERNET_ACCESS 判为 BAD/DEGRADED —— 那等于用
        // 未知目的地的失败冒充 Internet ground truth。
        // 自身 SLE 可继续为 BAD 并展示证据，但 Overall 不受其影响。
        // 只有受控主动探测（ACTIVE_PROBE）才具备判定 Internet 可用性的资格。
        if (tcp_app && tcp_connect.state == HealthState::BAD) {
            exp.warnings.push_back(
                "Observed TCP connect failures on application endpoints (not an Internet availability verdict)");
        }

        // Captive Portal：仅当存在受控探测证据时才可确诊。
        // scope == NO_CAPABILITY 表示当前没有可靠的 portal 判定能力
        // （capture 不提取 Location，也没有主动 probe），此时无权做任何决策。
        const bool portal_has_capability =
            (captive_portal.scope != EvidenceScope::NO_CAPABILITY) &&
            (captive_portal.source != EvidenceSource::UNSPECIFIED);
        if (portal_has_capability && captive_portal.state == HealthState::BAD) {
            exp.overall = HealthState::BAD;
            exp.primary_issue = "Captive portal interception detected";
            exp.display_score = 20;
            return exp;
        }

        // Passive cleartext HTTP 失败：同样 non-blocking。
        // 当前 scope 仅覆盖明文 HTTP（TLS 后无法识别），且目的地由用户业务决定，
        // 不足以判定 Internet 可用性。
        if (http_app && http_access.state == HealthState::BAD) {
            exp.warnings.push_back(
                "Observed cleartext HTTP failures (scope: cleartext only, not an Internet availability verdict)");
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
        // 被动 Service SLE（TCP / Cleartext HTTP）即便 DEGRADED 也不改变 Overall：
        // 若允许它们把 Overall 压到 DEGRADED，本质上仍是用未知目的地的失败
        // 评价 Internet，只是从 BAD 改成 DEGRADED，语义漏洞并未消失。
        bool dns_degraded = (dns_app && dns.state == HealthState::DEGRADED);
        const bool tcp_degraded = (tcp_app && tcp_connect.state == HealthState::DEGRADED);
        const bool http_degraded = (http_app && http_access.state == HealthState::DEGRADED);
        if (tcp_degraded) {
            exp.warnings.push_back(
                "Observed TCP connect degradation on application endpoints (non-blocking)");
        }
        if (http_degraded) {
            exp.warnings.push_back(
                "Observed cleartext HTTP degradation (non-blocking, cleartext only)");
        }
        if (reach_degraded || rel_degraded || resp_degraded || dns_degraded) {
            exp.overall = HealthState::DEGRADED;
            if (reach_degraded) exp.primary_issue = "Intermittent probe failure";
            else if (rel_degraded) exp.primary_issue = "Elevated network packet loss";
            else if (dns_degraded) exp.primary_issue = "Elevated DNS resolution failure or latency";
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

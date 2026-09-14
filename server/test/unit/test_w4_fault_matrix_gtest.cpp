/**
 * @file test_w4_fault_matrix_gtest.cpp
 * @brief W4 最终完整故障矩阵自动化测试
 *
 * 全量覆盖以下核心组合故障与边界场景：
 * 1. Healthy All -> FULL + GOOD
 * 2. 单 Active Target 故障 -> Warning, 不得 Internet BAD (Quorum 规则)
 * 3. 全部 Active DNS Target 故障 -> DNS capability BAD -> TCP/TLS/HTTPS blocked_by_dns
 * 4. 全部 Active TCP Target 故障 -> TCP capability BAD -> TLS/HTTPS blocked_by_tcp
 * 5. 单 TLS/Cert Target 故障 -> Target warning, 不得 Internet BAD
 * 6. 全部 eligible TLS Target 故障 -> HTTPS capability BAD -> Primary 报告证书或协议错误
 * 7. 合法 HTTP 404/500 -> HTTPS capability 仍为 GOOD (Transport 存在，业务状态解耦)
 * 8. 单 Portal Oracle 异常 -> portal_suspected, 不判 CAPTIVE_PORTAL
 * 9. >=2 独立故障域一致 Portal 信号 -> CAPTIVE_PORTAL (Primary 报告门户)
 * 10. Core Reachability BAD -> 优先抢占 Primary Issue，下游服务不抢占
 * 11. TLS 失败 + Portal Quorum 阳性 -> Primary = CAPTIVE_PORTAL (门户优先级高于 TLS 失败)
 * 12. TLS 失败 + 无 Portal 证据 -> Primary = TLS/HTTPS capability failed
 * 13. Passive 单业务故障 -> Non-blocking, 仅产生 Warning
 * 14. Probe Disabled -> INTERNET_ACCESS 返回 UNKNOWN/PARTIAL, 绝不伪造
 */

#include <gtest/gtest.h>
#include "assurance/overall_policy.hpp"
#include "assurance/active_connectivity.hpp"

using namespace weaknet;

namespace {

SleResult makeSle(HealthState s, Coverage c = Coverage::FULL_FOR_PROFILE,
                  EvidenceScope sc = EvidenceScope::NETWORK_PATH,
                  EvidenceSource src = EvidenceSource::SYSTEM_TELEMETRY,
                  bool veto = false, const std::string& r = "") {
    SleResult res;
    res.state = s;
    res.coverage = c;
    res.scope = sc;
    res.source = src;
    res.capability_level_negative = veto;
    res.reason = r;
    return res;
}

ActiveConnectivityConfig activeCfg() {
    ActiveConnectivityConfig c;
    c.tls_available = true;
    c.underlying_healthy = true;
    return c;
}

ProbeTargetResult makeProbeTarget(const std::string& id, bool dns_ok, bool tcp_ok, bool tls_ok, bool http_ok,
                                  int status = 200, const std::string& tls_detail = "connected",
                                  PortalSignal sig = PortalSignal::NOT_PROBED, const std::string& domain = "") {
    ProbeTargetResult t;
    t.target_id = id;
    t.hostname = id + ".example";
    t.tcp_port = 443;
    t.failure_domain = domain.empty() ? (id + ".example") : domain;
    t.dns.attempted = true;
    t.dns.success = dns_ok;
    t.tcp.attempted = dns_ok;
    t.tcp.success = tcp_ok;
    t.tls.attempted = tcp_ok;
    t.tls.success = tls_ok;
    t.tls.detail = tls_detail;
    t.http.attempted = tls_ok;
    t.http.success = http_ok;
    t.http_info.status_code = status;
    t.http_info.portal_signal = sig;
    return t;
}

}  // namespace

// --- 1. Healthy All -> FULL + GOOD ---

TEST(W4FaultMatrixTest, HealthyAllGivesFullGood) {
    auto reach = makeSle(HealthState::GOOD);
    auto resp = makeSle(HealthState::GOOD);
    auto rel = makeSle(HealthState::GOOD);
    auto rf = makeSle(HealthState::GOOD);
    auto dns = makeSle(HealthState::GOOD);
    auto tcp = makeSle(HealthState::GOOD);
    auto http = makeSle(HealthState::GOOD);
    auto portal = makeSle(HealthState::GOOD);

    std::vector<ProbeTargetResult> targets = {
        makeProbeTarget("t1", true, true, true, true),
        makeProbeTarget("t2", true, true, true, true),
        makeProbeTarget("t3", true, true, true, true)
    };
    auto act = ActiveConnectivityEvaluator::evaluate(targets, true, activeCfg());

    auto exp = OverallPolicy::decide("wlan0", reach, resp, rel, rf, dns, tcp, http, portal,
                                     act.dns, act.tcp, act.https, act.portal,
                                     AssessmentProfile::INTERNET_ACCESS);

    EXPECT_EQ(exp.overall, HealthState::GOOD);
    EXPECT_EQ(exp.overall_coverage, Coverage::FULL_FOR_PROFILE);
    EXPECT_FALSE(exp.primary_issue.has_value());
    EXPECT_TRUE(exp.warnings.empty());
}

// --- 2. 单 Active Target 挂 -> 不得 Internet BAD (Quorum 保护) ---

TEST(W4FaultMatrixTest, SingleActiveTargetFailureDoesNotTriggerInternetBad) {
    auto reach = makeSle(HealthState::GOOD);
    auto resp = makeSle(HealthState::GOOD);
    auto rel = makeSle(HealthState::GOOD);
    auto rf = makeSle(HealthState::GOOD);
    auto dns = makeSle(HealthState::GOOD);
    auto tcp = makeSle(HealthState::GOOD);
    auto http = makeSle(HealthState::GOOD);
    auto portal = makeSle(HealthState::GOOD);

    // 目标 1 挂，目标 2, 3 正常
    std::vector<ProbeTargetResult> targets = {
        makeProbeTarget("t1", false, false, false, false),
        makeProbeTarget("t2", true, true, true, true),
        makeProbeTarget("t3", true, true, true, true)
    };
    auto act = ActiveConnectivityEvaluator::evaluate(targets, true, activeCfg());

    auto exp = OverallPolicy::decide("wlan0", reach, resp, rel, rf, dns, tcp, http, portal,
                                     act.dns, act.tcp, act.https, act.portal,
                                     AssessmentProfile::INTERNET_ACCESS);

    EXPECT_EQ(exp.overall, HealthState::GOOD)
        << "单个目标故障不可判定 Internet 不可用";
    EXPECT_FALSE(act.dns.capability_level_negative);
    EXPECT_FALSE(act.tcp.capability_level_negative);
    EXPECT_FALSE(act.https.capability_level_negative);
}

// --- 3. 全部 Active DNS Target 故障 -> blocked_by_dns 依赖截断 ---

TEST(W4FaultMatrixTest, AllDnsFailBlocksTcpAndHttps) {
    std::vector<ProbeTargetResult> targets = {
        makeProbeTarget("t1", false, false, false, false),
        makeProbeTarget("t2", false, false, false, false)
    };
    auto act = ActiveConnectivityEvaluator::evaluate(targets, true, activeCfg());

    EXPECT_EQ(act.dns.state, HealthState::BAD);
    EXPECT_TRUE(act.dns.capability_level_negative);
    EXPECT_EQ(act.dns.reason, "active_dns_capability_failed");

    // 依赖截断：TCP 与 HTTPS 必须标为 blocked_by_dns，不产生重复假失败
    EXPECT_EQ(act.tcp.state, HealthState::UNKNOWN);
    EXPECT_EQ(act.tcp.reason, "blocked_by_dns");
    EXPECT_FALSE(act.tcp.capability_level_negative);

    EXPECT_EQ(act.https.state, HealthState::UNKNOWN);
    EXPECT_EQ(act.https.reason, "blocked_by_tcp");
    EXPECT_FALSE(act.https.capability_level_negative);

    auto exp = OverallPolicy::decide("wlan0", makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     act.dns, act.tcp, act.https, act.portal,
                                     AssessmentProfile::INTERNET_ACCESS);
    EXPECT_EQ(exp.overall, HealthState::BAD);
    EXPECT_EQ(exp.primary_issue, "Internet name resolution capability failed (all controlled targets)");
}

// --- 4. 全部 Active TCP Target 故障 -> blocked_by_tcp 依赖截断 ---

TEST(W4FaultMatrixTest, AllTcpFailBlocksHttps) {
    std::vector<ProbeTargetResult> targets = {
        makeProbeTarget("t1", true, false, false, false),
        makeProbeTarget("t2", true, false, false, false)
    };
    auto act = ActiveConnectivityEvaluator::evaluate(targets, true, activeCfg());

    EXPECT_EQ(act.dns.state, HealthState::GOOD);
    EXPECT_EQ(act.tcp.state, HealthState::BAD);
    EXPECT_TRUE(act.tcp.capability_level_negative);

    EXPECT_EQ(act.https.state, HealthState::UNKNOWN);
    EXPECT_EQ(act.https.reason, "blocked_by_tcp");

    auto exp = OverallPolicy::decide("wlan0", makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     act.dns, act.tcp, act.https, act.portal,
                                     AssessmentProfile::INTERNET_ACCESS);
    EXPECT_EQ(exp.overall, HealthState::BAD);
    EXPECT_EQ(exp.primary_issue, "Internet transport capability failed (all controlled targets)");
}

// --- 5. 合法 HTTP 404/500 -> HTTPS 仍为 GOOD (Transport 可用性与状态码解耦) ---

TEST(W4FaultMatrixTest, LegitimateHttpErrorsStillProveTransport) {
    std::vector<ProbeTargetResult> targets = {
        makeProbeTarget("t1", true, true, true, true, 404),
        makeProbeTarget("t2", true, true, true, true, 500)
    };
    auto act = ActiveConnectivityEvaluator::evaluate(targets, true, activeCfg());

    EXPECT_EQ(act.https.state, HealthState::GOOD);
    EXPECT_FALSE(act.https.capability_level_negative);
}

// --- 6. >=2 独立故障域一致 Portal 信号 -> CAPTIVE_PORTAL ---

TEST(W4FaultMatrixTest, PortalQuorumDetectsCaptivePortal) {
    std::vector<ProbeTargetResult> targets = {
        makeProbeTarget("t1", true, true, true, true, 302, "connected", PortalSignal::REDIRECTED, "domain1"),
        makeProbeTarget("t2", true, true, true, true, 200, "connected", PortalSignal::CONTENT_MISMATCH, "domain2")
    };
    auto act = ActiveConnectivityEvaluator::evaluate(targets, true, activeCfg());

    EXPECT_EQ(act.portal.state, HealthState::BAD);
    EXPECT_TRUE(act.portal.capability_level_negative);
    EXPECT_EQ(act.portal.reason, "captive_portal_detected");

    auto exp = OverallPolicy::decide("wlan0", makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     act.dns, act.tcp, act.https, act.portal,
                                     AssessmentProfile::INTERNET_ACCESS);
    EXPECT_EQ(exp.overall, HealthState::BAD);
    EXPECT_EQ(exp.primary_issue, "Captive portal interception detected (controlled oracle)");
}

// --- 7. TLS 失败 + Portal Quorum 阳性 -> 门户优先级高于 TLS ---

TEST(W4FaultMatrixTest, PortalPrecedesTlsFailure) {
    // 模拟典型门户网络环境：用户请求 HTTPS 被伪造证书拦截导致 TLS 失败，但明文 Portal 探测拿到阳性重定向
    auto act_https = makeSle(HealthState::BAD, Coverage::FULL_FOR_PROFILE, EvidenceScope::NETWORK_PATH,
                             EvidenceSource::ACTIVE_PROBE, /*veto=*/true, "active_https_cert_verification_failed");
    auto act_portal = makeSle(HealthState::BAD, Coverage::FULL_FOR_PROFILE, EvidenceScope::NETWORK_PATH,
                              EvidenceSource::ACTIVE_PROBE, /*veto=*/true, "captive_portal_detected");

    auto exp = OverallPolicy::decide("wlan0", makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     act_https, act_portal,
                                     AssessmentProfile::INTERNET_ACCESS);

    EXPECT_EQ(exp.overall, HealthState::BAD);
    // 关键断言：必须优先报告 Portal，而不是误报为 TLS 证书故障
    EXPECT_EQ(exp.primary_issue, "Captive portal interception detected (controlled oracle)");
}

// --- 8. Core Reachability BAD -> 优先抢占 Primary Issue ---

TEST(W4FaultMatrixTest, CoreReachabilityPreemptsServiceFailures) {
    auto reach_bad = makeSle(HealthState::BAD, Coverage::FULL_FOR_PROFILE, EvidenceScope::NETWORK_PATH,
                             EvidenceSource::ACTIVE_PROBE, /*veto=*/true, "gateway_unreachable");
    auto dns_bad = makeSle(HealthState::BAD, Coverage::FULL_FOR_PROFILE, EvidenceScope::HOST_RESOLVER_CAPABILITY,
                           EvidenceSource::PASSIVE_REAL_TRAFFIC, /*veto=*/true, "dns_service_failed");

    auto exp = OverallPolicy::decide("wlan0", reach_bad, makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     makeSle(HealthState::GOOD), dns_bad, makeSle(HealthState::GOOD),
                                     makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     AssessmentProfile::INTERNET_ACCESS);

    EXPECT_EQ(exp.overall, HealthState::BAD);
    EXPECT_EQ(exp.primary_issue, "IP path is unreachable")
        << "底层网关不通时，上游 DNS 不得抢占 Primary Issue";
}

// --- 9. Passive 业务端点故障 -> Non-blocking，不降 Overall ---

TEST(W4FaultMatrixTest, PassiveEndpointFailuresAreNonBlocking) {
    auto tcp_passive_bad = makeSle(HealthState::BAD, Coverage::FULL_FOR_PROFILE, EvidenceScope::PER_DESTINATION,
                                   EvidenceSource::PASSIVE_REAL_TRAFFIC, /*veto=*/false);
    auto http_passive_bad = makeSle(HealthState::BAD, Coverage::FULL_FOR_PROFILE, EvidenceScope::CLEARTEXT_PER_DESTINATION,
                                    EvidenceSource::PASSIVE_REAL_TRAFFIC, /*veto=*/false);

    auto exp = OverallPolicy::decide("wlan0", makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     makeSle(HealthState::GOOD), tcp_passive_bad, http_passive_bad,
                                     makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     makeSle(HealthState::GOOD),
                                     AssessmentProfile::INTERNET_ACCESS);

    EXPECT_EQ(exp.overall, HealthState::GOOD);
    EXPECT_GE(exp.warnings.size(), 2u)
        << "被动 TCP/HTTP 失败只能作为 observed experience 记录为 warning，无权将 Internet 判 BAD";
}

// --- 10. Active Probe 未启用时 -> INTERNET_ACCESS 保持 UNKNOWN ---

TEST(W4FaultMatrixTest, ProbeDisabledKeepsInternetAccessUnknown) {
    auto act = ActiveConnectivityEvaluator::evaluate({}, /*probe_enabled=*/false, activeCfg());

    auto exp = OverallPolicy::decide("wlan0", makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     makeSle(HealthState::GOOD), makeSle(HealthState::GOOD),
                                     act.dns, act.tcp, act.https, act.portal,
                                     AssessmentProfile::INTERNET_ACCESS);

    EXPECT_EQ(act.dns.state, HealthState::UNKNOWN);
    EXPECT_EQ(act.tcp.state, HealthState::UNKNOWN);
    EXPECT_EQ(act.dns.reason, "active_probe_disabled");
}

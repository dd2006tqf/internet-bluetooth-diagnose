/**
 * @file test_active_connectivity_gtest.cpp
 * @brief Active Connectivity Probe 真值表
 *
 * 固化本轮七条验收标准的语义部分（网络 I/O 由真机验收覆盖）：
 *   1. 多目标 DNS/TCP 健康          → Active DNS/TCP = GOOD
 *   2. 单个 target 故障             → 不得判 Internet capability BAD
 *   3. 所有独立 TCP target 失败     → Active TCP = BAD 且 capability_level_negative
 *   4. DNS 失败                     → TCP = blocked_by_dns，不产生 TCP 假失败
 *   5. HTTPS                        → UNKNOWN/NONE/no_tls_probe_capability
 *   6. Portal                       → UNKNOWN/NONE/no_portal_probe_capability
 *   7. 探测未启用                   → UNKNOWN，绝不退回被动证据替代
 */

#include <gtest/gtest.h>
#include "assurance/active_connectivity.hpp"

using namespace weaknet;

namespace {

ProbeTargetResult makeTarget(const std::string& id,
                             bool dns_ok, bool tcp_ok,
                             bool tcp_attempted = true) {
    ProbeTargetResult t;
    t.target_id = id;
    t.hostname = id + ".example";
    t.tcp_port = 443;
    t.dns.attempted = true;
    t.dns.success = dns_ok;
    t.tcp.attempted = tcp_attempted;
    t.tcp.success = tcp_ok;
    return t;
}

}  // namespace

// --- 1. 多目标健康 ---

TEST(ActiveConnectivityTest, AllTargetsHealthyIsGood) {
    std::vector<ProbeTargetResult> targets = {
        makeTarget("a", true, true),
        makeTarget("b", true, true),
        makeTarget("c", true, true),
    };
    auto r = ActiveConnectivityEvaluator::evaluate(targets, /*probe_enabled=*/true);

    EXPECT_EQ(r.dns.state, HealthState::GOOD);
    EXPECT_EQ(r.dns.coverage, Coverage::FULL_FOR_PROFILE);
    EXPECT_EQ(r.dns.source, EvidenceSource::ACTIVE_PROBE);
    EXPECT_FALSE(r.dns.capability_level_negative);

    EXPECT_EQ(r.tcp.state, HealthState::GOOD);
    EXPECT_EQ(r.tcp.coverage, Coverage::FULL_FOR_PROFILE);
    EXPECT_FALSE(r.tcp.capability_level_negative);
}

// --- 2. 单个 target 故障不判定整体能力失效 ---

TEST(ActiveConnectivityTest, SingleTargetFailureDoesNotVerdictCapability) {
    // 一个目标失败，其余成功 → capability 仍存在
    std::vector<ProbeTargetResult> targets = {
        makeTarget("a", true, false),   // 该目标自身可能有问题
        makeTarget("b", true, true),
        makeTarget("c", true, true),
    };
    auto r = ActiveConnectivityEvaluator::evaluate(targets, true);

    EXPECT_EQ(r.tcp.state, HealthState::GOOD)
        << "只要还有受控目标成功，就不能宣称 capability 整体失效";
    EXPECT_FALSE(r.tcp.capability_level_negative);
    EXPECT_EQ(r.dns.state, HealthState::GOOD);
}

// --- 3. 全部独立目标失败 → 强负面证据 ---

TEST(ActiveConnectivityTest, AllTargetsFailingIsCapabilityNegative) {
    std::vector<ProbeTargetResult> targets = {
        makeTarget("a", true, false),
        makeTarget("b", true, false),
        makeTarget("c", true, false),
    };
    auto r = ActiveConnectivityEvaluator::evaluate(targets, true);

    EXPECT_EQ(r.tcp.state, HealthState::BAD);
    EXPECT_EQ(r.tcp.coverage, Coverage::FULL_FOR_PROFILE);
    EXPECT_TRUE(r.tcp.capability_level_negative)
        << "受控目标全部失败是 host-level capability 故障，应具否决权";
    EXPECT_EQ(r.tcp.reason, "active_tcp_capability_failed");
}

// --- 4. 依赖截断：DNS 失败不产生 TCP 假失败 ---

TEST(ActiveConnectivityTest, DnsFailureBlocksTcpWithoutFakeFailure) {
    std::vector<ProbeTargetResult> targets = {
        makeTarget("a", /*dns_ok=*/false, false, /*tcp_attempted=*/false),
        makeTarget("b", /*dns_ok=*/false, false, /*tcp_attempted=*/false),
    };
    auto r = ActiveConnectivityEvaluator::evaluate(targets, true);

    // DNS 是真正的失败
    EXPECT_EQ(r.dns.state, HealthState::BAD);
    EXPECT_TRUE(r.dns.capability_level_negative);

    // TCP 不得记为失败 —— 它根本没被执行，属上游阻断
    EXPECT_EQ(r.tcp.state, HealthState::UNKNOWN)
        << "DNS 失败时 TCP 不得产生假失败（一次故障不得污染多个 SLE）";
    EXPECT_EQ(r.tcp.reason, "blocked_by_dns");
    EXPECT_FALSE(r.tcp.capability_level_negative);
}

TEST(ActiveConnectivityTest, PartialDnsFailureGivesDnsGoodButTcpUnknown) {
    // 一个 target DNS 失败、另一个成功。
    // DNS：只要还有受控目标解析成功，就不能宣称解析能力整体失效 → GOOD
    // TCP：只有 1 个 target 走到 TCP 阶段，不满足"≥2 个独立目标参与"
    //      → UNKNOWN（不足以下结论），既不能 GOOD 也不能 BAD
    std::vector<ProbeTargetResult> targets = {
        makeTarget("a", false, false, false),
        makeTarget("b", true, true),
    };
    auto r = ActiveConnectivityEvaluator::evaluate(targets, true);
    EXPECT_EQ(r.dns.state, HealthState::GOOD);
    EXPECT_EQ(r.tcp.state, HealthState::UNKNOWN);
    EXPECT_EQ(r.tcp.reason, "insufficient_probe_targets");
    EXPECT_FALSE(r.tcp.capability_level_negative);
}

TEST(ActiveConnectivityTest, TwoEligibleTargetsOneFailingStillGood) {
    // ≥2 个 target 走到 TCP 阶段，其中一个失败 → 仍 GOOD（目标冗余生效）
    std::vector<ProbeTargetResult> targets = {
        makeTarget("a", true, false),
        makeTarget("b", true, true),
    };
    auto r = ActiveConnectivityEvaluator::evaluate(targets, true);
    EXPECT_EQ(r.tcp.state, HealthState::GOOD);
    EXPECT_FALSE(r.tcp.capability_level_negative);
}

// --- 5/6. HTTPS 与 Portal 明确为"无能力" ---

TEST(ActiveConnectivityTest, HttpsAndPortalReportNoCapability) {
    std::vector<ProbeTargetResult> targets = {
        makeTarget("a", true, true),
        makeTarget("b", true, true),
    };
    auto r = ActiveConnectivityEvaluator::evaluate(targets, true);

    EXPECT_EQ(r.https.state, HealthState::UNKNOWN);
    EXPECT_EQ(r.https.coverage, Coverage::NONE);
    EXPECT_EQ(r.https.scope, EvidenceScope::NO_CAPABILITY);
    EXPECT_EQ(r.https.reason, "no_tls_probe_capability");

    EXPECT_EQ(r.portal.state, HealthState::UNKNOWN);
    EXPECT_EQ(r.portal.coverage, Coverage::NONE);
    EXPECT_EQ(r.portal.reason, "no_portal_probe_capability");
}

// 关键：DNS+TCP 成功**不能**推出 HTTPS/Portal 成功
TEST(ActiveConnectivityTest, TransportSuccessDoesNotImplyHttpsSuccess) {
    std::vector<ProbeTargetResult> targets = {
        makeTarget("a", true, true),
        makeTarget("b", true, true),
        makeTarget("c", true, true),
    };
    auto r = ActiveConnectivityEvaluator::evaluate(targets, true);

    EXPECT_EQ(r.dns.state, HealthState::GOOD);
    EXPECT_EQ(r.tcp.state, HealthState::GOOD);
    // 绝不因传输层成功就宣称应用层可用
    EXPECT_NE(r.https.state, HealthState::GOOD)
        << "DNS+TCP 成功不能证明 HTTPS Internet Access 可用";
    EXPECT_NE(r.portal.state, HealthState::GOOD);
}

// --- 7. 未启用时诚实返回 UNKNOWN，不退回被动证据 ---

TEST(ActiveConnectivityTest, DisabledProbeIsUnknownNotFallback) {
    std::vector<ProbeTargetResult> targets = {makeTarget("a", true, true)};
    auto r = ActiveConnectivityEvaluator::evaluate(targets, /*probe_enabled=*/false);

    EXPECT_EQ(r.dns.state, HealthState::UNKNOWN);
    EXPECT_EQ(r.dns.coverage, Coverage::NONE);
    EXPECT_EQ(r.dns.reason, "active_probe_disabled");
    EXPECT_EQ(r.tcp.state, HealthState::UNKNOWN);
    EXPECT_EQ(r.tcp.reason, "active_probe_disabled");
}

TEST(ActiveConnectivityTest, NoTargetsIsUnknownNotGood) {
    auto r = ActiveConnectivityEvaluator::evaluate({}, true);
    EXPECT_EQ(r.dns.state, HealthState::UNKNOWN);
    EXPECT_EQ(r.dns.reason, "no_probe_targets");
    EXPECT_EQ(r.tcp.state, HealthState::UNKNOWN);
    EXPECT_EQ(r.tcp.reason, "no_probe_targets");
}

// --- 目标数不足时不下结论 ---

TEST(ActiveConnectivityTest, SingleTargetIsInsufficientForVerdict) {
    std::vector<ProbeTargetResult> targets = {makeTarget("a", true, true)};
    auto r = ActiveConnectivityEvaluator::evaluate(targets, true);
    EXPECT_EQ(r.dns.state, HealthState::UNKNOWN);
    EXPECT_EQ(r.dns.reason, "insufficient_probe_targets");
    EXPECT_EQ(r.tcp.state, HealthState::UNKNOWN);
}

// ============================================================================
// W1: Active HTTPS / Captive Portal 语义
//
// 覆盖三条硬约束：
//   1. HTTPS 成功 = TLS 握手成功 **且** 得到合法 HTTP 响应；
//      404/500 不否定 transport 存在（我们选的 oracle 不代表 Internet）。
//   2. 依赖截断：上游失败不得在下游各产生一次失败。
//   3. Portal 需要 ≥2 个独立故障域的一致信号；
//      单端点异常只 suspected，底层不健康时不产生门户结论。
// ============================================================================

namespace {

/// 构造一个走到 TLS/HTTP 阶段的目标
ProbeTargetResult makeTlsTarget(const std::string& id, bool tls_ok, bool http_ok,
                                int status = 200, const std::string& tls_detail = "connected",
                                const std::string& domain = "") {
    ProbeTargetResult t = makeTarget(id, true, true);
    t.failure_domain = domain.empty() ? (id + ".example") : domain;
    t.tls.attempted = true;
    t.tls.success = tls_ok;
    t.tls.detail = tls_detail;
    t.http.attempted = tls_ok;
    t.http.success = http_ok;
    t.http_info.status_code = status;
    return t;
}

ActiveConnectivityConfig tlsCfg() {
    ActiveConnectivityConfig c;
    c.tls_available = true;
    return c;
}

}  // namespace

// --- HTTPS: 全部成功 ---

TEST(ActiveHttpsTest, AllTargetsTlsOkIsGood) {
    std::vector<ProbeTargetResult> targets = {
        makeTlsTarget("a", true, true), makeTlsTarget("b", true, true)};
    auto r = ActiveConnectivityEvaluator::evaluate(targets, true, tlsCfg());
    EXPECT_EQ(r.https.state, HealthState::GOOD);
    EXPECT_EQ(r.https.coverage, Coverage::FULL_FOR_PROFILE);
    EXPECT_EQ(r.https.reason, "active_https_capability_ok");
    EXPECT_FALSE(r.https.capability_level_negative);
}

// 关键语义：受控目标返回 404/500 仍然证明 HTTPS transport 存在。
// 我们选的 endpoint 业务失败不等于本机 Internet 不可用。
TEST(ActiveHttpsTest, HttpErrorStatusStillProvesTransport) {
    // status 500 但 http.success=true（得到了可解析响应）
    std::vector<ProbeTargetResult> targets = {
        makeTlsTarget("a", true, true, 500), makeTlsTarget("b", true, true, 404)};
    auto r = ActiveConnectivityEvaluator::evaluate(targets, true, tlsCfg());
    EXPECT_EQ(r.https.state, HealthState::GOOD)
        << "404/500 是 endpoint 业务语义，不得据此判定 Internet HTTPS 不可用";
    EXPECT_FALSE(r.https.capability_level_negative);
}

// --- HTTPS: 全部失败 ---

TEST(ActiveHttpsTest, AllTargetsTlsFailIsCapabilityNegative) {
    std::vector<ProbeTargetResult> targets = {
        makeTlsTarget("a", false, false, 0, "tls_handshake_failed"),
        makeTlsTarget("b", false, false, 0, "tls_handshake_failed")};
    auto r = ActiveConnectivityEvaluator::evaluate(targets, true, tlsCfg());
    EXPECT_EQ(r.https.state, HealthState::BAD);
    EXPECT_TRUE(r.https.capability_level_negative);
    EXPECT_EQ(r.https.reason, "active_https_capability_failed");
}

// 证书错误单独归因：全部证书失败与"连不上"是不同的根因
TEST(ActiveHttpsTest, AllCertFailuresAreReportedSeparately) {
    std::vector<ProbeTargetResult> targets = {
        makeTlsTarget("a", false, false, 0, "cert_verify_failed"),
        makeTlsTarget("b", false, false, 0, "cert_verify_failed")};
    auto r = ActiveConnectivityEvaluator::evaluate(targets, true, tlsCfg());
    EXPECT_EQ(r.https.state, HealthState::BAD);
    EXPECT_EQ(r.https.reason, "active_https_cert_verification_failed")
        << "全部证书校验失败应精确归因，不混进 '网络不可达'";
}

// 单目标 TLS 成功即可证明能力存在（capability 是 host-level 事实）
TEST(ActiveHttpsTest, PartialSuccessStillProvesCapability) {
    std::vector<ProbeTargetResult> targets = {
        makeTlsTarget("a", true, true), makeTlsTarget("b", false, false, 0, "tls_timeout")};
    auto r = ActiveConnectivityEvaluator::evaluate(targets, true, tlsCfg());
    EXPECT_EQ(r.https.state, HealthState::GOOD)
        << "单个受控目标 TLS 成功即证明本机具备 HTTPS capability";
    EXPECT_FALSE(r.https.capability_level_negative);
}

// 依赖截断：TCP 全失败时 HTTPS 是 blocked_by_tcp，不是 4 个 SLE 同时负面
TEST(ActiveHttpsTest, TcpFailureBlocksHttpsWithoutFakeFailure) {
    std::vector<ProbeTargetResult> targets = {
        makeTarget("a", true, false), makeTarget("b", true, false)};
    auto r = ActiveConnectivityEvaluator::evaluate(targets, true, tlsCfg());
    EXPECT_EQ(r.tcp.state, HealthState::BAD);
    EXPECT_EQ(r.https.state, HealthState::UNKNOWN);
    EXPECT_EQ(r.https.reason, "blocked_by_tcp");
    EXPECT_FALSE(r.https.capability_level_negative)
        << "TCP 故障不得在 HTTPS 维度再产生一次失败";
}

TEST(ActiveHttpsTest, DnsFailureBlocksEverythingDownstream) {
    std::vector<ProbeTargetResult> targets = {
        makeTarget("a", false, false, false), makeTarget("b", false, false, false)};
    auto r = ActiveConnectivityEvaluator::evaluate(targets, true, tlsCfg());
    EXPECT_EQ(r.dns.state, HealthState::BAD);
    EXPECT_EQ(r.tcp.reason, "blocked_by_dns");
    EXPECT_EQ(r.https.reason, "blocked_by_tcp");
}

// 未声明 TLS 能力时仍是 NO_CAPABILITY（既有语义不回退）
TEST(ActiveHttpsTest, NoTlsCapabilityDeclaredStaysNoCapability) {
    std::vector<ProbeTargetResult> targets = {
        makeTlsTarget("a", true, true), makeTlsTarget("b", true, true)};
    auto r = ActiveConnectivityEvaluator::evaluate(targets, true);  // 默认 tls_available=false
    EXPECT_EQ(r.https.state, HealthState::UNKNOWN);
    EXPECT_EQ(r.https.scope, EvidenceScope::NO_CAPABILITY);
    EXPECT_EQ(r.https.reason, "no_tls_probe_capability");
}

// --- Captive Portal ---

namespace {
ProbeTargetResult makePortalTarget(const std::string& id, PortalSignal sig,
                                   const std::string& domain) {
    ProbeTargetResult t = makeTarget(id, true, true);
    t.failure_domain = domain;
    t.http.attempted = true;
    t.http.success = true;
    t.http_info.portal_signal = sig;
    t.http_info.status_code = (sig == PortalSignal::REDIRECTED) ? 302 : 200;
    t.portal_detected = (sig == PortalSignal::REDIRECTED || sig == PortalSignal::CONTENT_MISMATCH);
    return t;
}
}  // namespace

// 未配置 oracle → 无能力，不推测
TEST(CaptivePortalTest, NoOracleMeansNoCapability) {
    std::vector<ProbeTargetResult> targets = {
        makePortalTarget("a", PortalSignal::NOT_PROBED, "d1"),
        makePortalTarget("b", PortalSignal::NOT_PROBED, "d2")};
    auto r = ActiveConnectivityEvaluator::evaluate(targets, true, tlsCfg());
    EXPECT_EQ(r.portal.state, HealthState::UNKNOWN);
    EXPECT_EQ(r.portal.scope, EvidenceScope::NO_CAPABILITY);
    EXPECT_EQ(r.portal.reason, "no_portal_probe_capability");
    EXPECT_FALSE(r.portal_suspected);
}

// 全部符合预期 → GOOD
TEST(CaptivePortalTest, ExpectedResponsesMeanNoPortal) {
    std::vector<ProbeTargetResult> targets = {
        makePortalTarget("a", PortalSignal::NONE, "d1"),
        makePortalTarget("b", PortalSignal::NONE, "d2")};
    auto r = ActiveConnectivityEvaluator::evaluate(targets, true, tlsCfg());
    EXPECT_EQ(r.portal.state, HealthState::GOOD);
    EXPECT_EQ(r.portal.reason, "no_captive_portal");
    EXPECT_FALSE(r.portal_suspected);
}

// 单端点异常：只 suspected，不确诊
TEST(CaptivePortalTest, SingleEndpointAnomalyIsOnlySuspected) {
    std::vector<ProbeTargetResult> targets = {
        makePortalTarget("a", PortalSignal::REDIRECTED, "d1"),
        makePortalTarget("b", PortalSignal::NONE, "d2")};
    auto r = ActiveConnectivityEvaluator::evaluate(targets, true, tlsCfg());
    EXPECT_TRUE(r.portal_suspected)
        << "单个 oracle 端点异常是常见运维事件，不足以确诊门户";
    EXPECT_NE(r.portal.state, HealthState::BAD);
    EXPECT_EQ(r.portal.reason, "portal_suspected_single_endpoint");
    EXPECT_FALSE(r.portal.capability_level_negative);
}

// ≥2 个独立故障域一致信号 → 确诊
TEST(CaptivePortalTest, QuorumAcrossDomainsDetectsPortal) {
    std::vector<ProbeTargetResult> targets = {
        makePortalTarget("a", PortalSignal::REDIRECTED, "d1"),
        makePortalTarget("b", PortalSignal::CONTENT_MISMATCH, "d2")};
    auto r = ActiveConnectivityEvaluator::evaluate(targets, true, tlsCfg());
    EXPECT_EQ(r.portal.state, HealthState::BAD);
    EXPECT_EQ(r.portal.reason, "captive_portal_detected");
    EXPECT_TRUE(r.portal.capability_level_negative);
    EXPECT_FALSE(r.portal_suspected);
}

// 关键：同一故障域的两个目标不能凑 quorum
TEST(CaptivePortalTest, SameFailureDomainDoesNotReachQuorum) {
    std::vector<ProbeTargetResult> targets = {
        makePortalTarget("a", PortalSignal::REDIRECTED, "same-cdn"),
        makePortalTarget("b", PortalSignal::REDIRECTED, "same-cdn")};
    auto r = ActiveConnectivityEvaluator::evaluate(targets, true, tlsCfg());
    EXPECT_TRUE(r.portal_suspected);
    EXPECT_NE(r.portal.state, HealthState::BAD)
        << "同一 CDN 的两个域名实为单点 oracle，不能凑 quorum";
}

// 底层不健康：不产生门户结论，交回底层 SLE 解释
TEST(CaptivePortalTest, UnhealthyUnderlyingSuppressesPortalVerdict) {
    std::vector<ProbeTargetResult> targets = {
        makePortalTarget("a", PortalSignal::REDIRECTED, "d1"),
        makePortalTarget("b", PortalSignal::CONTENT_MISMATCH, "d2")};
    auto cfg = tlsCfg();
    cfg.underlying_healthy = false;
    auto r = ActiveConnectivityEvaluator::evaluate(targets, true, cfg);
    EXPECT_NE(r.portal.state, HealthState::BAD)
        << "底层链路故障时不得把解释权归给门户";
    EXPECT_EQ(r.portal.reason, "portal_signal_but_underlying_unhealthy");
}

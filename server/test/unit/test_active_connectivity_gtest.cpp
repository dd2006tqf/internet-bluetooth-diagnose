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

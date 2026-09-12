/**
 * @file test_captive_portal_evaluator_gtest.cpp
 * @brief Captive Portal SLE 真值表
 *
 * 核心不变式：
 *   1. 只有明确门户特征 + 底层链路健康，才判定 CAPTIVE_PORTAL
 *   2. 门户特征存在但底层不健康 → UNKNOWN（不把底层故障误归因门户）
 *   3. 特征不明确 → UNKNOWN，绝不猜测
 *   4. 证据不足 / 观测不可靠 → UNKNOWN
 */

#include <gtest/gtest.h>
#include "assurance/captive_portal_evaluator.hpp"

using namespace weaknet;

namespace {

CaptivePortalEvaluator::Input makeInput(size_t portal, size_t expected,
                                        size_t inconclusive = 0) {
    CaptivePortalEvaluator::Input in;
    in.has_controlled_probe = true;   // 默认为"已提供受控探测证据"
    for (size_t i = 0; i < portal; ++i) {
        in.probes.push_back(CaptivePortalProbe{302, true, false});
    }
    for (size_t i = 0; i < expected; ++i) {
        in.probes.push_back(CaptivePortalProbe{200, false, true});
    }
    for (size_t i = 0; i < inconclusive; ++i) {
        in.probes.push_back(CaptivePortalProbe{200, false, false});
    }
    in.ip_reachable = true;
    in.dns_resolvable = true;
    in.tcp_connectable = true;
    in.capture_events = in.probes.size();
    return in;
}

}  // namespace

// 核心语义：没有受控探测能力时，一律 NO_CAPABILITY
TEST(CaptivePortalEvaluatorTruthTable, WithoutControlledProbeIsNoCapability) {
    CaptivePortalEvaluator::Input in;
    auto res = CaptivePortalEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::UNKNOWN);
    EXPECT_EQ(res.coverage, Coverage::NONE);
    EXPECT_EQ(res.reason, "no_portal_probe_capability");
    EXPECT_EQ(res.scope, EvidenceScope::NO_CAPABILITY);
}

// 关键不变式：普通 3xx 重定向**绝不能**判为 Portal。
// 301/302/307/308 是网站的常见正常行为。
TEST(CaptivePortalEvaluatorTruthTable, OrdinaryRedirectsNeverYieldPortalVerdict) {
    CaptivePortalEvaluator::Input in;
    in.has_controlled_probe = false;      // 无受控探测能力
    for (int i = 0; i < 100; ++i) {       // 即便观测到 100 个 302
        in.probes.push_back(CaptivePortalProbe{302, true, false});
    }
    in.ip_reachable = in.dns_resolvable = in.tcp_connectable = true;

    auto res = CaptivePortalEvaluator::evaluate(in);
    // 既不能判"有门户"，也不能判"没有门户"
    EXPECT_EQ(res.state, HealthState::UNKNOWN);
    EXPECT_EQ(res.coverage, Coverage::NONE);
    EXPECT_EQ(res.reason, "no_portal_probe_capability");
    EXPECT_NE(res.state, HealthState::BAD);
    // 重定向只作为观测事实记录，不产生状态语义
    ASSERT_FALSE(res.evidence.empty());
    EXPECT_EQ(res.evidence[0].metric, "redirect_observed");
}

TEST(CaptivePortalEvaluatorTruthTable, NoControlledProbesIsUnknown) {
    CaptivePortalEvaluator::Input in;
    in.has_controlled_probe = true;
    auto res = CaptivePortalEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::UNKNOWN);
    EXPECT_EQ(res.reason, "no_captive_portal_probes");
}

TEST(CaptivePortalEvaluatorTruthTable, BelowSampleGateIsUnknown) {
    auto in = makeInput(1, 0);
    auto res = CaptivePortalEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::UNKNOWN);
    EXPECT_EQ(res.reason, "insufficient_portal_probes");
}

TEST(CaptivePortalEvaluatorTruthTable, ClearPortalSignalIsBad) {
    // 明确门户特征 + 底层健康 → 确认门户
    auto in = makeInput(4, 0, 1);
    auto res = CaptivePortalEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::BAD);
    EXPECT_EQ(res.reason, "captive_portal_detected");
}

TEST(CaptivePortalEvaluatorTruthTable, ExpectedContentIsGood) {
    auto in = makeInput(0, 4, 1);
    auto res = CaptivePortalEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::GOOD);
    EXPECT_EQ(res.reason, "no_captive_portal");
}

TEST(CaptivePortalEvaluatorTruthTable, PortalSignalWithoutHealthyUnderlayIsUnknown) {
    // 门户特征存在，但底层链路不健康：不得归因门户
    auto in = makeInput(4, 0, 1);
    in.tcp_connectable = false;
    auto res = CaptivePortalEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::UNKNOWN);
    EXPECT_EQ(res.reason, "portal_signal_without_healthy_underlay");
}

TEST(CaptivePortalEvaluatorTruthTable, InconclusiveEvidenceIsUnknown) {
    // 既无门户特征也无预期内容 → 不猜测
    auto in = makeInput(0, 0, 5);
    auto res = CaptivePortalEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::UNKNOWN);
    EXPECT_EQ(res.reason, "portal_evidence_inconclusive");
}

TEST(CaptivePortalEvaluatorTruthTable, ObserverLossIsUnknown) {
    auto in = makeInput(0, 4, 1);
    in.capture_events = 1000;
    in.capture_lost = 200;
    auto res = CaptivePortalEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::UNKNOWN);
    EXPECT_EQ(res.reason, "observer_unreliable_event_loss");
}

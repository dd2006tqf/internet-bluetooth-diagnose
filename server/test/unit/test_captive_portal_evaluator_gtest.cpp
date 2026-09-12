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

TEST(CaptivePortalEvaluatorTruthTable, NoProbesIsUnknown) {
    CaptivePortalEvaluator::Input in;
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

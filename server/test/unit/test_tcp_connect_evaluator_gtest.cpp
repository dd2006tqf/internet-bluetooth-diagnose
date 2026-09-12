/**
 * @file test_tcp_connect_evaluator_gtest.cpp
 * @brief TCP Connect SLE 真值表：固化判定分支与 Evidence Quality 不变式
 *
 * 重点与 DNS Service SLE 保持一致的三条原则：
 *   1. 证据不足 → UNKNOWN，绝不伪造 GOOD
 *   2. 直接坏证据（真实观察到 connect 失败）可在 PARTIAL coverage 下成立
 *   3. 观测器不可靠且无直接坏证据 → UNKNOWN，绝不嫁祸网络
 */

#include <gtest/gtest.h>
#include "assurance/tcp_connect_evaluator.hpp"

using namespace weaknet;

namespace {

TcpConnectEvaluator::Input makeInput(size_t success, size_t failure,
                                     double success_latency_ms = 20.0) {
    TcpConnectEvaluator::Input in;
    for (size_t i = 0; i < success; ++i) {
        in.samples.push_back(TcpConnectSample{true, success_latency_ms});
    }
    for (size_t i = 0; i < failure; ++i) {
        in.samples.push_back(TcpConnectSample{false, 0.0});
    }
    in.capture_events = in.samples.size();
    return in;
}

}  // namespace

TEST(TcpConnectEvaluatorTruthTable, NoSamplesIsUnknownNotGood) {
    TcpConnectEvaluator::Input in;
    auto res = TcpConnectEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::UNKNOWN);
    EXPECT_EQ(res.coverage, Coverage::NONE);
    EXPECT_EQ(res.reason, "no_tcp_connect_observations");
}

TEST(TcpConnectEvaluatorTruthTable, BelowSampleGateIsUnknown) {
    auto in = makeInput(3, 0);  // 少于 min_samples(5)
    auto res = TcpConnectEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::UNKNOWN);
    EXPECT_EQ(res.coverage, Coverage::PARTIAL);
    EXPECT_EQ(res.reason, "insufficient_tcp_connect_samples");
}

TEST(TcpConnectEvaluatorTruthTable, AllSuccessIsGood) {
    auto in = makeInput(10, 0);
    auto res = TcpConnectEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::GOOD);
    EXPECT_EQ(res.coverage, Coverage::FULL_FOR_PROFILE);
    EXPECT_EQ(res.reason, "tcp_connect_healthy");
}

TEST(TcpConnectEvaluatorTruthTable, ElevatedFailureIsDegraded) {
    auto in = makeInput(19, 1);  // 5%
    auto res = TcpConnectEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::DEGRADED);
    EXPECT_EQ(res.reason, "elevated_connect_failure_rate");
}

TEST(TcpConnectEvaluatorTruthTable, HighFailureIsBad) {
    auto in = makeInput(8, 2);  // 20%
    auto res = TcpConnectEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::BAD);
    EXPECT_EQ(res.reason, "high_connect_failure_rate");
}

TEST(TcpConnectEvaluatorTruthTable, HighLatencyIsBad) {
    auto in = makeInput(6, 0, 1500.0);
    auto res = TcpConnectEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::BAD);
    EXPECT_EQ(res.reason, "excessive_connect_latency");
}

TEST(TcpConnectEvaluatorTruthTable, ElevatedLatencyIsDegraded) {
    auto in = makeInput(6, 0, 400.0);
    auto res = TcpConnectEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::DEGRADED);
    EXPECT_EQ(res.reason, "elevated_connect_latency");
}

TEST(TcpConnectEvaluatorTruthTable, ObserverLossWithoutDirectEvidenceIsUnknown) {
    auto in = makeInput(10, 0);          // 表面健康
    in.capture_events = 1000;
    in.capture_lost = 200;               // 20% 观测丢失
    auto res = TcpConnectEvaluator::evaluate(in);
    // 观测不可靠且无直接坏证据 → 不知道，绝不 GOOD
    EXPECT_EQ(res.state, HealthState::UNKNOWN);
    EXPECT_EQ(res.coverage, Coverage::PARTIAL);
    EXPECT_EQ(res.reason, "observer_unreliable_event_loss");
}

TEST(TcpConnectEvaluatorTruthTable, ObserverLossWithDirectEvidenceStillBad) {
    auto in = makeInput(8, 2);           // 真实观察到 2 次失败
    in.capture_events = 1000;
    in.capture_lost = 200;
    auto res = TcpConnectEvaluator::evaluate(in);
    // 直接坏证据不因观测丢失而失效，但 coverage 降级
    EXPECT_EQ(res.state, HealthState::BAD);
    EXPECT_EQ(res.coverage, Coverage::PARTIAL);
}

TEST(TcpConnectEvaluatorTruthTable, PairingIncompleteDegradesObserver) {
    auto in = makeInput(10, 0);
    in.capture_events = 100;
    in.unmatched_terminal = 50;          // 大量终态无对应 attempt
    auto res = TcpConnectEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::UNKNOWN);
    EXPECT_EQ(res.reason, "observer_unreliable_pairing_incomplete");
}

TEST(TcpConnectEvaluatorTruthTable, HealthyObserverKeepsFullCoverage) {
    auto in = makeInput(10, 0);
    in.capture_events = 100;
    auto res = TcpConnectEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::GOOD);
    EXPECT_EQ(res.coverage, Coverage::FULL_FOR_PROFILE);
}

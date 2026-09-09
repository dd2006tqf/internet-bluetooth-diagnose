// test_rtt_monitor_gtest.cpp
// RTT Monitor unit tests (Google Test version)
// Module under test: rtt_monitor.cpp / Assurance Engine (SLE evaluators)
//
// Note: rtt_monitor.cpp only contains start_rtt_monitor_thread which requires
// ServerContext and real network. We test the underlying logic via
// Responsiveness/Reachability evaluators and NetInfo quality assessment.

#include <gtest/gtest.h>
#include <chrono>
#include "weak_netmgr.hpp"
#include "net_info.hpp"
#include "assurance/responsiveness_evaluator.hpp"
#include "assurance/ip_reachability_evaluator.hpp"

using namespace weaknet;
using namespace weaknet_dbus;

// ============================================================================
// Test Suite: RTT Quality Assessment via SLE Assurance Evaluators
// ============================================================================

class RttQualityTest : public ::testing::Test {
protected:
    static MetricSample rttSample(double ms) {
        return MetricSample::valid(ms);
    }
};

// Test 1: Excellent RTT (< 60ms, ratio 0) -> GOOD
TEST_F(RttQualityTest, ExcellentRtt) {
    std::vector<MetricSample> rtts = {rttSample(30), rttSample(28), rttSample(32), rttSample(29)};
    auto res = ResponsivenessEvaluator::evaluate(rtts, {});
    EXPECT_EQ(res.state, HealthState::GOOD);
}

// Test 2: Good RTT (60~100ms) -> DEGRADED
TEST_F(RttQualityTest, GoodRtt) {
    std::vector<MetricSample> rtts = {rttSample(80), rttSample(85), rttSample(80), rttSample(82)};
    auto res = ResponsivenessEvaluator::evaluate(rtts, {});
    EXPECT_NE(res.state, HealthState::GOOD);
}

// Test 3: Poor RTT (> 150ms consistently) -> BAD
TEST_F(RttQualityTest, PoorRtt) {
    std::vector<MetricSample> rtts = {rttSample(160), rttSample(180), rttSample(200), rttSample(170)};
    auto res = ResponsivenessEvaluator::evaluate(rtts, {});
    EXPECT_EQ(res.state, HealthState::BAD);
}

// Test 4: Insufficient samples -> UNKNOWN (min_valid_samples)
TEST_F(RttQualityTest, InsufficientSamples) {
    std::vector<MetricSample> rtts = {rttSample(30)};
    auto res = ResponsivenessEvaluator::evaluate(rtts, {});
    EXPECT_EQ(res.state, HealthState::UNKNOWN);
}

// ============================================================================
// Test Suite: Reachability semantics
// ============================================================================

TEST(ReachabilityTest, ConsecutiveFailuresLeadToBad) {
    std::vector<MetricSample> events = {
        MetricSample::valid(1.0),
        MetricSample::valid(1.0),
        MetricSample::valid(0.0),
        MetricSample::valid(0.0),
        MetricSample::valid(0.0)
    };
    auto res = IpReachabilityEvaluator::evaluate(events);
    EXPECT_EQ(res.state, HealthState::BAD);
}

// ============================================================================
// Test Suite: WeakNetMgr RTT Update Logic
// ============================================================================

class RttUpdateTest : public ::testing::Test {
protected:
    WeakNetMgr mgr;
};

// Test 9: NetInfo RTT properties
TEST_F(RttUpdateTest, RttProperties) {
    NetInfo info("wlan0");
    info.setRttMs(45);
    info.setPrevRttMs(40);

    EXPECT_EQ(info.rttMs(), 45);
    EXPECT_EQ(info.prevRttMs(), 40);
    // hasRtt checks if rtt is not the default value (-1)
    EXPECT_TRUE(info.hasRtt());
}

// Test 10: NetInfo hasEnoughMetricsForAssessment
TEST_F(RttUpdateTest, HasEnoughMetrics) {
    NetInfo info("wlan0");
    EXPECT_FALSE(info.hasEnoughMetricsForAssessment());

    info.setRttMs(45);
    info.setTcpLossRate(0.0);
    info.setRssiDbm(-65);
    EXPECT_TRUE(info.hasEnoughMetricsForAssessment());
}

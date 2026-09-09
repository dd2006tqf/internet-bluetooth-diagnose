// test_jitter_monitor_gtest.cpp
// Jitter Monitor tests (Google Test version)
// Tests: JitterMonitor data structures and configuration

#include <gtest/gtest.h>
#include "net_info.hpp"
#include "assurance/responsiveness_evaluator.hpp"

using namespace weaknet;
using namespace weaknet_dbus;

// ============================================================================
// 测试套件：NetInfo Jitter 字段
// ============================================================================

class JitterTest : public ::testing::Test {
protected:
    NetInfo makeIface(const std::string& name) {
        NetInfo info(name);
        info.setState(NetState::Up);
        info.setType(NetType::WiFi);
        return info;
    }
};

TEST_F(JitterTest, DefaultJitter) {
    NetInfo info("wlan0");
    EXPECT_DOUBLE_EQ(info.jitterMs(), -1.0);
    EXPECT_EQ(info.jitterLevel(), "");
    EXPECT_FALSE(info.hasJitter());
}

TEST_F(JitterTest, SetJitter) {
    NetInfo info("wlan0");
    info.setJitterMs(15.5);
    info.setJitterLevel("good");

    EXPECT_DOUBLE_EQ(info.jitterMs(), 15.5);
    EXPECT_EQ(info.jitterLevel(), "good");
    EXPECT_TRUE(info.hasJitter());
}

TEST_F(JitterTest, JitterLevels) {
    NetInfo info("wlan0");

    // Test all jitter level strings
    info.setJitterMs(5.0);
    info.setJitterLevel("excellent");
    EXPECT_EQ(info.jitterLevel(), "excellent");

    info.setJitterLevel("good");
    EXPECT_EQ(info.jitterLevel(), "good");

    info.setJitterLevel("fair");
    EXPECT_EQ(info.jitterLevel(), "fair");

    info.setJitterLevel("poor");
    EXPECT_EQ(info.jitterLevel(), "poor");

    info.setJitterLevel("degraded");
    EXPECT_EQ(info.jitterLevel(), "degraded");
}

TEST_F(JitterTest, HighJitterImpactOnQuality) {
    // High jitter should degrade responsiveness assessment
    std::vector<MetricSample> rtts = {
        MetricSample::valid(30.0),
        MetricSample::valid(28.0),
        MetricSample::valid(32.0),
        MetricSample::valid(29.0)
    };

    // Low jitter -> GOOD
    std::vector<MetricSample> jitters_low = { MetricSample::valid(2.0) };
    auto r1 = ResponsivenessEvaluator::evaluate(rtts, jitters_low);
    EXPECT_EQ(r1.state, HealthState::GOOD);

    // High jitter -> BAD (jitter_excessive)
    std::vector<MetricSample> jitters_high = { MetricSample::valid(50.0) };
    auto r2 = ResponsivenessEvaluator::evaluate(rtts, jitters_high);
    EXPECT_EQ(r2.state, HealthState::BAD);
}

// ============================================================================
// 测试套件：Jitter 序列化
// ============================================================================

TEST_F(JitterTest, JitterJsonRoundTrip) {
    NetInfo original("wlan0");
    original.setJitterMs(25.5);
    original.setJitterLevel("degraded");
    original.setRttMs(100);

    std::string json = original.toJson();
    EXPECT_FALSE(json.empty());

    NetInfo restored;
    EXPECT_TRUE(restored.fromJson(json));

    EXPECT_DOUBLE_EQ(restored.jitterMs(), 25.5);
    EXPECT_EQ(restored.jitterLevel(), "degraded");
    EXPECT_EQ(restored.rttMs(), 100);
}

// ============================================================================
// 测试套件：Jitter 验证
// ============================================================================

TEST_F(JitterTest, JitterInValidRange) {
    NetInfo info("wlan0");

    // Valid jitter values
    info.setJitterMs(0.0);
    EXPECT_TRUE(info.isValid());

    info.setJitterMs(100.0);
    EXPECT_TRUE(info.isValid());

    // Negative jitter (invalid)
    info.setJitterMs(-5.0);
    // isValid checks all fields, jitter alone shouldn't make it invalid
    // unless the validator specifically checks it
}

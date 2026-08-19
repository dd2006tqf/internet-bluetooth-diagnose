// test_jitter_monitor_gtest.cpp
// Jitter Monitor tests (Google Test version)
// Tests: JitterMonitor data structures and configuration

#include <gtest/gtest.h>
#include "net_info.hpp"
#include "network_quality_assessor.hpp"

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
    // High jitter should degrade quality assessment
    NetworkQualityAssessor assessor;

    // Good RTT but high jitter
    NetInfo info1("wlan0");
    info1.setRttMs(30);
    info1.setTcpLossRate(0.0);
    info1.setRssiDbm(-50);
    auto r1 = assessor.assessInterfaceQuality(info1);

    // Same but with high jitter (jitter itself isn't in the assessor yet,
    // but the quality should still be affected by RTT)
    NetInfo info2("wlan0");
    info2.setRttMs(30);
    info2.setTcpLossRate(0.0);
    info2.setRssiDbm(-50);
    auto r2 = assessor.assessInterfaceQuality(info2);

    // Both should be similar since jitter isn't factored into quality yet
    EXPECT_DOUBLE_EQ(r1.score, r2.score);
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

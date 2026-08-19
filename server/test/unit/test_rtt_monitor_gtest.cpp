// test_rtt_monitor_gtest.cpp
// RTT Monitor unit tests (Google Test version)
// Module under test: rtt_monitor.cpp (RTT monitoring and quality assessment)
//
// Note: rtt_monitor.cpp only contains start_rtt_monitor_thread which requires
// ServerContext and real network. We test the underlying logic via WeakNetMgr
// and NetInfo quality assessment.

#include <gtest/gtest.h>
#include "weak_netmgr.hpp"
#include "net_info.hpp"
#include "network_quality_assessor.hpp"

using namespace weaknet_dbus;

// ============================================================================
// Test Suite: RTT Quality Assessment via NetworkQualityAssessor
// ============================================================================

class RttQualityTest : public ::testing::Test {
protected:
    NetworkQualityAssessor assessor;

    NetInfo makeIface(const std::string& name, int rtt, double loss, int rssi) {
        NetInfo info(name);
        info.setRttMs(rtt);
        info.setTcpLossRate(loss);
        info.setRssiDbm(rssi);
        info.setState(NetState::Up);
        info.setType(NetType::WiFi);
        info.setDefaultRoute(true);
        return info;
    }
};

// Test 1: Excellent RTT (< 50ms) gives high score
TEST_F(RttQualityTest, ExcellentRtt) {
    auto iface = makeIface("wlan0", 30, 0.0, -45);
    auto result = assessor.assessInterfaceQuality(iface);
    EXPECT_EQ(result.level, NetworkQualityLevel::EXCELLENT);
    EXPECT_GE(result.score, 90.0);
}

// Test 2: Good RTT (50-100ms)
TEST_F(RttQualityTest, GoodRtt) {
    auto iface = makeIface("wlan0", 80, 0.0, -55);
    auto result = assessor.assessInterfaceQuality(iface);
    EXPECT_EQ(result.level, NetworkQualityLevel::GOOD);
    EXPECT_GE(result.score, 75.0);
}

// Test 3: Fair RTT (100-200ms)
TEST_F(RttQualityTest, FairRtt) {
    auto iface = makeIface("wlan0", 150, 0.0, -60);
    auto result = assessor.assessInterfaceQuality(iface);
    EXPECT_EQ(result.level, NetworkQualityLevel::FAIR);
    EXPECT_GE(result.score, 50.0);
}

// Test 4: Poor RTT (> 200ms) + bad loss + weak signal
TEST_F(RttQualityTest, PoorRtt) {
    auto iface = makeIface("wlan0", 300, 5.0, -85);
    auto result = assessor.assessInterfaceQuality(iface);
    EXPECT_EQ(result.level, NetworkQualityLevel::POOR);
}

// Test 5: High packet loss degrades quality
TEST_F(RttQualityTest, HighPacketLoss) {
    auto iface = makeIface("wlan0", 50, 5.0, -50);
    auto result = assessor.assessInterfaceQuality(iface);
    EXPECT_NE(result.level, NetworkQualityLevel::EXCELLENT);
}

// Test 6: Weak signal degrades quality
TEST_F(RttQualityTest, WeakSignal) {
    auto iface = makeIface("wlan0", 30, 0.0, -85);
    auto result = assessor.assessInterfaceQuality(iface);
    EXPECT_NE(result.level, NetworkQualityLevel::EXCELLENT);
}

// Test 7: Empty interface list returns UNKNOWN
TEST_F(RttQualityTest, EmptyInterfaces) {
    auto result = assessor.assessQuality({});
    EXPECT_EQ(result.level, NetworkQualityLevel::UNKNOWN);
}

// Test 8: Quality level names are correct
TEST_F(RttQualityTest, QualityLevelNames) {
    EXPECT_EQ(NetworkQualityAssessor::getQualityLevelName(NetworkQualityLevel::EXCELLENT), "EXCELLENT");
    EXPECT_EQ(NetworkQualityAssessor::getQualityLevelName(NetworkQualityLevel::GOOD), "GOOD");
    EXPECT_EQ(NetworkQualityAssessor::getQualityLevelName(NetworkQualityLevel::FAIR), "FAIR");
    EXPECT_EQ(NetworkQualityAssessor::getQualityLevelName(NetworkQualityLevel::POOR), "POOR");
    EXPECT_EQ(NetworkQualityAssessor::getQualityLevelName(NetworkQualityLevel::UNKNOWN), "UNKNOWN");
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

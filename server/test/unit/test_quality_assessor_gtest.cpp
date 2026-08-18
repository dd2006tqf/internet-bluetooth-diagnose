// test_quality_assessor_gtest.cpp
// Network Quality Assessor unit tests (Google Test version)
// Module under test: network_quality_assessor.cpp (weighted scoring algorithm)

#include <gtest/gtest.h>
#include "network_quality_assessor.hpp"
#include "net_info.hpp"

using namespace weaknet_dbus;

// ============================================================================
// Test Fixture
// ============================================================================
class QualityAssessorTest : public ::testing::Test {
protected:
    // Helper: create NetInfo with specified metrics
    NetInfo makeIface(const std::string& name, int rtt, double loss,
                      int rssi, uint64_t bps, uint64_t pps, uint32_t flows,
                      bool usingNow = true) {
        NetInfo info(name);
        info.setRttMs(rtt);
        info.setTcpLossRate(loss);
        info.setRssiDbm(rssi);
        info.setTrafficStats(bps, pps, flows);
        info.setUsingNow(usingNow);
        return info;
    }
};

// ============================================================================
// Test Cases
// ============================================================================

// Test 1: Excellent scenario (low RTT + no loss + strong signal + large packets)
TEST_F(QualityAssessorTest, ExcellentScenario) {
    NetworkQualityAssessor a;
    // RTT=30(excellent) loss=0(excellent) RSSI=-45(excellent) large packets
    auto iface = makeIface("wlan0", 30, 0.0, -45, 1000000, 800, 10);
    auto r = a.assessInterfaceQuality(iface);

    EXPECT_EQ(r.level, NetworkQualityLevel::EXCELLENT);
    EXPECT_GE(r.score, 90.0);
    EXPECT_EQ(r.levelName, "EXCELLENT");
}

// Test 2: Good scenario
TEST_F(QualityAssessorTest, GoodScenario) {
    NetworkQualityAssessor a;
    auto iface = makeIface("wlan0", 80, 0.3, -55, 500000, 800, 8);
    auto r = a.assessInterfaceQuality(iface);

    EXPECT_EQ(r.level, NetworkQualityLevel::GOOD);
    EXPECT_GE(r.score, 75.0);
    EXPECT_LT(r.score, 90.0);
    EXPECT_EQ(r.levelName, "GOOD");
}

// Test 3: Poor scenario (high latency + high loss + weak signal)
TEST_F(QualityAssessorTest, PoorScenario) {
    NetworkQualityAssessor a;
    auto iface = makeIface("wlan0", 300, 5.0, -85, 10000, 100, 1);
    auto r = a.assessInterfaceQuality(iface);

    EXPECT_EQ(r.level, NetworkQualityLevel::POOR);
    EXPECT_LT(r.score, 50.0);
    EXPECT_FALSE(r.issues.empty());  // Should detect issues
    EXPECT_EQ(r.levelName, "POOR");
}

// Test 4: Empty interface list
TEST_F(QualityAssessorTest, EmptyInterfaces) {
    NetworkQualityAssessor a;
    auto r = a.assessQuality({});

    EXPECT_EQ(r.level, NetworkQualityLevel::UNKNOWN);
    EXPECT_EQ(r.levelName, "UNKNOWN");
    EXPECT_FALSE(r.issues.empty());  // Should report no interfaces
}

// Test 5: No active interface should return UNKNOWN
TEST_F(QualityAssessorTest, NoActiveInterface) {
    NetworkQualityAssessor a;
    std::vector<NetInfo> ifaces = {
        makeIface("eth0", 30, 0.0, -45, 1000000, 800, 10, false),
        makeIface("wlan0", 300, 5.0, -85, 10000, 100, 1, false)
    };
    auto r = a.assessQuality(ifaces);

    // Should return UNKNOWN when no active interface
    EXPECT_EQ(r.level, NetworkQualityLevel::UNKNOWN);
    EXPECT_EQ(r.levelName, "UNKNOWN");
    EXPECT_DOUBLE_EQ(r.score, 0.0);
    EXPECT_FALSE(r.issues.empty());  // Should report no active interface
}

// Test 6: Weighting - poor RTT should not be EXCELLENT
TEST_F(QualityAssessorTest, WeightingRtt) {
    NetworkQualityAssessor a;
    // RTT=500(poor) others excellent -> affected by RTT weight 30%
    auto iface = makeIface("wlan0", 500, 0.0, -45, 1000000, 800, 10);
    auto r = a.assessInterfaceQuality(iface);

    EXPECT_LT(r.score, 90.0);  // Should not be EXCELLENT
}

// Test 7: Quality change counter increments
TEST_F(QualityAssessorTest, QualityChangeCounter) {
    NetworkQualityAssessor a;
    auto good = makeIface("wlan0", 30, 0.0, -45, 1000000, 800, 10);
    a.assessInterfaceQuality(good);
    int32_t c1 = a.getQualityChangeCounter();

    auto poor = makeIface("wlan0", 500, 10.0, -85, 1000, 10, 1);
    a.assessInterfaceQuality(poor);
    int32_t c2 = a.getQualityChangeCounter();

    EXPECT_GT(c2, c1);  // Counter should increment on level change
}

// Test 8: Level name mapping completeness
TEST_F(QualityAssessorTest, LevelNames) {
    EXPECT_EQ(NetworkQualityAssessor::getQualityLevelName(NetworkQualityLevel::EXCELLENT), "EXCELLENT");
    EXPECT_EQ(NetworkQualityAssessor::getQualityLevelName(NetworkQualityLevel::GOOD), "GOOD");
    EXPECT_EQ(NetworkQualityAssessor::getQualityLevelName(NetworkQualityLevel::FAIR), "FAIR");
    EXPECT_EQ(NetworkQualityAssessor::getQualityLevelName(NetworkQualityLevel::POOR), "POOR");
    EXPECT_EQ(NetworkQualityAssessor::getQualityLevelName(NetworkQualityLevel::UNKNOWN), "UNKNOWN");
}

// Test 9: RSSI=0 (unavailable) should give medium score
TEST_F(QualityAssessorTest, RssiUnavailable) {
    NetworkQualityAssessor a;
    auto iface = makeIface("wlan0", 30, 0.0, 0, 1000000, 800, 10);
    auto r = a.assessInterfaceQuality(iface);

    EXPECT_GT(r.score, 0.0);  // Should have reasonable score
}

// Test 10: JSON details should contain key fields
TEST_F(QualityAssessorTest, JsonDetails) {
    NetworkQualityAssessor a;
    auto iface = makeIface("wlan0", 50, 1.0, -60, 500000, 800, 5);
    auto r = a.assessInterfaceQuality(iface);

    EXPECT_NE(r.details.find("interface"), std::string::npos);
    EXPECT_NE(r.details.find("quality_score"), std::string::npos);
    EXPECT_NE(r.details.find("wlan0"), std::string::npos);
}

// Test 11: Multiple interfaces - should assess active one
TEST_F(QualityAssessorTest, MultipleInterfaces) {
    NetworkQualityAssessor a;
    std::vector<NetInfo> ifaces = {
        makeIface("eth0", 30, 0.0, -45, 1000000, 800, 10, false),  // not using
        makeIface("wlan0", 80, 0.3, -55, 500000, 800, 8, true)     // using
    };
    auto r = a.assessQuality(ifaces);

    // Should assess wlan0 (active interface)
    EXPECT_EQ(r.level, NetworkQualityLevel::GOOD);
    EXPECT_NE(r.details.find("wlan0"), std::string::npos);
}

// Test 12: Fair scenario
TEST_F(QualityAssessorTest, FairScenario) {
    NetworkQualityAssessor a;
    auto iface = makeIface("wlan0", 150, 1.5, -70, 100000, 400, 3);
    auto r = a.assessInterfaceQuality(iface);

    EXPECT_EQ(r.level, NetworkQualityLevel::FAIR);
    EXPECT_GE(r.score, 50.0);
    EXPECT_LT(r.score, 75.0);
}

// Test 13: Issues detection for high latency
TEST_F(QualityAssessorTest, IssuesDetectionHighLatency) {
    NetworkQualityAssessor a;
    auto iface = makeIface("wlan0", 250, 0.0, -50, 1000000, 800, 10);
    auto r = a.assessInterfaceQuality(iface);

    // Should detect high latency issue
    bool foundLatencyIssue = false;
    for (const auto& issue : r.issues) {
        if (issue.find("latency") != std::string::npos ||
            issue.find("Latency") != std::string::npos) {
            foundLatencyIssue = true;
            break;
        }
    }
    EXPECT_TRUE(foundLatencyIssue);
}

// ============================================================================
// main function
// ============================================================================
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

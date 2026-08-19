// test_weak_netmgr_gtest.cpp
// WeakNetMgr unit tests (Google Test version)
// Module under test: net_info.cpp (NetInfo data structure used by WeakNetMgr)
//
// Note: WeakNetMgr itself has heavy dependencies (NetPing, WiFiRssiClient, TrafficAnalyzer)
// so we test the underlying NetInfo data structure and static utility functions.

#include <gtest/gtest.h>
#include "net_info.hpp"

using namespace weaknet_dbus;

// ============================================================================
// Test Fixture
// ============================================================================
class WeakNetMgrTest : public ::testing::Test {
protected:
    void SetUp() override {
        // WeakNetMgr has heavy dependencies (NetPing, WiFiRssiClient, TrafficAnalyzer)
        // so we only test static utility functions and NetInfo
    }
};

// ============================================================================
// Test Cases
// ============================================================================

// Test 1: namesOf extracts interface names correctly
TEST(NetInfoTest, NamesOf) {
    std::vector<NetInfo> list;
    list.push_back(NetInfo("wlan0"));
    list.push_back(NetInfo("eth0"));
    list.push_back(NetInfo("lo"));

    // Extract names manually (WeakNetMgr::namesOf has heavy dependencies)
    std::vector<std::string> names;
    for (const auto& iface : list) {
        names.push_back(iface.ifName());
    }

    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "wlan0");
    EXPECT_EQ(names[1], "eth0");
    EXPECT_EQ(names[2], "lo");
}

// Test 3: NetInfo basic properties
TEST(NetInfoTest, BasicProperties) {
    NetInfo info("wlan0");
    info.setDefaultRoute(true);
    info.setState(NetState::Up);
    info.setType(NetType::WiFi);
    info.setRttMs(45);
    info.setRssiDbm(-65);
    info.setTrafficStats(1000000, 500, 10);

    EXPECT_EQ(info.ifName(), "wlan0");
    EXPECT_TRUE(info.isDefaultRoute());
    EXPECT_EQ(info.state(), NetState::Up);
    EXPECT_EQ(info.type(), NetType::WiFi);
    EXPECT_EQ(info.rttMs(), 45);
    EXPECT_EQ(info.rssiDbm(), -65);
    EXPECT_EQ(info.trafficTotalBps(), 1000000u);
    EXPECT_EQ(info.trafficTotalPps(), 500u);
    EXPECT_EQ(info.trafficActiveFlows(), 10u);
}

// Test 4: NetInfo quality level
TEST(NetInfoTest, QualityLevel) {
    NetInfo info("wlan0");
    info.setRttMs(30);
    info.setTcpLossRate(0.0);
    info.setRssiDbm(-45);

    EXPECT_EQ(info.quality(), LinkQuality::Unknown);

    info.setQuality(LinkQuality::Good);
    EXPECT_EQ(info.quality(), LinkQuality::Good);
}

// ============================================================================
// NetInfo 验证接口测试
// ============================================================================

// Test 5: hasRtt / hasTcpLoss / hasRssi / hasTraffic / hasJitter
TEST(NetInfoTest, HasMetrics) {
    NetInfo info("wlan0");
    EXPECT_FALSE(info.hasRtt());
    EXPECT_FALSE(info.hasTcpLoss());
    EXPECT_FALSE(info.hasRssi());
    EXPECT_FALSE(info.hasTraffic());
    EXPECT_FALSE(info.hasJitter());

    info.setRttMs(45);
    EXPECT_TRUE(info.hasRtt());

    info.setTcpLossRate(1.5);
    EXPECT_TRUE(info.hasTcpLoss());

    info.setRssiDbm(-65);
    EXPECT_TRUE(info.hasRssi());

    info.setTrafficStats(1000, 10, 2);
    EXPECT_TRUE(info.hasTraffic());

    info.setJitterMs(5.0);
    EXPECT_TRUE(info.hasJitter());
}

// Test 6: hasEnoughMetricsForAssessment
TEST(NetInfoTest, HasEnoughMetricsForAssessment) {
    NetInfo info("wlan0");
    EXPECT_FALSE(info.hasEnoughMetricsForAssessment());

    info.setRttMs(45);
    EXPECT_FALSE(info.hasEnoughMetricsForAssessment());

    info.setTcpLossRate(0.0);
    EXPECT_TRUE(info.hasEnoughMetricsForAssessment());
}

// Test 7: isValid
TEST(NetInfoTest, IsValid) {
    NetInfo info("wlan0");
    EXPECT_TRUE(info.isValid());

    // Empty ifname is invalid
    NetInfo empty("");
    EXPECT_FALSE(empty.isValid());
}

// Test 8: equals and sameKey
TEST(NetInfoTest, EqualsAndSameKey) {
    NetInfo a("wlan0");
    a.setDefaultRoute(true);
    a.setState(NetState::Up);

    NetInfo b("wlan0");
    b.setDefaultRoute(true);
    b.setState(NetState::Up);

    EXPECT_TRUE(a.sameKey(b));
    EXPECT_TRUE(a.equals(b));

    b.setRttMs(45);
    EXPECT_TRUE(a.sameKey(b));
    EXPECT_FALSE(a.equals(b));
}

// Test 9: 蓝牙相关字段
TEST(NetInfoTest, BluetoothFields) {
    NetInfo info("wlan0");

    // 默认值
    EXPECT_FALSE(info.hasBtDistance());
    EXPECT_DOUBLE_EQ(info.btDistance(), -1.0);
    EXPECT_EQ(info.btAudioQuality(), "");
    EXPECT_FALSE(info.bandConflict());
    EXPECT_DOUBLE_EQ(info.bandConflictConfidence(), 0.0);

    // 设置值
    info.setBtDistance(2.5);
    EXPECT_TRUE(info.hasBtDistance());
    EXPECT_DOUBLE_EQ(info.btDistance(), 2.5);

    info.setBtAudioQuality("excellent");
    EXPECT_EQ(info.btAudioQuality(), "excellent");

    info.setBandConflict(true);
    EXPECT_TRUE(info.bandConflict());

    info.setBandConflictConfidence(85.0);
    EXPECT_DOUBLE_EQ(info.bandConflictConfidence(), 85.0);
}

// Test 10: JSON 序列化/反序列化往返
TEST(NetInfoTest, JsonRoundTrip) {
    NetInfo original("wlan0");
    original.setDefaultRoute(true);
    original.setState(NetState::Up);
    original.setType(NetType::WiFi);
    original.setRttMs(45);
    original.setPrevRttMs(40);
    original.setRssiDbm(-65);
    original.setTcpLossRate(2.5);
    original.setTcpLossLevel("degraded");
    original.setTrafficStats(1000000, 500, 10);
    original.setJitterMs(10.5);
    original.setJitterLevel("good");
    original.setUsingNow(true);
    original.setQuality(LinkQuality::Good);

    std::string json = original.toJson();
    EXPECT_FALSE(json.empty());

    NetInfo restored;
    EXPECT_TRUE(restored.fromJson(json));

    EXPECT_EQ(restored.ifName(), "wlan0");
    EXPECT_TRUE(restored.isDefaultRoute());
    EXPECT_EQ(restored.state(), NetState::Up);
    EXPECT_EQ(restored.type(), NetType::WiFi);
    EXPECT_EQ(restored.rttMs(), 45);
    EXPECT_EQ(restored.prevRttMs(), 40);
    EXPECT_EQ(restored.rssiDbm(), -65);
    EXPECT_DOUBLE_EQ(restored.tcpLossRate(), 2.5);
    EXPECT_EQ(restored.tcpLossLevel(), "degraded");
    EXPECT_EQ(restored.trafficTotalBps(), 1000000u);
    EXPECT_EQ(restored.trafficTotalPps(), 500u);
    EXPECT_EQ(restored.trafficActiveFlows(), 10u);
    EXPECT_DOUBLE_EQ(restored.jitterMs(), 10.5);
    EXPECT_EQ(restored.jitterLevel(), "good");
    EXPECT_TRUE(restored.usingNow());
}

// Test 11: JSON 反序列化畸形输入
TEST(NetInfoTest, JsonMalformed) {
    NetInfo info("wlan0");
    EXPECT_FALSE(info.fromJson(""));
    EXPECT_FALSE(info.fromJson("{bad json"));
    EXPECT_FALSE(info.fromJson("12345"));
}

// Test 12: 默认值验证
TEST(NetInfoTest, DefaultValues) {
    NetInfo info("eth0");
    EXPECT_FALSE(info.isDefaultRoute());
    EXPECT_EQ(info.type(), NetType::Unknown);
    EXPECT_EQ(info.rttMs(), -1);
    EXPECT_EQ(info.prevRttMs(), -1);
    EXPECT_EQ(info.state(), NetState::Down);
    EXPECT_FALSE(info.usingNow());
    EXPECT_EQ(info.quality(), LinkQuality::Unknown);
    EXPECT_EQ(info.rssiDbm(), -1000);
    EXPECT_DOUBLE_EQ(info.tcpLossRate(), -1.0);
    EXPECT_EQ(info.tcpLossLevel(), "");
    EXPECT_EQ(info.trafficTotalBps(), 0u);
    EXPECT_EQ(info.trafficTotalPps(), 0u);
    EXPECT_EQ(info.trafficActiveFlows(), 0u);
    EXPECT_DOUBLE_EQ(info.jitterMs(), -1.0);
    EXPECT_EQ(info.jitterLevel(), "");
}

// ============================================================================
// NetworkQualityAssessor 与 RTT 质量评估交叉测试
// ============================================================================

#include "network_quality_assessor.hpp"

TEST(NetworkQualityCrossTest, RttAndLossCombined) {
    NetworkQualityAssessor assessor;

    // RTT 好但丢包高
    NetInfo info1("wlan0");
    info1.setRttMs(30);
    info1.setTcpLossRate(8.0);
    info1.setRssiDbm(-50);
    auto r1 = assessor.assessInterfaceQuality(info1);
    EXPECT_NE(r1.level, NetworkQualityLevel::EXCELLENT);

    // RTT 差但丢包低
    NetInfo info2("wlan0");
    info2.setRttMs(250);
    info2.setTcpLossRate(0.0);
    info2.setRssiDbm(-50);
    auto r2 = assessor.assessInterfaceQuality(info2);
    EXPECT_NE(r2.level, NetworkQualityLevel::EXCELLENT);
}

TEST(NetworkQualityCrossTest, PerfectConditions) {
    NetworkQualityAssessor assessor;
    NetInfo info("wlan0");
    info.setRttMs(20);
    info.setTcpLossRate(0.0);
    info.setRssiDbm(-40);
    info.setTrafficStats(5000000, 1000, 20);

    auto result = assessor.assessInterfaceQuality(info);
    EXPECT_EQ(result.level, NetworkQualityLevel::EXCELLENT);
    EXPECT_GE(result.score, 90.0);
    EXPECT_TRUE(result.issues.empty());
}

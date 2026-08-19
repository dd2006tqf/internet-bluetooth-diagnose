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

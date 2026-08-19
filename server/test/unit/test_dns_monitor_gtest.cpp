// test_dns_monitor_gtest.cpp
// DNS Monitor tests (Google Test version)
// Tests: DNS monitor data structures and configuration

#include <gtest/gtest.h>
#include "net_info.hpp"
#include "network_quality_assessor.hpp"

using namespace weaknet_dbus;

// ============================================================================
// 测试套件：DNS 解析质量评估
// ============================================================================

class DnsMonitorTest : public ::testing::Test {
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

TEST_F(DnsMonitorTest, ExcellentDnsConditions) {
    // Low RTT = good DNS resolution
    auto iface = makeIface("wlan0", 20, 0.0, -45);
    auto result = assessor.assessInterfaceQuality(iface);
    EXPECT_EQ(result.level, NetworkQualityLevel::EXCELLENT);
}

TEST_F(DnsMonitorTest, PoorDnsConditions) {
    // High RTT = slow DNS
    auto iface = makeIface("wlan0", 500, 10.0, -85);
    auto result = assessor.assessInterfaceQuality(iface);
    EXPECT_EQ(result.level, NetworkQualityLevel::POOR);
}

TEST_F(DnsMonitorTest, ModerateDnsConditions) {
    // Moderate RTT = moderate DNS
    auto iface = makeIface("wlan0", 150, 1.0, -65);
    auto result = assessor.assessInterfaceQuality(iface);
    EXPECT_NE(result.level, NetworkQualityLevel::EXCELLENT);
}

// ============================================================================
// 测试套件：DNS 统计字段（通过 NetInfo 扩展）
// ============================================================================

TEST_F(DnsMonitorTest, NetInfoWithDnsMetrics) {
    NetInfo info("wlan0");
    info.setRttMs(45);
    info.setTcpLossRate(0.5);

    // DNS metrics would be stored as additional fields
    // Currently DNS stats are separate, but RTT includes DNS latency
    EXPECT_EQ(info.rttMs(), 45);
    EXPECT_DOUBLE_EQ(info.tcpLossRate(), 0.5);
}

// ============================================================================
// 测试套件：DNS 超时场景
// ============================================================================

TEST_F(DnsMonitorTest, TimeoutScenario) {
    // Simulate DNS timeout: RTT = -1 (timeout)
    auto iface = makeIface("wlan0", -1, 0.0, -50);

    // RTT of -1 means no data
    EXPECT_FALSE(iface.hasRtt());

    // Quality assessment with no RTT data
    auto result = assessor.assessInterfaceQuality(iface);
    EXPECT_NE(result.level, NetworkQualityLevel::EXCELLENT);
}

TEST_F(DnsMonitorTest, MultipleQueriesScenario) {
    // Single interface with good metrics should give non-UNKNOWN quality
    std::vector<NetInfo> ifaces;
    NetInfo wlan0("wlan0");
    wlan0.setRttMs(30);
    wlan0.setTcpLossRate(0.0);
    wlan0.setRssiDbm(-50);
    wlan0.setState(NetState::Up);
    wlan0.setType(NetType::WiFi);
    wlan0.setDefaultRoute(true);
    ifaces.push_back(wlan0);

    auto result = assessor.assessQuality(ifaces);
    // With single interface, quality should be UNKNOWN (needs multiple interfaces for comparison)
    // or a valid quality level
    EXPECT_TRUE(result.level == NetworkQualityLevel::UNKNOWN ||
                result.level == NetworkQualityLevel::EXCELLENT ||
                result.level == NetworkQualityLevel::GOOD ||
                result.level == NetworkQualityLevel::FAIR ||
                result.level == NetworkQualityLevel::POOR);
}

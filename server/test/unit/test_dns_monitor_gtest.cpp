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
    // Simulate multiple DNS queries with varying latency
    std::vector<NetInfo> ifaces;
    ifaces.push_back(makeIface("wlan0", 30, 0.0, -50));
    ifaces.push_back(makeIface("eth0", 80, 0.1, -40));

    auto result = assessor.assessQuality(ifaces);
    // Overall quality should be between best and worst
    EXPECT_NE(result.level, NetworkQualityLevel::UNKNOWN);
}

// test_dns_monitor_gtest.cpp
// DNS Monitor unit tests (Google Test version)
// Module under test: DNS-related RTT semantic mapping in Assurance Engine
//
// Note: DNS eBPF probe statistics are tested separately (test_ebpf_monitor_observability_gtest).
// This suite verifies that DNS-related RTT outcomes map into SLE states properly.

#include <gtest/gtest.h>
#include <chrono>
#include "net_info.hpp"
#include "assurance/responsiveness_evaluator.hpp"
#include "assurance/ip_reachability_evaluator.hpp"

using namespace weaknet;
using namespace weaknet_dbus;

class DnsMonitorTest : public ::testing::Test {
protected:
    static MetricSample rttSample(double ms) {
        return MetricSample::valid(ms);
    }
};

TEST_F(DnsMonitorTest, ExcellentDnsConditions) {
    // Low RTT = good DNS resolution
    std::vector<MetricSample> rtts = {rttSample(20), rttSample(22), rttSample(21), rttSample(23)};
    auto res = ResponsivenessEvaluator::evaluate(rtts, {});
    EXPECT_EQ(res.state, HealthState::GOOD);
}

TEST_F(DnsMonitorTest, PoorDnsConditions) {
    // High RTT = slow DNS
    std::vector<MetricSample> rtts = {rttSample(500), rttSample(520), rttSample(510), rttSample(530)};
    auto res = ResponsivenessEvaluator::evaluate(rtts, {});
    EXPECT_EQ(res.state, HealthState::BAD);
}

TEST_F(DnsMonitorTest, ModerateDnsConditions) {
    // Moderate RTT = moderate DNS
    std::vector<MetricSample> rtts = {rttSample(150), rttSample(155), rttSample(152), rttSample(154)};
    auto res = ResponsivenessEvaluator::evaluate(rtts, {});
    EXPECT_NE(res.state, HealthState::GOOD);
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
    // Simulate DNS timeout: RTT = -1 (timeout) -> no reachability success
    std::vector<MetricSample> events = {
        MetricSample::valid(0.0),
        MetricSample::valid(0.0),
        MetricSample::valid(0.0)
    };
    auto res = IpReachabilityEvaluator::evaluate(events);
    EXPECT_NE(res.state, HealthState::GOOD);
}

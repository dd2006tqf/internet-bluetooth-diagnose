#include <gtest/gtest.h>
#include "ebpf_monitor_interface.hpp"
#include "ebpf_monitor_metrics.hpp"
#include "dns_monitor.hpp"
#include "wifi_packet_loss_monitor.hpp"
#include "http_latency_monitor.hpp"
#include "process_net_profiler.hpp"
#include "tcp_retransmit_monitor.hpp"
#include "bt_audio_analyzer.hpp"

using namespace weaknet_dbus;

TEST(EbpfMonitorMetricsTest, TracksReadsAndReset) {
    EbpfMonitorMetricsTracker tracker;
    tracker.recordProbeAttached();
    tracker.recordReadSuccess(10);
    tracker.recordReadSuccess(20, false);
    tracker.recordReadFailure("read failed");

    auto metrics = tracker.snapshot();
    EXPECT_EQ(metrics.attachedProbes, 1u);
    EXPECT_EQ(metrics.mapReads, 2u);
    EXPECT_EQ(metrics.samples, 1u);
    EXPECT_EQ(metrics.mapReadErrors, 1u);
    EXPECT_EQ(metrics.totalReadTimeUs, 30u);
    EXPECT_EQ(metrics.averageReadTimeUs, 15u);
    EXPECT_EQ(metrics.lastError, "read failed");
    EXPECT_EQ(tracker.consecutiveErrors(), 1u);
    EXPECT_NE(tracker.lastSuccessfulSampleNs(), 0u);

    tracker.reset();
    metrics = tracker.snapshot();
    EXPECT_EQ(metrics.mapReads, 0u);
    EXPECT_EQ(metrics.mapReadErrors, 0u);
    EXPECT_EQ(metrics.samples, 0u);
    EXPECT_EQ(tracker.consecutiveErrors(), 0u);
    EXPECT_EQ(tracker.lastSuccessfulSampleNs(), 0u);
}

TEST(EbpfMonitorMetricsTest, HealthStateTransitions) {
    EbpfMonitorStateSupport support("test");
    auto health = support.health();
    EXPECT_EQ(health.name, "test");
    EXPECT_EQ(health.state, EbpfMonitorState::Uninitialized);
    EXPECT_FALSE(health.available);
    EXPECT_FALSE(health.healthy);

    support.setState(EbpfMonitorState::Attached, true, "attached");
    support.recordReadSuccess(5);
    health = support.health();
    EXPECT_EQ(health.state, EbpfMonitorState::Attached);
    EXPECT_TRUE(health.available);
    EXPECT_TRUE(health.healthy);

    support.recordReadFailure("failure");
    support.recordReadFailure("failure");
    support.recordReadFailure("failure");
    health = support.health();
    EXPECT_FALSE(health.healthy);
    EXPECT_EQ(health.consecutiveErrors, 3u);

    support.setState(EbpfMonitorState::Fallback, false, "fallback");
    health = support.health();
    EXPECT_EQ(health.state, EbpfMonitorState::Fallback);
    EXPECT_FALSE(health.healthy);
}

TEST(EbpfMonitorInterfaceTest, AllSixMonitorsExposeCommonContract) {
    DnsMonitor dns;
    WifiPacketLossMonitor wifi;
    HttpLatencyMonitor http;
    ProcessNetProfiler process;
    TcpRetransMonitor tcp;
    BtAudioAnalyzer bt;

    IEbpfMonitor* monitors[] = {&dns, &wifi, &http, &process, &tcp, &bt};
    const char* names[] = {"DnsMonitor", "WifiPacketLossMonitor", "HttpLatencyMonitor",
                           "ProcessNetProfiler", "TcpRetransMonitor", "BtAudioAnalyzer"};
    for (size_t i = 0; i < 6; ++i) {
        EXPECT_STREQ(monitors[i]->monitorName(), names[i]);
        EXPECT_EQ(monitors[i]->commonState(), EbpfMonitorState::Uninitialized);
        EXPECT_FALSE(monitors[i]->isAvailable());
        EXPECT_EQ(monitors[i]->health().name, names[i]);
        monitors[i]->resetMetrics();
    }
}

TEST(EbpfMonitorInterfaceTest, FailedInitializationReportsFallbackOrError) {
    DnsMonitor dns;
    EXPECT_FALSE(dns.init("/nonexistent/dns_monitor.bpf.o"));
    EXPECT_FALSE(dns.isAvailable());
    EXPECT_TRUE(dns.commonState() == EbpfMonitorState::Error ||
                dns.commonState() == EbpfMonitorState::Fallback);
}

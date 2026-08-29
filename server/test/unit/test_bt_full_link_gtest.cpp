// test_bt_full_link_gtest.cpp
// Bluetooth Full Link Integration Test (Google Test version)
//
// Verifies spec Event Routing "band conflict event" scenario:
//   WHEN band conflict detector outputs detected=true
//   THEN pushed via EventManager.emitNetworkQualityChanged, signal payload contains "band_conflict" identifier
//
// This test links multiple real production components (BandConflictDetector + NetworkEventManager + DbusService
// stub), verifying feed->detect->emit->callback end-to-end chain. DbusService uses mock stub instead of real
// D-Bus connection, only as signal sink to record counter; emit routing logic is real execution from event_manager.cpp.

#include <gtest/gtest.h>
#include "band_conflict_detector.hpp"
#include "event_manager.hpp"
#include "server.hpp"
#include "dbus_service.hpp"
#include "database_manager.hpp"
#include "dns_monitor.hpp"
#include "wifi_packet_loss_monitor.hpp"
#include "http_latency_monitor.hpp"
#include "process_net_profiler.hpp"
#include "tcp_retransmit_monitor.hpp"
#include "bt_monitor.hpp"
#include "weak_netmgr.hpp"

#include <atomic>
#include <string>
#include <vector>
#include <cstdint>

// Test counter recorder (defined in mock_dbus_service.cpp) to verify D-Bus emit path
namespace weaknet_dbus::test_recorder {
void resetCounters();
std::vector<int32_t> snapshotCounters();
}

using namespace weaknet_dbus;

// ============================================================================
// Test Fixture
// ============================================================================
class BtFullLinkTest : public ::testing::Test {
protected:
    void SetUp() override {
        mgr_ = &getEventManager();
        ctx_ = std::make_unique<ServerContext>();
        svc_ = std::make_unique<DbusService>(ctx_.get());
        ctx_->service = std::make_unique<DbusService>(ctx_.get());
        mgr_->startEventMonitoring(ctx_.get());
        test_recorder::resetCounters();
    }

    void TearDown() override {
        mgr_->unregisterCallback(EventType::NetworkQualityChanged);
        ctx_->service.reset();
        svc_.reset();
        ctx_.reset();
    }

    NetworkEventManager* mgr_;
    std::unique_ptr<ServerContext> ctx_;
    std::unique_ptr<DbusService> svc_;
};

// ============================================================================
// Test Cases
// ============================================================================

// Test 1: Band conflict full link: feedSample -> detect -> emit -> callback with band_conflict payload
TEST_F(BtFullLinkTest, BandConflictFullLink) {
    // Register callback to record NetworkQualityChanged event payload
    std::atomic<bool> callback_triggered{false};
    std::string recorded_msg, recorded_src, recorded_details;
    mgr_->registerCallback(EventType::NetworkQualityChanged,
        [&](const NetworkEvent& ev) {
            callback_triggered.store(true);
            recorded_msg = ev.message;
            recorded_src = ev.source;
            recorded_details = ev.details;
        });

    // feedSample injects fully correlated samples (spec "band conflict confirmation" scenario)
    // 20 high baseline + 3 synchronized drops (20dBm drop, far exceeds 10dBm threshold, Pearson~1.0)
    BandConflictDetector detector;
    for (int i = 0; i < 20; i++) detector.feedSample(-50, -60);
    for (int i = 0; i < 3; i++) detector.feedSample(-70, -80);

    auto result = detector.detect();
    EXPECT_TRUE(result.detected);
    EXPECT_GT(result.confidence, 50.0);
    EXPECT_GT(result.correlation, 0.7);
    EXPECT_FALSE(result.suggestion.empty());

    // Simulate server.cpp network_quality_thread emit call
    mgr_->emitNetworkQualityChanged(
        "2.4GHz band conflict detected",
        result.suggestion,
        "band_conflict_detector"
    );

    // Verify callback triggered (EventManager routing works)
    EXPECT_TRUE(callback_triggered.load());

    // Verify source contains band_conflict identifier
    EXPECT_NE(recorded_src.find("band_conflict"), std::string::npos);

    // Verify details (suggestion) contains band_conflict identifier
    EXPECT_NE(recorded_details.find("band_conflict"), std::string::npos);

    // Verify D-Bus emit path works (mock stub recorded counter)
    EXPECT_FALSE(test_recorder::snapshotCounters().empty());
}

// Test 2: No conflict should not emit - verify full link doesn't emit when no conflict
TEST_F(BtFullLinkTest, NoConflictNoEmit) {
    std::atomic<int> trigger_count{0};
    mgr_->registerCallback(EventType::NetworkQualityChanged,
        [&trigger_count](const NetworkEvent&) { trigger_count.fetch_add(1); });

    // Stable samples (no drops) -> detect returns detected=false
    BandConflictDetector detector;
    for (int i = 0; i < 25; i++) detector.feedSample(-50, -60);
    auto result = detector.detect();
    EXPECT_FALSE(result.detected);

    // server.cpp only emits when detected && confidence>50, so should not emit here
    if (result.detected && result.confidence > 50.0) {
        mgr_->emitNetworkQualityChanged("band conflict", result.suggestion, "band_conflict_detector");
    }

    EXPECT_EQ(trigger_count.load(), 0);
    EXPECT_TRUE(test_recorder::snapshotCounters().empty());
}

// ============================================================================
// main function
// ============================================================================
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

// test_bt_monitor.cpp
// Bluetooth monitor special tests (pure logic + degraded scenarios)
// Module under test: bt_monitor.cpp (1637 lines, project's largest module)
//
// Test strategy: bt_monitor depends on BlueZ D-Bus + Bluetooth hardware + eBPF, cannot be fully
// detached from hardware. This test focuses on "hardware-free" pure logic and degraded scenarios:
//   - BtDeviceInfo inline methods (rssiLevel / averageRssi)
//   - estimateDistance path loss model
//   - calibrateDistance calibration
//   - setDefaultTxPower
//   - Degraded initialization without Bluetooth environment

#include <gtest/gtest.h>
#include "bt_monitor.hpp"
#include "bt_audio_fusion.hpp"

using namespace weaknet_dbus;

// ============================================================================
// BtDeviceInfo Pure Logic Tests (inline methods, no BtMonitor instance needed)
// ============================================================================

// Test 1: RSSI level classification
TEST(BtMonitorTest, RssiLevel) {
    BtDeviceInfo info;
    info.rssiHistory = {-45};
    EXPECT_EQ(info.rssiLevel(), "excellent");

    info.rssiHistory = {-55};
    EXPECT_EQ(info.rssiLevel(), "good");

    info.rssiHistory = {-65};
    EXPECT_EQ(info.rssiLevel(), "fair");

    info.rssiHistory = {-75};
    EXPECT_EQ(info.rssiLevel(), "poor");

    info.rssiHistory = {-85};
    EXPECT_EQ(info.rssiLevel(), "very_poor");
}

// Test 2: RSSI level - empty history returns unknown
TEST(BtMonitorTest, RssiLevelEmpty) {
    BtDeviceInfo info;
    info.rssiHistory.clear();
    EXPECT_EQ(info.rssiLevel(), "unknown");
}

// Test 3: RSSI history average
TEST(BtMonitorTest, AverageRssi) {
    BtDeviceInfo info;
    info.rssiHistory = {-60, -62, -58, -60, -64};
    EXPECT_EQ(info.averageRssi(), -60);  // (-60-62-58-60-64)/5 = -60.8 -> -60
}

// Test 4: RSSI history - empty returns 0
TEST(BtMonitorTest, AverageRssiEmpty) {
    BtDeviceInfo info;
    info.rssiHistory.clear();
    EXPECT_EQ(info.averageRssi(), 0);
}

// ============================================================================
// BtMonitor Distance Estimation Tests (pure math, no D-Bus needed)
// ============================================================================

// Test 5: Distance at reference point should be ~1m
TEST(BtMonitorTest, DistanceAtReference) {
    BtMonitor monitor;  // Don't call initialize, only use pure math methods
    // Default txPower=-59, n=2.5, ref=1.0
    // distance = 10^((txPower - rssi)/(10*n)) * ref
    // rssi=-59 -> distance=1.0m
    double d = monitor.estimateDistance(-59);
    EXPECT_NEAR(d, 1.0, 0.15);
}

// Test 6: Distance far away should increase
TEST(BtMonitorTest, DistanceFarAway) {
    BtMonitor monitor;
    double d0 = monitor.estimateDistance(-59);
    double d1 = monitor.estimateDistance(-80);
    EXPECT_GT(d1, d0);
    EXPECT_GT(d1, 5.0);  // -80dBm should be >5m
}

// Test 7: Invalid value returns -1
TEST(BtMonitorTest, DistanceInvalid) {
    BtMonitor monitor;
    // rssi=0 means not obtained, should be invalid
    EXPECT_DOUBLE_EQ(monitor.estimateDistance(0), -1.0);
}

// Test 8: setDefaultTxPower affects distance estimation
TEST(BtMonitorTest, SetDefaultTxPower) {
    BtMonitor monitor;
    monitor.setDefaultTxPower(-50);  // Change to -50
    // Now rssi=-50 -> 1m
    double d = monitor.estimateDistance(-50);
    EXPECT_NEAR(d, 1.0, 0.15);
    // rssi=-59 now distance >1m (because txPower is higher)
    double d2 = monitor.estimateDistance(-59);
    EXPECT_GT(d2, 1.0);
}

// Test 9: calibrateDistance should return false when no device (no crash)
TEST(BtMonitorTest, CalibrateDistanceNoDevice) {
    BtMonitor monitor;
    // Not initialized, no devices, calibration should fail but not crash
    bool ok = monitor.calibrateDistance("AA:BB:CC:DD:EE:FF", 1.0);
    EXPECT_FALSE(ok);
}

// ============================================================================
// Degraded Scenario Tests (no Bluetooth hardware environment)
// ============================================================================

// Test 10: Degraded initialization without Bluetooth
TEST(BtMonitorTest, DegradedInit) {
    BtMonitor monitor;
    // 初始化并检查是否有蓝牙适配器
    bool ok = monitor.initialize();
    if (ok && monitor.hasAdapter()) {
        GTEST_SKIP() << "本机有蓝牙硬件，跳过降级测试";
    }
}

// Test 11: Query before init should safely return empty
TEST(BtMonitorTest, QueryBeforeInit) {
    BtMonitor monitor;
    auto devices = monitor.getDevices();
    EXPECT_TRUE(devices.empty());
    EXPECT_EQ(monitor.deviceCount(), 0u);
    EXPECT_EQ(monitor.connectedCount(), 0u);
}

// Test 12: Get adapter state before init should not crash
TEST(BtMonitorTest, AdapterStateBeforeInit) {
    BtMonitor monitor;
    auto state = monitor.getAdapterState();
    // Should return default state, not crash
    EXPECT_FALSE(state.powered);
    EXPECT_FALSE(state.discovering);
}

// Test 13: RSSI query before init returns default value
TEST(BtMonitorTest, RssiQueryBeforeInit) {
    BtMonitor monitor;
    int16_t rssi = monitor.getDeviceRssi("AA:BB:CC:DD:EE:FF");
    // Should return invalid value (-1000 or 0), not crash
    EXPECT_LE(rssi, 0);
}

// Test 14: cleanup before init should not crash
TEST(BtMonitorTest, CleanupBeforeInit) {
    BtMonitor monitor;
    monitor.cleanup();  // Should be safe no-op
    EXPECT_FALSE(monitor.isInitialized());
}

// Test 15: BtEvent type enum completeness
TEST(BtMonitorTest, BtEventTypes) {
    BtEvent e;
    e.type = BtEvent::Type::AdapterAdded;
    EXPECT_EQ(e.type, BtEvent::Type::AdapterAdded);
    e.type = BtEvent::Type::AdapterRemoved;
    EXPECT_EQ(e.type, BtEvent::Type::AdapterRemoved);
    e.type = BtEvent::Type::AdapterPowered;
    EXPECT_EQ(e.type, BtEvent::Type::AdapterPowered);
    e.type = BtEvent::Type::DeviceFound;
    EXPECT_EQ(e.type, BtEvent::Type::DeviceFound);
    e.type = BtEvent::Type::DeviceLost;
    EXPECT_EQ(e.type, BtEvent::Type::DeviceLost);
    e.type = BtEvent::Type::DeviceConnected;
    EXPECT_EQ(e.type, BtEvent::Type::DeviceConnected);
    e.type = BtEvent::Type::DeviceDisconnected;
    EXPECT_EQ(e.type, BtEvent::Type::DeviceDisconnected);
    e.type = BtEvent::Type::DeviceRssiChanged;
    EXPECT_EQ(e.type, BtEvent::Type::DeviceRssiChanged);
    e.type = BtEvent::Type::DiscoveryStarted;
    EXPECT_EQ(e.type, BtEvent::Type::DiscoveryStarted);
    e.type = BtEvent::Type::DiscoveryStopped;
    EXPECT_EQ(e.type, BtEvent::Type::DiscoveryStopped);
}

// Test 16: BtDeviceType enum
TEST(BtMonitorTest, BtDeviceType) {
    BtDeviceInfo info;
    info.deviceType = BtDeviceType::Classic;
    EXPECT_EQ(info.deviceType, BtDeviceType::Classic);
    info.deviceType = BtDeviceType::BLE;
    EXPECT_EQ(info.deviceType, BtDeviceType::BLE);
    info.deviceType = BtDeviceType::Dual;
    EXPECT_EQ(info.deviceType, BtDeviceType::Dual);
}

// ============================================================================
// A2DP Audio Quality Score Tests (REQ-A2DP-QUALITY, T4)
// ============================================================================

// Test friend: access BtMonitor private pure function calculateAudioScore
class BtMonitorAudioScoreTest {
public:
    static double score(const BtMonitor& m, const BtAudioTransport& t) {
        return m.calculateAudioScore(t);
    }
};

// Construct an active + SBC codec Transport, only delay is variable
static BtAudioTransport makeActiveTransport(uint16_t delay) {
    BtAudioTransport t;
    t.transportPath = "/org/bluez/hci0/dev_00_11_22_33_44_55/fd1";
    t.deviceMac = "00:11:22:33:44:55";
    t.state = "active";
    t.delay = delay;
    t.volume = 80;
    t.codec = 0x00;  // SBC
    return t;
}

// Test 17: delay=0 should have no delay penalty (only SBC -5)
TEST(BtMonitorTest, AudioScoreDelay0) {
    BtMonitor monitor;
    auto t = makeActiveTransport(0);
    double s = BtMonitorAudioScoreTest::score(monitor, t);
    EXPECT_NEAR(s, 95.0, 0.01);  // 100 - 5(SBC)
}

// Test 18: delay=100 still within 500 threshold
TEST(BtMonitorTest, AudioScoreDelay100) {
    BtMonitor monitor;
    auto t = makeActiveTransport(100);
    double s = BtMonitorAudioScoreTest::score(monitor, t);
    EXPECT_NEAR(s, 95.0, 0.01);
}

// Test 19: delay=501 triggers light delay penalty (-10)
TEST(BtMonitorTest, AudioScoreDelay500Boundary) {
    BtMonitor monitor;
    auto t = makeActiveTransport(501);
    double s = BtMonitorAudioScoreTest::score(monitor, t);
    EXPECT_NEAR(s, 85.0, 0.01);  // 100 - 10 - 5
}

// Test 20: delay=2001 triggers severe delay penalty (-40)
TEST(BtMonitorTest, AudioScoreDelay2000Boundary) {
    BtMonitor monitor;
    auto t = makeActiveTransport(2001);
    double s = BtMonitorAudioScoreTest::score(monitor, t);
    EXPECT_NEAR(s, 55.0, 0.01);  // 100 - 40 - 5
}

// Test 21: delay=5000 + inactive state additional penalty
TEST(BtMonitorTest, AudioScoreDelay5000Inactive) {
    BtMonitor monitor;
    auto t = makeActiveTransport(5000);
    t.state = "idle";
    double s = BtMonitorAudioScoreTest::score(monitor, t);
    EXPECT_NEAR(s, 40.0, 0.01);  // 100 - 40 - 5 - 15
}

// ============================================================================
// Phase 2 Fusion Layer Tests (REQ-FUSION + REQ-EVENT-ROUTING, T9)
// ============================================================================

// Test 22: initPhase2 degrades gracefully without eBPF (returns false, no crash)
TEST(BtMonitorTest, InitPhase2Degraded) {
    BtMonitor monitor;
    // No eBPF environment / nonexistent object file, initPhase2 should return false
    bool ok = monitor.initPhase2("nonexistent_bpf.o");
    EXPECT_FALSE(ok);
    // cleanup should safely clean fusion layer and analyzer
    monitor.cleanup();
    EXPECT_FALSE(monitor.isInitialized());
}

// Test 23: initPhase2 multiple calls are idempotent
TEST(BtMonitorTest, InitPhase2Idempotent) {
    BtMonitor monitor;
    bool ok1 = monitor.initPhase2("nonexistent_bpf.o");
    bool ok2 = monitor.initPhase2("nonexistent_bpf.o");
    EXPECT_FALSE(ok1);
    EXPECT_FALSE(ok2);
    monitor.cleanup();
}

// ============================================================================
// main function
// ============================================================================
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

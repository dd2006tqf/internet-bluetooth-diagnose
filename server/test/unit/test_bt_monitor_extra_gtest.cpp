// test_bt_monitor_extra_gtest.cpp
// Bluetooth data structure tests (Google Test version)
// Tests: BtAdapterState, BtDeviceInfo, BtAudioQuality structs
// Note: BtMonitor itself has heavy dependencies (D-Bus, BlueZ), so we test data structures only

#include <gtest/gtest.h>
#include "bt_monitor.hpp"

using namespace weaknet_dbus;

// ============================================================================
// 测试套件：蓝牙音频质量等级
// ============================================================================

TEST(BtAudioQualityTest, DefaultQuality) {
    BtAudioQuality q;
    EXPECT_TRUE(q.deviceMac.empty());
    EXPECT_FALSE(q.isActive);
    EXPECT_DOUBLE_EQ(q.qualityScore, 0.0);
    EXPECT_TRUE(q.level.empty());
    EXPECT_EQ(q.currentDelay, 0);
    EXPECT_DOUBLE_EQ(q.activeRatio, 0.0);
    EXPECT_TRUE(q.issues.empty());
}

TEST(BtAudioQualityTest, SetProperties) {
    BtAudioQuality q;
    q.deviceMac = "AA:BB:CC:DD:EE:FF";
    q.isActive = true;
    q.qualityScore = 85.5;
    q.level = "excellent";
    q.currentDelay = 150;
    q.activeRatio = 0.95;
    q.issues.push_back("no issues");

    EXPECT_EQ(q.deviceMac, "AA:BB:CC:DD:EE:FF");
    EXPECT_TRUE(q.isActive);
    EXPECT_DOUBLE_EQ(q.qualityScore, 85.5);
    EXPECT_EQ(q.level, "excellent");
    EXPECT_EQ(q.currentDelay, 150);
    EXPECT_DOUBLE_EQ(q.activeRatio, 0.95);
    EXPECT_EQ(q.issues.size(), 1u);
}

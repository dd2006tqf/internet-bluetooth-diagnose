// test_audio_fusion_gtest.cpp
// Bluetooth Audio Fusion Evaluator unit tests (Google Test version)
// Module under test: bt_audio_fusion.cpp (D-Bus state + eBPF traffic fusion scoring)

#include <gtest/gtest.h>
#include "bt_audio_fusion.hpp"
#include "bt_monitor.hpp"      // BtAudioTransport
#include "bt_audio_analyzer.hpp" // BtTrafficStats

using namespace weaknet_dbus;

// ============================================================================
// Test Fixture
// ============================================================================
class AudioFusionTest : public ::testing::Test {
protected:
    // Helper: create BtAudioTransport
    BtAudioTransport makeTransport(const std::string& state, uint16_t delay,
                                   uint8_t codec, const std::string& mac = "AA:BB:CC") {
        BtAudioTransport t;
        t.state = state;
        t.delay = delay;
        t.codec = codec;
        t.deviceMac = mac;
        return t;
    }

    // Helper: create BtTrafficStats
    BtTrafficStats makeStats(uint64_t bytes, uint64_t packets,
                             uint64_t gapCount, uint64_t maxGapNs) {
        BtTrafficStats s;
        s.bytes = bytes;
        s.packets = packets;
        s.gapCount = gapCount;
        s.maxGapNs = maxGapNs;
        return s;
    }
};

// ============================================================================
// Test Cases
// ============================================================================

// Test 1: Inactive transport should return 0 score
TEST_F(AudioFusionTest, InactiveTransport) {
    BtAudioFusion f;
    auto t = makeTransport("idle", 100, 0x00);
    auto r = f.evaluate(t, nullptr, nullptr, false);

    EXPECT_DOUBLE_EQ(r.qualityScore, 0.0);
    EXPECT_EQ(r.level, "inactive");
    EXPECT_FALSE(r.effectiveActive);
    EXPECT_FALSE(r.isActive);
}

// Test 2: DB-only mode (eBPF unavailable)
TEST_F(AudioFusionTest, DbOnlyMode) {
    BtAudioFusion f;
    auto t = makeTransport("active", 100, 0x01);  // Low delay + non-SBC
    auto r = f.evaluateDbOnly(t);

    EXPECT_FALSE(r.ebpfAvailable);
    EXPECT_GT(r.qualityScore, 80.0);  // 100 - 0(delay) - 0(non-SBC)
    EXPECT_TRUE(r.level == "excellent" || r.level == "good");
}

// Test 3: DB-only mode - high delay + SBC should score low
TEST_F(AudioFusionTest, DbOnlyHighDelay) {
    BtAudioFusion f;
    auto t = makeTransport("active", 3000, 0x00);  // delay=3000(>2000) + SBC
    auto r = f.evaluateDbOnly(t);

    // 100 - 40(high delay) - 5(SBC) = 55
    EXPECT_LT(r.qualityScore, 60.0);
}

// Test 4: Fusion mode - valid traffic should boost score
TEST_F(AudioFusionTest, FusionWithTraffic) {
    BtAudioFusion f;
    auto t = makeTransport("active", 200, 0x01);
    auto prev = makeStats(0, 0, 0, 0);
    auto cur = makeStats(50000, 100, 0, 1000000);  // 50KB increment, no stall
    auto r = f.evaluate(t, &cur, &prev, true);

    EXPECT_TRUE(r.effectiveActive);     // 50000 > 5120 threshold
    EXPECT_FALSE(r.suspectedStall);     // gapCount=0
    EXPECT_GT(r.ebpfCorrection, 0.0);  // Active traffic boost
    EXPECT_TRUE(r.ebpfAvailable);
}

// Test 5: Fusion mode - stall should penalize score
TEST_F(AudioFusionTest, FusionWithStall) {
    BtAudioFusion f;
    auto t = makeTransport("active", 200, 0x00);
    auto prev = makeStats(0, 0, 0, 0);
    auto cur = makeStats(50000, 100, 10, 600000000);  // gapCount=10, maxGap=600ms
    auto r = f.evaluate(t, &cur, &prev, true);

    EXPECT_TRUE(r.suspectedStall);     // 10 > 3 threshold
    EXPECT_LT(r.ebpfCorrection, 0.0);  // Stall penalty
    EXPECT_EQ(r.maxGapMs, 600);
}

// Test 6: Increment calculation - use delta when prevStats exists
TEST_F(AudioFusionTest, IncrementCalc) {
    BtAudioFusion f;
    auto t = makeTransport("active", 100, 0x01);
    auto prev = makeStats(10000, 50, 1, 0);
    auto cur = makeStats(60000, 150, 5, 0);  // Increment: 50000 bytes
    auto r = f.evaluate(t, &cur, &prev, true);

    // bytesDelta=50000 > 5120 -> effective active
    EXPECT_TRUE(r.effectiveActive);
}

// Test 7: eBPF unavailable should degrade to DB-only mode
TEST_F(AudioFusionTest, EbpfUnavailableDegradation) {
    BtAudioFusion f;
    auto t = makeTransport("active", 100, 0x01);

    // ebpfAvailable=false should use DB-only path
    auto r = f.evaluate(t, nullptr, nullptr, false);

    EXPECT_FALSE(r.ebpfAvailable);
    EXPECT_GT(r.qualityScore, 0.0);  // Still has DB score
}

// Test 8: scoreToLevel mapping
TEST_F(AudioFusionTest, ScoreToLevel) {
    EXPECT_EQ(BtAudioFusion::scoreToLevel(95.0), "excellent");
    EXPECT_EQ(BtAudioFusion::scoreToLevel(80.0), "good");
    EXPECT_EQ(BtAudioFusion::scoreToLevel(60.0), "fair");
    EXPECT_EQ(BtAudioFusion::scoreToLevel(40.0), "poor");
    EXPECT_EQ(BtAudioFusion::scoreToLevel(10.0), "unknown");
}

// Test 9: Config set/get
TEST_F(AudioFusionTest, ConfigSetGet) {
    BtAudioFusion f;
    BtAudioFusionConfig cfg;
    cfg.minBytesPerSec = 10240;
    cfg.stallThresholdMs = 300;
    f.setConfig(cfg);

    auto got = f.config();
    EXPECT_EQ(got.minBytesPerSec, 10240u);
    EXPECT_EQ(got.stallThresholdMs, 300u);
}

// Test 10: toLegacyQuality backward compatibility conversion
TEST_F(AudioFusionTest, ToLegacyQuality) {
    BtAudioFusion f;
    auto t = makeTransport("active", 200, 0x00);
    auto prev = makeStats(0, 0, 0, 0);
    auto cur = makeStats(50000, 100, 10, 600000000);
    auto r = f.evaluate(t, &cur, &prev, true);

    auto legacy = BtAudioFusion::toLegacyQuality(r);

    EXPECT_EQ(legacy.deviceMac, r.deviceMac);
    EXPECT_EQ(legacy.isActive, r.isActive);
    EXPECT_DOUBLE_EQ(legacy.qualityScore, r.qualityScore);
    EXPECT_EQ(legacy.level, r.level);
}

// Test 11: Active but no traffic should be inactive
TEST_F(AudioFusionTest, ActiveNoTraffic) {
    BtAudioFusion f;
    auto t = makeTransport("active", 100, 0x01);
    auto prev = makeStats(0, 0, 0, 0);
    auto cur = makeStats(100, 1, 0, 0);  // Very small traffic
    auto r = f.evaluate(t, &cur, &prev, true);

    // 100 < 5120 threshold -> not effective active
    EXPECT_FALSE(r.effectiveActive);
}

// Test 12: Multiple stalls should increase penalty
TEST_F(AudioFusionTest, MultipleStalls) {
    BtAudioFusion f;
    auto t = makeTransport("active", 200, 0x00);
    auto prev = makeStats(0, 0, 0, 0);

    // Many gaps -> high stall count
    auto cur = makeStats(50000, 100, 20, 600000000);
    auto r = f.evaluate(t, &cur, &prev, true);

    EXPECT_TRUE(r.suspectedStall);
    EXPECT_LT(r.ebpfCorrection, -20.0);  // Should be significant penalty
}

// ============================================================================
// main function
// ============================================================================
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

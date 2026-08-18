// test_band_conflict_gtest.cpp
// Band Conflict Detector unit tests (Google Test version)
// Module under test: band_conflict_detector.cpp (Wi-Fi/Bluetooth RSSI correlation conflict detection)

#include <gtest/gtest.h>
#include "band_conflict_detector.hpp"

using namespace weaknet_dbus;

// ============================================================================
// Test Fixture
// ============================================================================
class BandConflictTest : public ::testing::Test {
protected:
    void SetUp() override {
        detector_ = std::make_unique<BandConflictDetector>();
    }

    void TearDown() override {
        detector_.reset();
    }

    std::unique_ptr<BandConflictDetector> detector_;
};

// ============================================================================
// Test Cases
// ============================================================================

// Test 1: Insufficient samples (<5) should not output conclusion
TEST_F(BandConflictTest, InsufficientSamples) {
    for (int i = 0; i < 3; i++) {
        detector_->feedSample(-50, -60);  // Only 3 < MIN_SAMPLES(5)
    }
    auto r = detector_->detect();
    EXPECT_FALSE(r.detected);
}

// Test 2: Stable samples should have no conflict
TEST_F(BandConflictTest, NoConflict) {
    for (int i = 0; i < 25; i++) {
        detector_->feedSample(-50, -60);
    }
    auto r = detector_->detect();
    EXPECT_FALSE(r.detected);
}

// Test 3: Synchronized drop should detect conflict
// Note: baseline() takes "last 20" samples mean, drop samples will lower baseline.
// So use 20 high baseline samples + 3 significant drop samples.
TEST_F(BandConflictTest, ConflictDetected) {
    // 20 high baseline samples
    for (int i = 0; i < 20; i++) {
        detector_->feedSample(-50, -60);
    }
    // 3 synchronized drop samples (20dBm drop, far exceeds 10dBm threshold)
    for (int i = 0; i < 3; i++) {
        detector_->feedSample(-70, -80);
    }

    auto r = detector_->detect();
    EXPECT_TRUE(r.detected);
    EXPECT_GT(r.correlation, 0.7);
    EXPECT_GE(r.wifiRssiDrop, 10);
    EXPECT_GE(r.btRssiDrop, 10);
    EXPECT_FALSE(r.suggestion.empty());
}

// Test 4: Invalid values should be filtered (0 or <=-1000)
TEST_F(BandConflictTest, InvalidValuesSkipped) {
    detector_->feedSample(0, -60);       // wifi=0 invalid, skip
    detector_->feedSample(-50, -1000);   // bt=-1000 invalid, skip
    detector_->feedSample(-50, -60);     // valid

    EXPECT_EQ(detector_->sampleCount(), 1u);
}

// Test 5: Reset clears history
TEST_F(BandConflictTest, Reset) {
    for (int i = 0; i < 10; i++) {
        detector_->feedSample(-50, -60);
    }
    EXPECT_EQ(detector_->sampleCount(), 10u);

    detector_->reset();
    EXPECT_EQ(detector_->sampleCount(), 0u);
}

// Test 6: Single-side drop (only Wi-Fi drops) should not be conflict
TEST_F(BandConflictTest, SingleSideDrop) {
    for (int i = 0; i < 20; i++) {
        detector_->feedSample(-50, -60);
    }
    // Only Wi-Fi drops, Bluetooth stable -> low correlation, not conflict
    for (int i = 0; i < 10; i++) {
        detector_->feedSample(-70, -60);
    }

    auto r = detector_->detect();
    // Single-side drop should have lower correlation
    EXPECT_LT(r.correlation, 0.7);
}

// Test 7: generateSuggestion static method
TEST_F(BandConflictTest, GenerateSuggestion) {
    BandConflictResult r;
    r.detected = false;
    EXPECT_TRUE(BandConflictDetector::generateSuggestion(r).empty());

    r.detected = true;
    r.wifiRssiDrop = 15;
    r.btRssiDrop = 12;
    r.confidence = 85.0;
    auto suggestion = BandConflictDetector::generateSuggestion(r);
    EXPECT_FALSE(suggestion.empty());
}

// Test 8: Sample limit 30 (excess auto-discards oldest)
TEST_F(BandConflictTest, MaxHistoryLimit) {
    for (int i = 0; i < 40; i++) {
        detector_->feedSample(-50, -60);
    }
    EXPECT_EQ(detector_->sampleCount(), 30u);  // MAX_HISTORY=30
}

// Test 9: High confidence when both drop significantly
TEST_F(BandConflictTest, HighConfidence) {
    // Create clear conflict pattern
    for (int i = 0; i < 20; i++) {
        detector_->feedSample(-45, -55);  // Strong baseline
    }
    for (int i = 0; i < 5; i++) {
        detector_->feedSample(-75, -85);  // Significant drop (30dBm)
    }

    auto r = detector_->detect();
    EXPECT_TRUE(r.detected);
    EXPECT_GT(r.confidence, 80.0);  // Should have high confidence
}

// ============================================================================
// main function
// ============================================================================
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

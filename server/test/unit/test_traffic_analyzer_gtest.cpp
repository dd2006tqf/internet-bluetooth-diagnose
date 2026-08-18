// test_traffic_analyzer_gtest.cpp
// TrafficAnalyzer degraded mode unit tests (Google Test version)
// Module under test: traffic_analyzer.hpp / traffic_analyzer.cpp (degraded mode flag)

#include <gtest/gtest.h>
#include "traffic_analyzer.hpp"

#include <chrono>
#include <thread>

using namespace weaknet_dbus;

// ============================================================================
// Test Fixture
// ============================================================================
class TrafficAnalyzerTest : public ::testing::Test {
protected:
    void SetUp() override {
        analyzer_ = std::make_unique<TrafficAnalyzer>();
    }

    void TearDown() override {
        if (analyzer_) {
            analyzer_->stop();
        }
        analyzer_.reset();
    }

    std::unique_ptr<TrafficAnalyzer> analyzer_;
};

// ============================================================================
// Test Cases
// ============================================================================

// Test 1: Degraded mode should be false by default
TEST_F(TrafficAnalyzerTest, DefaultDegradedMode) {
    EXPECT_FALSE(analyzer_->isDegradedMode());
}

// Test 2: initForInterface failure should enter degraded mode
TEST_F(TrafficAnalyzerTest, InitFailureEntersDegradedMode) {
    // Use non-existent interface name to trigger initForInterface failure
    analyzer_->start("nonexistent_iface_test", 1);
    // degraded_mode_ is set synchronously in start(), no need to wait for thread
    EXPECT_TRUE(analyzer_->isDegradedMode());
}

// Test 3: Start and stop should work without crash
TEST_F(TrafficAnalyzerTest, StartStop) {
    // This should not crash
    analyzer_->start("nonexistent_iface_test", 1);
    EXPECT_TRUE(analyzer_->isDegradedMode());
    analyzer_->stop();
}

// Test 4: Multiple start calls should be handled gracefully
TEST_F(TrafficAnalyzerTest, MultipleStartCalls) {
    analyzer_->start("nonexistent_iface_test", 1);
    EXPECT_TRUE(analyzer_->isDegradedMode());

    // Second start should be handled gracefully
    analyzer_->start("nonexistent_iface_test", 1);
    EXPECT_TRUE(analyzer_->isDegradedMode());
}

// ============================================================================
// main function
// ============================================================================
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

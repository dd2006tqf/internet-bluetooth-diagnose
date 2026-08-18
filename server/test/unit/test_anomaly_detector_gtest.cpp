// test_anomaly_detector_gtest.cpp
// Traffic Anomaly Detector unit tests (Google Test version)
// Module under test: traffic_anomaly_detector.cpp (data exfiltration/suspicious connection/temporal anomaly detection)

#include <gtest/gtest.h>
#include "traffic_anomaly_detector.h"

// Note: FlowRate / TrafficAnomalyDetector are in global namespace

// ============================================================================
// Test Fixture
// ============================================================================
class AnomalyDetectorTest : public ::testing::Test {
protected:
    // Helper: create FlowRate
    FlowRate makeFlow(const std::string& src, int sport,
                      const std::string& dst, int dport,
                      const std::string& proto, uint64_t bps,
                      uint64_t pps, uint32_t pid = 0) {
        FlowRate f;
        f.src = src;
        f.sport = sport;
        f.dst = dst;
        f.dport = dport;
        f.proto = proto;
        f.bps = bps;
        f.pps = pps;
        f.pid = pid;
        return f;
    }
};

// ============================================================================
// Test Cases
// ============================================================================

// Test 1: Normal low traffic should have no anomaly
TEST_F(AnomalyDetectorTest, NormalTraffic) {
    TrafficAnomalyDetector d;
    std::vector<FlowRate> flows = {
        makeFlow("10.0.0.1", 1234, "8.8.8.8", 53, "UDP", 1000, 10, 100)
    };

    auto anomalies = d.analyzeTrafficPatterns(flows);
    EXPECT_TRUE(anomalies.empty());  // Few samples + low traffic, no anomaly
}

// Test 2: Data exfiltration detection - single high traffic (>10MB/s)
TEST_F(AnomalyDetectorTest, DataExfiltrationDirect) {
    TrafficAnomalyDetector d;
    // detectDataExfiltration directly checks flow.bps > 10MB/s
    const uint64_t HIGH_BPS = 15 * 1024 * 1024;  // 15MB/s
    std::vector<FlowRate> flows = {
        makeFlow("10.0.0.1", 1000, "1.2.3.4", 443, "TCP", HIGH_BPS, 800, 200)
    };

    auto anomalies = d.detectDataExfiltration(flows);
    EXPECT_FALSE(anomalies.empty());
    EXPECT_EQ(anomalies[0].anomalyType, "data_exfiltration");
    EXPECT_GT(anomalies[0].confidence, 0.0);
}

// Test 3: Suspicious connection detection - single process >50 connections
TEST_F(AnomalyDetectorTest, SuspiciousConnections) {
    TrafficAnomalyDetector d;
    std::vector<FlowRate> flows;
    // Same PID 51 connections -> exceeds threshold 50
    for (int i = 0; i < 51; i++) {
        flows.push_back(makeFlow("10.0.0.1", 2000 + i, "9.9.9.9", 80, "TCP", 500, 5, 999));
    }

    auto anomalies = d.detectSuspiciousConnections(flows);
    EXPECT_FALSE(anomalies.empty());
    EXPECT_EQ(anomalies[0].anomalyType, "suspicious_connection");
    EXPECT_NE(anomalies[0].description.find("999"), std::string::npos);
}

// Test 4: Normal connection count should have no anomaly
TEST_F(AnomalyDetectorTest, NormalConnections) {
    TrafficAnomalyDetector d;
    std::vector<FlowRate> flows;
    // Same PID 30 connections < 50 threshold
    for (int i = 0; i < 30; i++) {
        flows.push_back(makeFlow("10.0.0.1", 3000 + i, "9.9.9.9", 80, "TCP", 500, 5, 888));
    }

    auto anomalies = d.detectSuspiciousConnections(flows);
    EXPECT_TRUE(anomalies.empty());
}

// Test 5: Clear history
TEST_F(AnomalyDetectorTest, ClearHistory) {
    TrafficAnomalyDetector d;
    d.analyzeTrafficPatterns({makeFlow("10.0.0.1", 1234, "8.8.8.8", 53, "UDP", 1000, 10)});
    EXPECT_FALSE(d.getTrafficPatterns().empty());

    d.clearHistory();
    EXPECT_TRUE(d.getTrafficPatterns().empty());
}

// Test 6: Set detection parameters without crash
TEST_F(AnomalyDetectorTest, SetDetectionParams) {
    TrafficAnomalyDetector d;
    d.setDetectionParams(3.0, 4.0, 2.5);

    // Should work normally after setting params
    auto anomalies = d.analyzeTrafficPatterns(
        {makeFlow("10.0.0.1", 1234, "8.8.8.8", 53, "UDP", 1000, 10)});
    EXPECT_TRUE(anomalies.empty());  // Low traffic still no anomaly
}

// Test 7: Empty flow list should not crash
TEST_F(AnomalyDetectorTest, EmptyFlows) {
    TrafficAnomalyDetector d;
    auto a1 = d.analyzeTrafficPatterns({});
    auto a2 = d.detectDataExfiltration({});
    auto a3 = d.detectSuspiciousConnections({});
    auto a4 = d.detectTemporalAnomalies({});

    EXPECT_TRUE(a1.empty());
    EXPECT_TRUE(a2.empty());
    EXPECT_TRUE(a3.empty());
    EXPECT_TRUE(a4.empty());
}

// Test 8: Low traffic should have no data exfiltration
TEST_F(AnomalyDetectorTest, NoExfiltration) {
    TrafficAnomalyDetector d;
    std::vector<FlowRate> flows = {
        makeFlow("10.0.0.1", 1234, "8.8.8.8", 53, "UDP", 1000, 10, 100)
    };

    auto anomalies = d.detectDataExfiltration(flows);
    EXPECT_TRUE(anomalies.empty());  // 1000 B/s far below 10MB/s
}

// ============================================================================
// main function
// ============================================================================
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

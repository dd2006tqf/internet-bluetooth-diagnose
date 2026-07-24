// test_anomaly_detector.cpp
// 流量异常检测器单元测试
// 被测模块: traffic_anomaly_detector.cpp（数据泄露/可疑连接/时序异常检测）
// 编译: g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Itest/unit -o test/bin/test_anomaly_detector
//        test/unit/test_anomaly_detector.cpp src/traffic_anomaly_detector.cpp
//        src/logger.cpp -lglog

#include "test_common.hpp"
#include "traffic_anomaly_detector.h"

// 注意: FlowRate / TrafficAnomalyDetector 在全局命名空间（无 namespace）

// 辅助：构造 FlowRate
static FlowRate makeFlow(const std::string& src, int sport,
                         const std::string& dst, int dport,
                         const std::string& proto, uint64_t bps,
                         uint64_t pps, uint32_t pid = 0) {
    FlowRate f;
    f.src = src; f.sport = sport; f.dst = dst; f.dport = dport;
    f.proto = proto; f.bps = bps; f.pps = pps; f.pid = pid;
    return f;
}

// 测试1: 正常低流量无异常
static void testNormalTraffic() {
    TEST_CASE("正常低流量无异常");
    TrafficAnomalyDetector d;
    std::vector<FlowRate> flows = {
        makeFlow("10.0.0.1", 1234, "8.8.8.8", 53, "UDP", 1000, 10, 100)
    };
    auto anomalies = d.analyzeTrafficPatterns(flows);
    CHECK(anomalies.empty());  // 样本少 + 低流量，无异常
}

// 测试2: 数据泄露检测 - 单次高流量（>10MB/s）
static void testDataExfiltrationDirect() {
    TEST_CASE("数据泄露检测-单次超高流量");
    TrafficAnomalyDetector d;
    // detectDataExfiltration 直接检查 flow.bps > 10MB/s
    const uint64_t HIGH_BPS = 15 * 1024 * 1024;  // 15MB/s
    std::vector<FlowRate> flows = {
        makeFlow("10.0.0.1", 1000, "1.2.3.4", 443, "TCP", HIGH_BPS, 800, 200)
    };
    auto anomalies = d.detectDataExfiltration(flows);
    CHECK(!anomalies.empty());
    CHECK_EQ(anomalies[0].anomalyType, "data_exfiltration");
    CHECK_GT(anomalies[0].confidence, 0.0);
}

// 测试3: 可疑连接检测 - 单进程超过50连接
static void testSuspiciousConnections() {
    TEST_CASE("可疑连接检测-单进程51连接");
    TrafficAnomalyDetector d;
    std::vector<FlowRate> flows;
    // 同一 PID 51 个连接 → 超过阈值 50
    for (int i = 0; i < 51; i++) {
        flows.push_back(makeFlow("10.0.0.1", 2000 + i, "9.9.9.9", 80, "TCP", 500, 5, 999));
    }
    auto anomalies = d.detectSuspiciousConnections(flows);
    CHECK(!anomalies.empty());
    CHECK_EQ(anomalies[0].anomalyType, "suspicious_connection");
    CHECK_CONTAINS(anomalies[0].description, "999");
}

// 测试4: 可疑连接检测 - 少于阈值无异常
static void testNormalConnections() {
    TEST_CASE("正常连接数无异常");
    TrafficAnomalyDetector d;
    std::vector<FlowRate> flows;
    // 同一 PID 30 个连接 < 50 阈值
    for (int i = 0; i < 30; i++) {
        flows.push_back(makeFlow("10.0.0.1", 3000 + i, "9.9.9.9", 80, "TCP", 500, 5, 888));
    }
    auto anomalies = d.detectSuspiciousConnections(flows);
    CHECK(anomalies.empty());
}

// 测试5: 历史记录清理
static void testClearHistory() {
    TEST_CASE("清理历史数据");
    TrafficAnomalyDetector d;
    d.analyzeTrafficPatterns({makeFlow("10.0.0.1", 1234, "8.8.8.8", 53, "UDP", 1000, 10)});
    CHECK(!d.getTrafficPatterns().empty());
    d.clearHistory();
    CHECK(d.getTrafficPatterns().empty());
}

// 测试6: 设置检测参数不崩溃
static void testSetDetectionParams() {
    TEST_CASE("设置检测参数");
    TrafficAnomalyDetector d;
    d.setDetectionParams(3.0, 4.0, 2.5);
    // 设置后正常工作
    auto anomalies = d.analyzeTrafficPatterns(
        {makeFlow("10.0.0.1", 1234, "8.8.8.8", 53, "UDP", 1000, 10)});
    CHECK(anomalies.empty());  // 低流量仍无异常
}

// 测试7: 空流列表处理
static void testEmptyFlows() {
    TEST_CASE("空流列表不崩溃");
    TrafficAnomalyDetector d;
    auto a1 = d.analyzeTrafficPatterns({});
    auto a2 = d.detectDataExfiltration({});
    auto a3 = d.detectSuspiciousConnections({});
    auto a4 = d.detectTemporalAnomalies({});
    CHECK(a1.empty());
    CHECK(a2.empty());
    CHECK(a3.empty());
    CHECK(a4.empty());
}

// 测试8: 数据泄露 - 低流量无异常
static void testNoExfiltration() {
    TEST_CASE("低流量无数据泄露");
    TrafficAnomalyDetector d;
    std::vector<FlowRate> flows = {
        makeFlow("10.0.0.1", 1234, "8.8.8.8", 53, "UDP", 1000, 10, 100)
    };
    auto anomalies = d.detectDataExfiltration(flows);
    CHECK(anomalies.empty());  // 1000 B/s 远低于 10MB/s
}

int main(int /*argc*/, char* argv[]) {
    initTestLogging(argv[0]);
    testNormalTraffic();
    testDataExfiltrationDirect();
    testSuspiciousConnections();
    testNormalConnections();
    testClearHistory();
    testSetDetectionParams();
    testEmptyFlows();
    testNoExfiltration();
    return printTestResult();
}

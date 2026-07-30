// test_quality_assessor.cpp
// 网络质量评估器单元测试
// 被测模块: network_quality_assessor.cpp（加权评分算法）
// 编译: g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Itest/unit -o test/bin/test_quality_assessor
//        test/unit/test_quality_assessor.cpp src/network_quality_assessor.cpp
//        src/net_info.cpp src/serializer.cpp src/logger.cpp -lglog

#include "test_common.hpp"
#include "network_quality_assessor.hpp"
#include "net_info.hpp"

using namespace weaknet_dbus;

// 辅助：构造指定指标的 NetInfo
static NetInfo makeIface(const std::string& name, int rtt, double loss,
                         int rssi, uint64_t bps, uint64_t pps, uint32_t flows,
                         bool usingNow = true) {
    NetInfo info(name);
    info.setRttMs(rtt);
    info.setTcpLossRate(loss);
    info.setRssiDbm(rssi);
    info.setTrafficStats(bps, pps, flows);
    info.setUsingNow(usingNow);
    return info;
}

// 测试1: 优秀场景（低RTT + 无丢包 + 强信号 + 大包流量）
static void testExcellent() {
    TEST_CASE("优秀场景应返回EXCELLENT");
    NetworkQualityAssessor a;
    // RTT=30(满分) 丢包=0(满分) RSSI=-45(满分) 流量大包(高分)
    auto iface = makeIface("wlan0", 30, 0.0, -45, 1000000, 800, 10);
    auto r = a.assessInterfaceQuality(iface);
    CHECK(r.level == NetworkQualityLevel::EXCELLENT);
    CHECK_GE(r.score, 90.0);
    CHECK_EQ(r.levelName, "EXCELLENT");
}

// 测试2: 良好场景
static void testGood() {
    TEST_CASE("良好场景应返回GOOD");
    NetworkQualityAssessor a;
    auto iface = makeIface("wlan0", 80, 0.3, -55, 500000, 800, 8);
    auto r = a.assessInterfaceQuality(iface);
    CHECK(r.level == NetworkQualityLevel::GOOD);
    CHECK_GE(r.score, 75.0);
    CHECK_LT(r.score, 90.0);
    CHECK_EQ(r.levelName, "GOOD");
}

// 测试3: 差场景（高延迟 + 高丢包 + 弱信号）
static void testPoor() {
    TEST_CASE("差场景应返回POOR并检测出问题");
    NetworkQualityAssessor a;
    auto iface = makeIface("wlan0", 300, 5.0, -85, 10000, 100, 1);
    auto r = a.assessInterfaceQuality(iface);
    CHECK(r.level == NetworkQualityLevel::POOR);
    CHECK_LT(r.score, 50.0);
    CHECK(!r.issues.empty());  // 差场景应检测出问题
    CHECK_EQ(r.levelName, "POOR");
}

// 测试4: 空接口列表
static void testEmptyInterfaces() {
    TEST_CASE("空接口列表应返回UNKNOWN");
    NetworkQualityAssessor a;
    auto r = a.assessQuality({});
    CHECK(r.level == NetworkQualityLevel::UNKNOWN);
    CHECK_EQ(r.levelName, "UNKNOWN");
    CHECK(!r.issues.empty());  // 应提示无接口
}

// 测试5: 无活跃接口时应返回 UNKNOWN
static void testNoActiveInterface() {
    TEST_CASE("无活跃接口时应返回 UNKNOWN");
    NetworkQualityAssessor a;
    std::vector<NetInfo> ifaces = {
        makeIface("eth0", 30, 0.0, -45, 1000000, 800, 10, false),
        makeIface("wlan0", 300, 5.0, -85, 10000, 100, 1, false)
    };
    auto r = a.assessQuality(ifaces);
    // 无活跃接口时应返回 UNKNOWN，而不是评估不活跃接口
    CHECK(r.level == NetworkQualityLevel::UNKNOWN);
    CHECK_EQ(r.levelName, "UNKNOWN");
    CHECK_EQ(r.score, 0.0);
    CHECK(!r.issues.empty());  // 应提示无活跃接口
}

// 测试6: 权重验证 - 仅RTT差时总分受影响
static void testWeighting() {
    TEST_CASE("权重验证-RTT差时不应EXCELLENT");
    NetworkQualityAssessor a;
    // RTT=500(差) 其余满分 → 受RTT权重30%影响
    auto iface = makeIface("wlan0", 500, 0.0, -45, 1000000, 800, 10);
    auto r = a.assessInterfaceQuality(iface);
    CHECK_LT(r.score, 90.0);  // 不会是EXCELLENT
}

// 测试7: 质量变化计数器
static void testQualityChanged() {
    TEST_CASE("质量等级变化时计数器递增");
    NetworkQualityAssessor a;
    auto good = makeIface("wlan0", 30, 0.0, -45, 1000000, 800, 10);
    a.assessInterfaceQuality(good);
    int32_t c1 = a.getQualityChangeCounter();
    auto poor = makeIface("wlan0", 500, 10.0, -85, 1000, 10, 1);
    a.assessInterfaceQuality(poor);
    int32_t c2 = a.getQualityChangeCounter();
    CHECK_GT(c2, c1);  // 等级变化后计数器递增
}

// 测试8: 等级名称映射
static void testLevelNames() {
    TEST_CASE("等级名称映射完整");
    CHECK_EQ(NetworkQualityAssessor::getQualityLevelName(NetworkQualityLevel::EXCELLENT), "EXCELLENT");
    CHECK_EQ(NetworkQualityAssessor::getQualityLevelName(NetworkQualityLevel::GOOD), "GOOD");
    CHECK_EQ(NetworkQualityAssessor::getQualityLevelName(NetworkQualityLevel::FAIR), "FAIR");
    CHECK_EQ(NetworkQualityAssessor::getQualityLevelName(NetworkQualityLevel::POOR), "POOR");
    CHECK_EQ(NetworkQualityAssessor::getQualityLevelName(NetworkQualityLevel::UNKNOWN), "UNKNOWN");
}

// 测试9: RSSI=0（无法测量）应给中等分
static void testRssiUnavailable() {
    TEST_CASE("RSSI=0时给中等分数不崩溃");
    NetworkQualityAssessor a;
    auto iface = makeIface("wlan0", 30, 0.0, 0, 1000000, 800, 10);
    auto r = a.assessInterfaceQuality(iface);
    CHECK(r.score > 0.0);  // 应有合理分数
}

// 测试10: JSON详情生成
static void testJsonDetails() {
    TEST_CASE("JSON详情应包含关键字段");
    NetworkQualityAssessor a;
    auto iface = makeIface("wlan0", 50, 1.0, -60, 500000, 800, 5);
    auto r = a.assessInterfaceQuality(iface);
    CHECK_CONTAINS(r.details, "interface");
    CHECK_CONTAINS(r.details, "quality_score");
    CHECK_CONTAINS(r.details, "wlan0");
}

int main(int /*argc*/, char* argv[]) {
    initTestLogging(argv[0]);
    testExcellent();
    testGood();
    testPoor();
    testEmptyInterfaces();
    testNoActiveInterface();
    testWeighting();
    testQualityChanged();
    testLevelNames();
    testRssiUnavailable();
    testJsonDetails();
    return printTestResult();
}

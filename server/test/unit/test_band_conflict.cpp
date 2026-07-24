// test_band_conflict.cpp
// 频段冲突检测器单元测试
// 被测模块: band_conflict_detector.cpp（Wi-Fi/蓝牙 RSSI 相关性冲突检测）
// 编译: g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Itest/unit -o test/bin/test_band_conflict
//        test/unit/test_band_conflict.cpp src/band_conflict_detector.cpp
//        src/logger.cpp -lglog

#include "test_common.hpp"
#include "band_conflict_detector.hpp"

using namespace weaknet_dbus;

// 测试1: 样本不足（<5）不输出结论
static void testInsufficientSamples() {
    TEST_CASE("样本不足不输出结论");
    BandConflictDetector d;
    for (int i = 0; i < 3; i++) d.feedSample(-50, -60);  // 仅3个 < MIN_SAMPLES(5)
    auto r = d.detect();
    CHECK(!r.detected);
}

// 测试2: 稳定样本无冲突
static void testNoConflict() {
    TEST_CASE("稳定样本无冲突");
    BandConflictDetector d;
    for (int i = 0; i < 25; i++) d.feedSample(-50, -60);
    auto r = d.detect();
    CHECK(!r.detected);
}

// 测试3: 同步下降应检测到冲突
// 注意: baseline() 取"最近 20 个"样本均值，下降样本过多会拉低基线。
// 故采用 20 个高值基线 + 3 个显著下降样本，使基线保持高值、当前值低。
static void testConflictDetected() {
    TEST_CASE("同步下降应检测到冲突");
    BandConflictDetector d;
    // 20个高值基线样本
    for (int i = 0; i < 20; i++) d.feedSample(-50, -60);
    // 3个同步下降样本（降幅 20dBm，远超 10dBm 阈值）
    for (int i = 0; i < 3; i++) d.feedSample(-70, -80);
    auto r = d.detect();
    CHECK(r.detected);
    CHECK_GT(r.correlation, 0.7);
    CHECK_GE(r.wifiRssiDrop, 10);
    CHECK_GE(r.btRssiDrop, 10);
    CHECK(!r.suggestion.empty());
}

// 测试4: 无效值应被过滤（0 或 <=-1000）
static void testInvalidValuesSkipped() {
    TEST_CASE("无效值应被过滤");
    BandConflictDetector d;
    d.feedSample(0, -60);       // wifi=0 无效，跳过
    d.feedSample(-50, -1000);   // bt=-1000 无效，跳过
    d.feedSample(-50, -60);     // 有效
    CHECK_EQ(d.sampleCount(), 1u);
}

// 测试5: 重置清空历史
static void testReset() {
    TEST_CASE("重置清空历史");
    BandConflictDetector d;
    for (int i = 0; i < 10; i++) d.feedSample(-50, -60);
    CHECK_EQ(d.sampleCount(), 10u);
    d.reset();
    CHECK_EQ(d.sampleCount(), 0u);
}

// 测试6: 单边下降（仅Wi-Fi降）不算冲突
static void testSingleSideDrop() {
    TEST_CASE("单边下降不算冲突");
    BandConflictDetector d;
    for (int i = 0; i < 20; i++) d.feedSample(-50, -60);
    // 仅 Wi-Fi 下降，蓝牙稳定 → 相关性低，不算冲突
    for (int i = 0; i < 10; i++) d.feedSample(-70, -60);
    auto r = d.detect();
    // 单边下降相关性应较低
    CHECK_LT(r.correlation, 0.7);
}

// 测试7: generateSuggestion 静态方法
static void testGenerateSuggestion() {
    TEST_CASE("generateSuggestion静态方法");
    BandConflictResult r;
    r.detected = false;
    CHECK(BandConflictDetector::generateSuggestion(r).empty());

    r.detected = true;
    r.wifiRssiDrop = 15;
    r.btRssiDrop = 12;
    r.confidence = 85.0;
    auto suggestion = BandConflictDetector::generateSuggestion(r);
    CHECK(!suggestion.empty());
}

// 测试8: 样本上限30（超出自动丢弃最旧）
static void testMaxHistoryLimit() {
    TEST_CASE("样本上限30自动丢弃最旧");
    BandConflictDetector d;
    for (int i = 0; i < 40; i++) d.feedSample(-50, -60);
    CHECK_EQ(d.sampleCount(), 30u);  // MAX_HISTORY=30
}

int main(int /*argc*/, char* argv[]) {
    initTestLogging(argv[0]);
    testInsufficientSamples();
    testNoConflict();
    testConflictDetected();
    testInvalidValuesSkipped();
    testReset();
    testSingleSideDrop();
    testGenerateSuggestion();
    testMaxHistoryLimit();
    return printTestResult();
}

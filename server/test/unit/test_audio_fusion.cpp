// test_audio_fusion.cpp
// 蓝牙音频融合评估器单元测试
// 被测模块: bt_audio_fusion.cpp（D-Bus状态 + eBPF流量融合评分）
// 编译: g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Itest/unit -o test/bin/test_audio_fusion
//        test/unit/test_audio_fusion.cpp src/bt_audio_fusion.cpp
//        src/logger.cpp -lglog

#include "test_common.hpp"
#include "bt_audio_fusion.hpp"
#include "bt_monitor.hpp"      // BtAudioTransport
#include "bt_audio_analyzer.hpp" // BtTrafficStats

using namespace weaknet_dbus;

// 辅助：构造 BtAudioTransport
static BtAudioTransport makeTransport(const std::string& state, uint16_t delay,
                                      uint8_t codec, const std::string& mac = "AA:BB:CC") {
    BtAudioTransport t;
    t.state = state;
    t.delay = delay;
    t.codec = codec;
    t.deviceMac = mac;
    return t;
}

// 辅助：构造 BtTrafficStats
static BtTrafficStats makeStats(uint64_t bytes, uint64_t packets,
                                uint64_t gapCount, uint64_t maxGapNs) {
    BtTrafficStats s;
    s.bytes = bytes;
    s.packets = packets;
    s.gapCount = gapCount;
    s.maxGapNs = maxGapNs;
    return s;
}

// 测试1: 非活跃传输应返回0分
static void testInactiveTransport() {
    TEST_CASE("非活跃传输应返回0分");
    BtAudioFusion f;
    auto t = makeTransport("idle", 100, 0x00);
    auto r = f.evaluate(t, nullptr, nullptr, false);
    CHECK_EQ(r.qualityScore, 0.0);
    CHECK_EQ(r.level, "inactive");
    CHECK(!r.effectiveActive);
    CHECK(!r.isActive);
}

// 测试2: 纯D-Bus模式（eBPF不可用）
static void testDbOnlyMode() {
    TEST_CASE("纯D-Bus模式-低延迟非SBC应高分");
    BtAudioFusion f;
    auto t = makeTransport("active", 100, 0x01);  // 低延迟 + 非SBC
    auto r = f.evaluateDbOnly(t);
    CHECK(!r.ebpfAvailable);
    CHECK_GT(r.qualityScore, 80.0);  // 100 - 0(delay) - 0(非SBC)
    CHECK(r.level == "excellent" || r.level == "good");
}

// 测试3: 纯D-Bus模式 - 高延迟+SBC应低分
static void testDbOnlyHighDelay() {
    TEST_CASE("纯D-Bus模式-高延迟+SBC应低分");
    BtAudioFusion f;
    auto t = makeTransport("active", 3000, 0x00);  // delay=3000(>2000) + SBC
    auto r = f.evaluateDbOnly(t);
    // 100 - 40(高延迟) - 5(SBC) = 55
    CHECK_LT(r.qualityScore, 60.0);
}

// 测试4: 融合模式 - 有有效流量应加分
static void testFusionWithTraffic() {
    TEST_CASE("融合模式-有效流量应加分");
    BtAudioFusion f;
    auto t = makeTransport("active", 200, 0x01);
    auto prev = makeStats(0, 0, 0, 0);
    auto cur = makeStats(50000, 100, 0, 1000000);  // 50KB增量，无卡顿
    auto r = f.evaluate(t, &cur, &prev, true);
    CHECK(r.effectiveActive);     // 50000 > 5120 阈值
    CHECK(!r.suspectedStall);     // gapCount=0
    CHECK_GT(r.ebpfCorrection, 0.0);  // 有效活跃加分
    CHECK(r.ebpfAvailable);
}

// 测试5: 融合模式 - 卡顿应扣分
static void testFusionWithStall() {
    TEST_CASE("融合模式-卡顿应扣分");
    BtAudioFusion f;
    auto t = makeTransport("active", 200, 0x00);
    auto prev = makeStats(0, 0, 0, 0);
    auto cur = makeStats(50000, 100, 10, 600000000);  // gapCount=10, maxGap=600ms
    auto r = f.evaluate(t, &cur, &prev, true);
    CHECK(r.suspectedStall);     // 10 > 3 阈值
    CHECK_LT(r.ebpfCorrection, 0.0);  // 卡顿扣分
    CHECK_EQ(r.maxGapMs, 600);
}

// 测试6: 增量计算 - prevStats存在时取差值
static void testIncrementCalc() {
    TEST_CASE("增量计算-prevStats存在取差值");
    BtAudioFusion f;
    auto t = makeTransport("active", 100, 0x01);
    auto prev = makeStats(10000, 50, 1, 0);
    auto cur = makeStats(60000, 150, 5, 0);  // 增量: 50000字节
    auto r = f.evaluate(t, &cur, &prev, true);
    // bytesDelta=50000 > 5120 → 有效活跃
    CHECK(r.effectiveActive);
}

// 测试7: eBPF不可用时降级到D-Bus模式
static void testEbpfUnavailableDegradation() {
    TEST_CASE("eBPF不可用时降级到D-Bus模式");
    BtAudioFusion f;
    auto t = makeTransport("active", 100, 0x01);
    // ebpfAvailable=false 应走纯D-Bus路径
    auto r = f.evaluate(t, nullptr, nullptr, false);
    CHECK(!r.ebpfAvailable);
    CHECK_GT(r.qualityScore, 0.0);  // 仍有D-Bus评分
}

// 测试8: scoreToLevel 等级映射
static void testScoreToLevel() {
    TEST_CASE("评分→等级映射");
    CHECK_EQ(BtAudioFusion::scoreToLevel(95.0), "excellent");
    CHECK_EQ(BtAudioFusion::scoreToLevel(80.0), "good");
    CHECK_EQ(BtAudioFusion::scoreToLevel(60.0), "fair");
    CHECK_EQ(BtAudioFusion::scoreToLevel(40.0), "poor");
    CHECK_EQ(BtAudioFusion::scoreToLevel(10.0), "unknown");
}

// 测试9: 配置设置与获取
static void testConfigSetGet() {
    TEST_CASE("配置设置与获取");
    BtAudioFusion f;
    BtAudioFusionConfig cfg;
    cfg.minBytesPerSec = 10240;
    cfg.stallThresholdMs = 300;
    f.setConfig(cfg);
    auto got = f.config();
    CHECK_EQ(got.minBytesPerSec, 10240u);
    CHECK_EQ(got.stallThresholdMs, 300u);
}

// 测试10: toLegacyQuality 向后兼容转换
static void testToLegacyQuality() {
    TEST_CASE("toLegacyQuality向后兼容转换");
    BtAudioFusion f;
    auto t = makeTransport("active", 200, 0x00);
    auto prev = makeStats(0, 0, 0, 0);
    auto cur = makeStats(50000, 100, 10, 600000000);
    auto r = f.evaluate(t, &cur, &prev, true);
    auto legacy = BtAudioFusion::toLegacyQuality(r);
    CHECK_EQ(legacy.deviceMac, r.deviceMac);
    CHECK_EQ(legacy.isActive, r.isActive);
    CHECK_EQ(legacy.qualityScore, r.qualityScore);
    CHECK_EQ(legacy.level, r.level);
}

int main(int /*argc*/, char* argv[]) {
    initTestLogging(argv[0]);
    testInactiveTransport();
    testDbOnlyMode();
    testDbOnlyHighDelay();
    testFusionWithTraffic();
    testFusionWithStall();
    testIncrementCalc();
    testEbpfUnavailableDegradation();
    testScoreToLevel();
    testConfigSetGet();
    testToLegacyQuality();
    return printTestResult();
}

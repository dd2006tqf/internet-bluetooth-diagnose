// test_bt_monitor.cpp
// 蓝牙监控器专项测试（纯逻辑层 + 降级场景）
// 被测模块: bt_monitor.cpp（1637行，项目最大模块）
//
// 测试策略：bt_monitor 依赖 BlueZ D-Bus + 蓝牙硬件 + eBPF，无法完全脱离硬件。
// 本测试聚焦"无需硬件"的纯逻辑层与降级场景：
//   - BtDeviceInfo 内联方法（rssiLevel / averageRssi）
//   - estimateDistance 路径损耗模型
//   - calibrateDistance 校准
//   - setDefaultTxPower
//   - 无蓝牙环境降级初始化
//
// 编译: g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Itest/unit -o test/bin/test_bt_monitor
//        test/special/test_bt_monitor.cpp src/bt_monitor.cpp
//        src/bt_audio_analyzer.cpp src/bt_audio_fusion.cpp
//        src/logger.cpp -lglog `pkg-config --cflags --libs dbus-1`

#include "test_common.hpp"
#include "bt_monitor.hpp"
#include "bt_audio_fusion.hpp"  // BtAudioFusion 完整定义（bt_monitor.hpp 仅前置声明）

using namespace weaknet_dbus;

// ============================================================================
// BtDeviceInfo 纯逻辑测试（内联方法，无需 BtMonitor 实例）
// ============================================================================

// 测试1: RSSI 等级分级
static void testRssiLevel() {
    TEST_CASE("RSSI等级分级");
    BtDeviceInfo info;
    info.rssiHistory = {-45}; CHECK_EQ(info.rssiLevel(), "excellent");
    info.rssiHistory = {-55}; CHECK_EQ(info.rssiLevel(), "good");
    info.rssiHistory = {-65}; CHECK_EQ(info.rssiLevel(), "fair");
    info.rssiHistory = {-75}; CHECK_EQ(info.rssiLevel(), "poor");
    info.rssiHistory = {-85}; CHECK_EQ(info.rssiLevel(), "very_poor");
}

// 测试2: RSSI 等级 - 空历史返回 unknown
static void testRssiLevelEmpty() {
    TEST_CASE("空RSSI历史返回unknown");
    BtDeviceInfo info;
    info.rssiHistory.clear();
    CHECK_EQ(info.rssiLevel(), "unknown");
}

// 测试3: RSSI 历史平均值
static void testAverageRssi() {
    TEST_CASE("RSSI历史平均值");
    BtDeviceInfo info;
    info.rssiHistory = {-60, -62, -58, -60, -64};
    CHECK_EQ(info.averageRssi(), -60);  // (-60-62-58-60-64)/5 = -60.8 → -60
}

// 测试4: RSSI 历史 - 空返回0
static void testAverageRssiEmpty() {
    TEST_CASE("空RSSI历史平均返回0");
    BtDeviceInfo info;
    info.rssiHistory.clear();
    CHECK_EQ(info.averageRssi(), 0);
}

// ============================================================================
// BtMonitor 距离估算测试（纯数学，无需 D-Bus）
// ============================================================================

// 测试5: 距离估算 - 参考点应≈1m
static void testDistanceAtReference() {
    TEST_CASE("距离估算-参考点RSSI应≈1m");
    BtMonitor monitor;  // 不调用 initialize，仅用纯数学方法
    // 默认 txPower=-59, n=2.5, ref=1.0
    // distance = 10^((txPower - rssi)/(10*n)) * ref
    // rssi=-59 → distance=1.0m
    double d = monitor.estimateDistance(-59);
    CHECK_NEAR(d, 1.0, 0.15);
}

// 测试6: 距离估算 - 远距离应增大
static void testDistanceFarAway() {
    TEST_CASE("距离估算-远距离应增大");
    BtMonitor monitor;
    double d0 = monitor.estimateDistance(-59);
    double d1 = monitor.estimateDistance(-80);
    CHECK_GT(d1, d0);
    CHECK_GT(d1, 5.0);  // -80dBm 应 >5m
}

// 测试7: 距离估算 - 无效值返回-1
static void testDistanceInvalid() {
    TEST_CASE("距离估算-无效值返回-1");
    BtMonitor monitor;
    // rssi=0 表示未获取，应无效
    CHECK_EQ(monitor.estimateDistance(0), -1.0);
}

// 测试8: setDefaultTxPower 影响距离估算
static void testSetDefaultTxPower() {
    TEST_CASE("setDefaultTxPower影响距离估算");
    BtMonitor monitor;
    monitor.setDefaultTxPower(-50);  // 改为 -50
    // 现在 rssi=-50 → 1m
    double d = monitor.estimateDistance(-50);
    CHECK_NEAR(d, 1.0, 0.15);
    // rssi=-59 现在距离 >1m（因为 txPower 更高）
    double d2 = monitor.estimateDistance(-59);
    CHECK_GT(d2, 1.0);
}

// 测试9: calibrateDistance 校准（无设备时应返回false，不崩溃）
static void testCalibrateDistanceNoDevice() {
    TEST_CASE("calibrateDistance-无设备返回false不崩溃");
    BtMonitor monitor;
    // 未初始化、无设备，校准应失败但不崩溃
    bool ok = monitor.calibrateDistance("AA:BB:CC:DD:EE:FF", 1.0);
    CHECK(!ok);
}

// ============================================================================
// 降级场景测试（无蓝牙硬件环境）
// ============================================================================

// 测试10: 无蓝牙环境降级初始化
static void testDegradedInit() {
    TEST_CASE("无蓝牙环境降级初始化");
    BtMonitor monitor;
    // 无蓝牙硬件环境，initialize 应返回 false 且不崩溃
    bool ok = monitor.initialize();
    CHECK(!ok);
    CHECK(!monitor.isInitialized());
    CHECK(!monitor.hasAdapter());
}

// 测试11: 未初始化时查询设备应安全返回空
static void testQueryBeforeInit() {
    TEST_CASE("未初始化时查询设备安全返回空");
    BtMonitor monitor;
    auto devices = monitor.getDevices();
    CHECK(devices.empty());
    CHECK_EQ(monitor.deviceCount(), 0u);
    CHECK_EQ(monitor.connectedCount(), 0u);
}

// 测试12: 未初始化时获取适配器状态不崩溃
static void testAdapterStateBeforeInit() {
    TEST_CASE("未初始化时获取适配器状态不崩溃");
    BtMonitor monitor;
    auto state = monitor.getAdapterState();
    // 应返回默认状态，不崩溃
    CHECK(!state.powered);
    CHECK(!state.discovering);
}

// 测试13: 未初始化时 RSSI 查询返回默认值
static void testRssiQueryBeforeInit() {
    TEST_CASE("未初始化时RSSI查询返回默认值");
    BtMonitor monitor;
    int16_t rssi = monitor.getDeviceRssi("AA:BB:CC:DD:EE:FF");
    // 应返回无效值（-1000 或 0），不崩溃
    CHECK(rssi <= 0);
}

// 测试14: cleanup 在未初始化时调用不崩溃
static void testCleanupBeforeInit() {
    TEST_CASE("cleanup在未初始化时调用不崩溃");
    BtMonitor monitor;
    monitor.cleanup();  // 应安全无操作
    CHECK(!monitor.isInitialized());
}

// 测试15: BtEvent 事件类型枚举完整
static void testBtEventTypes() {
    TEST_CASE("BtEvent事件类型枚举完整");
    // 验证所有事件类型可构造且互不相等
    BtEvent e;
    e.type = BtEvent::Type::AdapterAdded;
    CHECK_EQ(e.type, BtEvent::Type::AdapterAdded);
    e.type = BtEvent::Type::AdapterRemoved;
    CHECK_EQ(e.type, BtEvent::Type::AdapterRemoved);
    e.type = BtEvent::Type::AdapterPowered;
    CHECK_EQ(e.type, BtEvent::Type::AdapterPowered);
    e.type = BtEvent::Type::DeviceFound;
    CHECK_EQ(e.type, BtEvent::Type::DeviceFound);
    e.type = BtEvent::Type::DeviceLost;
    CHECK_EQ(e.type, BtEvent::Type::DeviceLost);
    e.type = BtEvent::Type::DeviceConnected;
    CHECK_EQ(e.type, BtEvent::Type::DeviceConnected);
    e.type = BtEvent::Type::DeviceDisconnected;
    CHECK_EQ(e.type, BtEvent::Type::DeviceDisconnected);
    e.type = BtEvent::Type::DeviceRssiChanged;
    CHECK_EQ(e.type, BtEvent::Type::DeviceRssiChanged);
    e.type = BtEvent::Type::DiscoveryStarted;
    CHECK_EQ(e.type, BtEvent::Type::DiscoveryStarted);
    e.type = BtEvent::Type::DiscoveryStopped;
    CHECK_EQ(e.type, BtEvent::Type::DiscoveryStopped);
}

// 测试16: BtDeviceType 枚举
static void testBtDeviceType() {
    TEST_CASE("BtDeviceType枚举");
    BtDeviceInfo info;
    info.deviceType = BtDeviceType::Classic;
    CHECK_EQ(info.deviceType, BtDeviceType::Classic);
    info.deviceType = BtDeviceType::BLE;
    CHECK_EQ(info.deviceType, BtDeviceType::BLE);
    info.deviceType = BtDeviceType::Dual;
    CHECK_EQ(info.deviceType, BtDeviceType::Dual);
}

// ============================================================================
// A2DP 音频质量评分测试（REQ-A2DP-QUALITY，T4）
// ============================================================================

// 测试友元：访问 BtMonitor 私有纯函数 calculateAudioScore
class BtMonitorAudioScoreTest {
public:
    static double score(const BtMonitor& m, const BtAudioTransport& t) {
        return m.calculateAudioScore(t);
    }
};

// 构造一个 active + SBC 编解码器的 Transport，仅 delay 可变
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

// 测试16: delay=0 应无延迟扣分（仅 SBC 扣 5）
static void testAudioScoreDelay0() {
    TEST_CASE("音频评分-delay=0 仅 SBC 扣分");
    BtMonitor monitor;
    auto t = makeActiveTransport(0);
    double s = BtMonitorAudioScoreTest::score(monitor, t);
    CHECK_NEAR(s, 95.0, 0.01);  // 100 - 5(SBC)
}

// 测试17: delay=100 仍在 500 阈值内
static void testAudioScoreDelay100() {
    TEST_CASE("音频评分-delay=100 不触发延迟扣分");
    BtMonitor monitor;
    auto t = makeActiveTransport(100);
    double s = BtMonitorAudioScoreTest::score(monitor, t);
    CHECK_NEAR(s, 95.0, 0.01);
}

// 测试18: delay=501 触发轻度延迟扣分（-10）
static void testAudioScoreDelay500Boundary() {
    TEST_CASE("音频评分-delay>500 触发 -10 扣分");
    BtMonitor monitor;
    auto t = makeActiveTransport(501);
    double s = BtMonitorAudioScoreTest::score(monitor, t);
    CHECK_NEAR(s, 85.0, 0.01);  // 100 - 10 - 5
}

// 测试19: delay=2001 触发严重延迟扣分（-40）
static void testAudioScoreDelay2000Boundary() {
    TEST_CASE("音频评分-delay>2000 触发 -40 严重扣分");
    BtMonitor monitor;
    auto t = makeActiveTransport(2001);
    double s = BtMonitorAudioScoreTest::score(monitor, t);
    CHECK_NEAR(s, 55.0, 0.01);  // 100 - 40 - 5
}

// 测试20: delay=5000 严重延迟，且非 active 状态额外扣 15
static void testAudioScoreDelay5000Inactive() {
    TEST_CASE("音频评分-delay=5000+inactive 综合扣分");
    BtMonitor monitor;
    auto t = makeActiveTransport(5000);
    t.state = "idle";
    double s = BtMonitorAudioScoreTest::score(monitor, t);
    CHECK_NEAR(s, 40.0, 0.01);  // 100 - 40 - 5 - 15
}

// ============================================================================
// Phase 2 融合层接入测试（REQ-FUSION + REQ-EVENT-ROUTING，T9）
// ============================================================================

// 测试21: initPhase2 在无 eBPF 环境下降级创建融合层（返回 false 但不崩溃）
static void testInitPhase2Degraded() {
    TEST_CASE("initPhase2-无eBPF环境降级返回false不崩溃");
    BtMonitor monitor;
    // 无 eBPF 环境/不存在对象文件，initPhase2 应返回 false（eBPF 未挂载）
    bool ok = monitor.initPhase2("nonexistent_bpf.o");
    CHECK(!ok);
    // cleanup 应安全清理融合层与分析器
    monitor.cleanup();
    CHECK(!monitor.isInitialized());
}

// 测试22: initPhase2 多次调用幂等不崩溃
static void testInitPhase2Idempotent() {
    TEST_CASE("initPhase2-重复调用幂等安全");
    BtMonitor monitor;
    bool ok1 = monitor.initPhase2("nonexistent_bpf.o");
    bool ok2 = monitor.initPhase2("nonexistent_bpf.o");
    CHECK(!ok1);
    CHECK(!ok2);
    monitor.cleanup();
}

int main(int /*argc*/, char* argv[]) {
    initTestLogging(argv[0]);
    testRssiLevel();
    testRssiLevelEmpty();
    testAverageRssi();
    testAverageRssiEmpty();
    testDistanceAtReference();
    testDistanceFarAway();
    testDistanceInvalid();
    testSetDefaultTxPower();
    testCalibrateDistanceNoDevice();
    testDegradedInit();
    testQueryBeforeInit();
    testAdapterStateBeforeInit();
    testRssiQueryBeforeInit();
    testCleanupBeforeInit();
    testBtEventTypes();
    testBtDeviceType();
    testAudioScoreDelay0();
    testAudioScoreDelay100();
    testAudioScoreDelay500Boundary();
    testAudioScoreDelay2000Boundary();
    testAudioScoreDelay5000Inactive();
    testInitPhase2Degraded();
    testInitPhase2Idempotent();
    return printTestResult();
}

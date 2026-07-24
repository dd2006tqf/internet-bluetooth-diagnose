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
    return printTestResult();
}

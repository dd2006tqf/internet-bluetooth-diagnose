// test_event_manager.cpp
// 事件管理器单元测试
// 被测模块: event_manager.cpp（事件注册/分发/注销）
// 编译: g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Itest/unit -o test/bin/test_event_manager
//        test/unit/test_event_manager.cpp src/event_manager.cpp
//        src/logger.cpp -lglog `pkg-config --cflags --libs dbus-1`

#include "test_common.hpp"
#include "event_manager.hpp"

using namespace weaknet_dbus;

// 测试1: 注册回调后emit应触发
static void testRegisterAndEmit() {
    TEST_CASE("注册回调后emit应触发");
    auto& mgr = getEventManager();
    int callCount = 0;
    mgr.registerCallback(EventType::InterfaceChanged,
        [&callCount](const NetworkEvent&) { callCount++; });
    mgr.emitInterfaceChanged("test message", "test_source");
    CHECK_EQ(callCount, 1);
    mgr.unregisterCallback(EventType::InterfaceChanged);
}

// 测试2: 注销后emit不应触发
static void testUnregister() {
    TEST_CASE("注销后emit不应触发");
    auto& mgr = getEventManager();
    int callCount = 0;
    mgr.registerCallback(EventType::RttChanged,
        [&callCount](const NetworkEvent&) { callCount++; });
    mgr.unregisterCallback(EventType::RttChanged);
    mgr.emitRttChanged("msg", "src");
    CHECK_EQ(callCount, 0);
}

// 测试3: 同类型多回调都应触发
static void testMultipleCallbacks() {
    TEST_CASE("同类型多回调都应触发");
    auto& mgr = getEventManager();
    int c1 = 0, c2 = 0;
    mgr.registerCallback(EventType::RssiChanged, [&c1](const NetworkEvent&) { c1++; });
    mgr.registerCallback(EventType::RssiChanged, [&c2](const NetworkEvent&) { c2++; });
    mgr.emitRssiChanged("msg", "src");
    CHECK_EQ(c1, 1);
    CHECK_EQ(c2, 1);
    mgr.unregisterCallback(EventType::RssiChanged);
}

// 测试4: 7种事件类型各自分发
static void testAllEventTypes() {
    TEST_CASE("7种事件类型各自分发");
    auto& mgr = getEventManager();
    int counts[7] = {0};
    mgr.registerCallback(EventType::InterfaceChanged, [&](const NetworkEvent&){counts[0]++;});
    mgr.registerCallback(EventType::ConnectionModeChanged, [&](const NetworkEvent&){counts[1]++;});
    mgr.registerCallback(EventType::NetworkQualityChanged, [&](const NetworkEvent&){counts[2]++;});
    mgr.registerCallback(EventType::TcpLossRateChanged, [&](const NetworkEvent&){counts[3]++;});
    mgr.registerCallback(EventType::RttChanged, [&](const NetworkEvent&){counts[4]++;});
    mgr.registerCallback(EventType::RssiChanged, [&](const NetworkEvent&){counts[5]++;});
    mgr.registerCallback(EventType::BluetoothDeviceChanged, [&](const NetworkEvent&){counts[6]++;});

    mgr.emitInterfaceChanged("m", "s");
    mgr.emitConnectionModeChanged("m", "s");
    mgr.emitNetworkQualityChanged("m", "d", "s");
    mgr.emitTcpLossRateChanged("m", "s");
    mgr.emitRttChanged("m", "s");
    mgr.emitRssiChanged("m", "s");
    mgr.emitBluetoothDeviceChanged("m", "s");

    for (int i = 0; i < 7; i++) {
        CHECK_EQ(counts[i], 1);
    }
    // 清理
    mgr.unregisterCallback(EventType::InterfaceChanged);
    mgr.unregisterCallback(EventType::ConnectionModeChanged);
    mgr.unregisterCallback(EventType::NetworkQualityChanged);
    mgr.unregisterCallback(EventType::TcpLossRateChanged);
    mgr.unregisterCallback(EventType::RttChanged);
    mgr.unregisterCallback(EventType::RssiChanged);
    mgr.unregisterCallback(EventType::BluetoothDeviceChanged);
}

// 测试5: 事件类型隔离 - A类型emit不影响B类型回调
static void testEventTypeIsolation() {
    TEST_CASE("事件类型隔离");
    auto& mgr = getEventManager();
    int interfaceCount = 0, rttCount = 0;
    mgr.registerCallback(EventType::InterfaceChanged,
        [&interfaceCount](const NetworkEvent&){interfaceCount++;});
    mgr.registerCallback(EventType::RttChanged,
        [&rttCount](const NetworkEvent&){rttCount++;});
    mgr.emitInterfaceChanged("m", "s");
    CHECK_EQ(interfaceCount, 1);
    CHECK_EQ(rttCount, 0);  // RTT回调不应被触发
    mgr.unregisterCallback(EventType::InterfaceChanged);
    mgr.unregisterCallback(EventType::RttChanged);
}

// 测试6: 事件数据正确传递
static void testEventDataPropagation() {
    TEST_CASE("事件数据正确传递");
    auto& mgr = getEventManager();
    std::string receivedMsg, receivedSrc;
    mgr.registerCallback(EventType::InterfaceChanged,
        [&receivedMsg, &receivedSrc](const NetworkEvent& ev) {
            receivedMsg = ev.message;
            receivedSrc = ev.source;
        });
    mgr.emitInterfaceChanged("hello事件", "src_module");
    CHECK_EQ(receivedMsg, "hello事件");
    CHECK_EQ(receivedSrc, "src_module");
    mgr.unregisterCallback(EventType::InterfaceChanged);
}

// 测试7: 多次emit累计触发
static void testMultipleEmits() {
    TEST_CASE("多次emit累计触发");
    auto& mgr = getEventManager();
    int count = 0;
    mgr.registerCallback(EventType::TcpLossRateChanged,
        [&count](const NetworkEvent&){count++;});
    for (int i = 0; i < 5; i++) {
        mgr.emitTcpLossRateChanged("m", "s");
    }
    CHECK_EQ(count, 5);
    mgr.unregisterCallback(EventType::TcpLossRateChanged);
}

int main(int /*argc*/, char* argv[]) {
    initTestLogging(argv[0]);
    testRegisterAndEmit();
    testUnregister();
    testMultipleCallbacks();
    testAllEventTypes();
    testEventTypeIsolation();
    testEventDataPropagation();
    testMultipleEmits();
    return printTestResult();
}

// test_event_manager.cpp
// 事件管理器单元测试
// 被测模块: event_manager.cpp（事件注册/分发/注销）
// 编译: g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Itest/unit -o test/bin/test_event_manager
//        test/unit/test_event_manager.cpp src/event_manager.cpp
//        src/logger.cpp -lglog `pkg-config --cflags --libs dbus-1`

#include "test_common.hpp"
#include "event_manager.hpp"
#include "server.hpp"
#include "dbus_service.hpp"

#include <atomic>
#include <thread>
#include <unordered_set>
#include <vector>

// 测试用 counter 记录器（定义于 mock_dbus_service.cpp），供并发原子性测试使用
namespace weaknet_dbus::test_recorder {
void resetCounters();
std::vector<int32_t> snapshotCounters();
}

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

// 测试8: 并发下 eventCounter 必须无重复（验证原子性）
//   emitEvent 内的 static eventCounter 被 ++ 的操作若非原子，多线程并发会丢更新，
//   导致传给 DbusService::emitSpecificSignal 的 counter 值出现重复。
//   本测试通过 mock 记录器收集所有 counter 值，断言其总数与唯一数均等于期望值。
static void testEventCounterAtomicUnderConcurrency() {
    TEST_CASE("并发下 eventCounter 无重复（原子性）");
    auto& mgr = getEventManager();

    // 构造 ServerContext + DbusService，使 emitEvent 走 counter 路径
    // （server_ctx_ && server_ctx_->service 非空）
    ServerContext ctx;
    DbusService svc(&ctx);
    ctx.service = &svc;
    mgr.startEventMonitoring(&ctx);

    const int N_THREADS = 10;
    const int N_PER_THREAD = 10000;
    const int EXPECTED_TOTAL = N_THREADS * N_PER_THREAD;

    test_recorder::resetCounters();

    auto worker = [&mgr](int /*id*/) {
        for (int i = 0; i < N_PER_THREAD; ++i) {
            mgr.emitRttChanged("concurrent", "test");
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(N_THREADS);
    for (int i = 0; i < N_THREADS; ++i) {
        threads.emplace_back(worker, i);
    }
    for (auto& t : threads) t.join();

    auto recorded = test_recorder::snapshotCounters();
    std::unordered_set<int32_t> unique(recorded.begin(), recorded.end());

    // 期望：所有 emit 都被记录（总数 == N_THREADS * N_PER_THREAD）
    CHECK_EQ(static_cast<int>(recorded.size()), EXPECTED_TOTAL);
    // 期望：counter 值无重复（唯一数 == 总数）。
    // 非原子 int32_t++ 在并发下丢更新 → 重复值 → 唯一数 < 总数 → 断言失败（RED）
    CHECK_EQ(static_cast<int>(unique.size()), EXPECTED_TOTAL);

    // 注：startEventMonitoring 设置的 server_ctx_ 指向本函数局部 ctx，
    // 本测试为 main 中最后一个，函数返回后不再有 emit，悬垂指针不会被解引用。
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
    testEventCounterAtomicUnderConcurrency();
    return printTestResult();
}

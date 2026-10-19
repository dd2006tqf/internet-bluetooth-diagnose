// test_event_manager_gtest.cpp
// Event Manager unit tests (Google Test version)
// Module under test: event_manager.cpp (event registration/dispatch/unregistration)
//
// O3-B 之后 EventManager 只做「回调分发」单通道——D-Bus 信号发射已停发
// （7 个信号零生产订阅者），因此本测试不再构造 ServerContext/DbusService，
// 只验证回调注册/注销/分发语义。

#include <gtest/gtest.h>
#include "event_manager.hpp"

using namespace weaknet_dbus;

// ============================================================================
// Test Fixture
// ============================================================================
class EventManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        mgr_ = &getEventManager();
        // 只绑定默认日志回调；O3-B 后不再需要 ServerContext/DbusService
        mgr_->startEventMonitoring();
    }

    void TearDown() override {
        mgr_->unregisterCallback(EventType::InterfaceChanged);
        mgr_->unregisterCallback(EventType::ConnectionModeChanged);
        mgr_->unregisterCallback(EventType::NetworkQualityChanged);
        mgr_->unregisterCallback(EventType::TcpLossRateChanged);
        mgr_->unregisterCallback(EventType::RttChanged);
        mgr_->unregisterCallback(EventType::RssiChanged);
        mgr_->unregisterCallback(EventType::BluetoothDeviceChanged);
    }

    NetworkEventManager* mgr_;
};

// ============================================================================
// Test Cases
// ============================================================================

// Test 1: Register callback then emit should trigger
TEST_F(EventManagerTest, RegisterAndEmit) {
    int callCount = 0;
    mgr_->registerCallback(EventType::InterfaceChanged,
        [&callCount](const NetworkEvent&) { callCount++; });

    mgr_->emitInterfaceChanged("test message", "test_source");
    EXPECT_EQ(callCount, 1);
}

// Test 2: After unregister, emit should not trigger
TEST_F(EventManagerTest, Unregister) {
    int callCount = 0;
    mgr_->registerCallback(EventType::RttChanged,
        [&callCount](const NetworkEvent&) { callCount++; });

    mgr_->unregisterCallback(EventType::RttChanged);
    mgr_->emitRttChanged("msg", "src");
    EXPECT_EQ(callCount, 0);
}

// Test 3: Multiple callbacks of same type should all trigger
TEST_F(EventManagerTest, MultipleCallbacks) {
    int c1 = 0, c2 = 0;
    mgr_->registerCallback(EventType::RssiChanged, [&c1](const NetworkEvent&) { c1++; });
    mgr_->registerCallback(EventType::RssiChanged, [&c2](const NetworkEvent&) { c2++; });

    mgr_->emitRssiChanged("msg", "src");
    EXPECT_EQ(c1, 1);
    EXPECT_EQ(c2, 1);
}

// Test 4: All 7 event types should dispatch correctly
TEST_F(EventManagerTest, AllEventTypes) {
    int counts[7] = {0};
    mgr_->registerCallback(EventType::InterfaceChanged, [&](const NetworkEvent&){counts[0]++;});
    mgr_->registerCallback(EventType::ConnectionModeChanged, [&](const NetworkEvent&){counts[1]++;});
    mgr_->registerCallback(EventType::NetworkQualityChanged, [&](const NetworkEvent&){counts[2]++;});
    mgr_->registerCallback(EventType::TcpLossRateChanged, [&](const NetworkEvent&){counts[3]++;});
    mgr_->registerCallback(EventType::RttChanged, [&](const NetworkEvent&){counts[4]++;});
    mgr_->registerCallback(EventType::RssiChanged, [&](const NetworkEvent&){counts[5]++;});
    mgr_->registerCallback(EventType::BluetoothDeviceChanged, [&](const NetworkEvent&){counts[6]++;});

    mgr_->emitInterfaceChanged("m", "s");
    mgr_->emitConnectionModeChanged("m", "s");
    mgr_->emitNetworkQualityChanged("m", "d", "s");
    mgr_->emitTcpLossRateChanged("m", "s");
    mgr_->emitRttChanged("m", "s");
    mgr_->emitRssiChanged("m", "s");
    mgr_->emitBluetoothDeviceChanged("m", "s");

    for (int i = 0; i < 7; i++) {
        EXPECT_EQ(counts[i], 1) << "Event type " << i << " was not dispatched";
    }
}

// Test 5: Event type isolation - emit type A should not affect type B callback
TEST_F(EventManagerTest, EventTypeIsolation) {
    int interfaceCount = 0, rttCount = 0;
    mgr_->registerCallback(EventType::InterfaceChanged,
        [&interfaceCount](const NetworkEvent&){interfaceCount++;});
    mgr_->registerCallback(EventType::RttChanged,
        [&rttCount](const NetworkEvent&){rttCount++;});

    mgr_->emitInterfaceChanged("m", "s");
    EXPECT_EQ(interfaceCount, 1);
    EXPECT_EQ(rttCount, 0);  // RTT callback should not be triggered
}

// Test 6: Event data propagation
TEST_F(EventManagerTest, EventDataPropagation) {
    std::string receivedMsg, receivedSrc;
    mgr_->registerCallback(EventType::InterfaceChanged,
        [&receivedMsg, &receivedSrc](const NetworkEvent& ev) {
            receivedMsg = ev.message;
            receivedSrc = ev.source;
        });

    mgr_->emitInterfaceChanged("hello event", "src_module");
    EXPECT_EQ(receivedMsg, "hello event");
    EXPECT_EQ(receivedSrc, "src_module");
}

// Test 7: Multiple emits should accumulate
TEST_F(EventManagerTest, MultipleEmits) {
    int count = 0;
    mgr_->registerCallback(EventType::TcpLossRateChanged,
        [&count](const NetworkEvent&){count++;});

    for (int i = 0; i < 5; i++) {
        mgr_->emitTcpLossRateChanged("m", "s");
    }
    EXPECT_EQ(count, 5);
}

// Test 8（已随 O3-B 移除）：原 EventCounterAtomicUnderConcurrency 验证的是
// emitEvent 内部 eventCounter++ 的原子性。该计数器只服务于 D-Bus 信号载荷
// （接收方去重/排序），而 7 个信号零生产订阅者、发射已全部停发——被测对象
// 本身不复存在，测试随之删除而非空转。

// ============================================================================
// main function
// ============================================================================
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

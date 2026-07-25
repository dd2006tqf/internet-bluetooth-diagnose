// mock_dbus_service.cpp
// DbusService 的最小桩实现，仅供单元测试链接使用。
//
// 设计目的：
//   event_manager.cpp 在 emitEvent() 中调用 DbusService::emitSpecificSignal /
//   emitNetworkQualitySignal，并传入递增的 eventCounter。本桩除满足链接器符号
//   需求外，还提供线程安全的 counter 记录器，供并发场景下的原子性测试使用。
//
// 注意：此文件仅用于测试编译链接，不应包含任何业务逻辑。

#include "dbus_service.hpp"

#include <cstdint>
#include <mutex>
#include <vector>

namespace weaknet_dbus {

// ============================================================================
// 测试用 counter 记录器（线程安全）
//   仅供 test_event_manager 的并发原子性测试使用。其它测试不走 emit 路径，
//   不会写入记录。记录器自身用 mutex 保护，避免成为新的竞态源。
// ============================================================================
namespace test_recorder {
static std::mutex g_mtx;
static std::vector<int32_t> g_counters;

void resetCounters() {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_counters.clear();
}

void recordCounter(int32_t counter) {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_counters.push_back(counter);
}

std::vector<int32_t> snapshotCounters() {
    std::lock_guard<std::mutex> lock(g_mtx);
    return g_counters;
}
}  // namespace test_recorder

// 构造函数：仅保存上下文指针（测试中可能为 nullptr 或指向测试构造的 ctx）
DbusService::DbusService(ServerContext* ctx) : ctx_(ctx) {}

// 桩实现：记录 counter 后返回 true，不实际发送 D-Bus 信号
bool DbusService::emitSpecificSignal(const std::string& /*signalName*/,
                                      const std::string& /*message*/,
                                      int32_t counter) {
    test_recorder::recordCounter(counter);
    return true;
}

// 桩实现：记录 counter 后返回 true，不实际发送 D-Bus 信号
bool DbusService::emitNetworkQualitySignal(const std::string& /*message*/,
                                            const std::string& /*details*/,
                                            int32_t counter) {
    test_recorder::recordCounter(counter);
    return true;
}

}  // namespace weaknet_dbus

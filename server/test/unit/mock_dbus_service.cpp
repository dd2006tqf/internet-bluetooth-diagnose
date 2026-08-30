// mock_dbus_service.cpp
// DbusService stub implementation for unit testing only.
//
// Design purpose:
//   event_manager.cpp calls DbusService::emitSpecificSignal / emitNetworkQualitySignal
//   in emitEvent(). This stub satisfies linker symbol requirements and provides
//   thread-safe counter recorder for concurrency atomicity tests.
//
// Note: This file is only for test compilation/linking, should not contain any business logic.

#include "dbus_service.hpp"
#include "server.hpp"
#include "net_info.hpp"
#include "database_manager.hpp"
#include "dns_monitor.hpp"
#include "wifi_packet_loss_monitor.hpp"
#include "http_latency_monitor.hpp"
#include "process_net_profiler.hpp"
#include "tcp_retransmit_monitor.hpp"
#include "bt_monitor.hpp"
#include "weak_netmgr.hpp"

#include <cstdint>
#include <mutex>
#include <vector>

namespace weaknet_dbus {

// ============================================================================
// Test counter recorder (thread-safe)
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

// ServerContext destructor stub (simplified, no resource cleanup needed for tests)
ServerContext::~ServerContext() {
    // Intentionally empty - test stub doesn't manage real resources
}

// Constructor: only saves context pointer
DbusService::DbusService(ServerContext* ctx) : ctx_(ctx) {}

// Stub implementation: record counter then return true
bool DbusService::emitSpecificSignal(const std::string& /*signalName*/,
                                      const std::string& /*message*/,
                                       int32_t counter) {
    test_recorder::recordCounter(counter);
    return true;
}

// Stub implementation: record counter then return true
bool DbusService::emitNetworkQualitySignal(const std::string& /*message*/,
                                            const std::string& /*details*/,
                                                   int32_t counter) {
    test_recorder::recordCounter(counter);
    return true;
}

}  // namespace weaknet_dbus

// mock_dbus_service.cpp
// ServerContext 析构桩（仅用于单元测试链接）。
//
// 设计目的：
//   ServerContext 的析构函数定义在 server.cpp（不进 server_lib，测试不链接），
//   而事件/生命周期类测试会构造 ServerContext——本桩仅为链接提供
//   ServerContext::~ServerContext 空实现（成员仍按声明顺序正常销毁）。
//
// O3-B 变更：此前本文件还提供 DbusService 构造桩与 emitSpecificSignal/
// emitNetworkQualitySignal 的计数桩（供 test_recorder 验证 D-Bus 发射路径）。
// 信号停发后测试不再构造 DbusService、也不再校验发射计数，相关桩已删除。
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
#include "tcp_conn_monitor.hpp"
#include "tcp_connect_monitor.hpp"
#include "active_connectivity_monitor.hpp"
#include "bt_monitor.hpp"
#include "weak_netmgr.hpp"

namespace weaknet_dbus {

// ServerContext destructor stub (simplified, no resource cleanup needed for tests)
ServerContext::~ServerContext() {
    // Intentionally empty - test stub doesn't manage real resources
}

}  // namespace weaknet_dbus

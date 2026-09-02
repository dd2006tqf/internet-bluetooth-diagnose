/**
 * @file monitor_registry.hpp
 * @brief 监控器插件静态注册表
 *
 * 编译期注册 + 运行期按 order 实例化的插件管理。
 *
 * 用法（在每个插件 .cpp 末尾注册自己）：
 *   static const bool _rtt_registered =
 *       (weaknet_dbus::registerPlugin("rtt",
 *           []{ return std::make_unique<RttPlugin>(); }), true);
 *
 * 启动/收尾（server.cpp）：
 *   auto plugins = weaknet_dbus::instantiateAllPlugins();
 *   for (auto& p : plugins) p->init(&ctx);
 *   for (auto& p : plugins) p->start(&ctx);
 *   // 停止按逆序
 */

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "monitor_plugin.hpp"

namespace weaknet_dbus {

using PluginFactory = std::function<std::unique_ptr<IMonitorPlugin>()>;

/**
 * @brief 注册插件工厂（编译期静态注册）
 *
 * @param name    插件唯一名（重复名会被拒绝并记录警告）
 * @param factory 工厂函数，返回插件实例
 */
void registerPlugin(const char* name, PluginFactory factory);

/**
 * @brief 实例化所有已注册插件，并按 order() 升序排序
 *
 * @return 按启动顺序排列的插件实例列表（停止时应逆序遍历）
 */
std::vector<std::unique_ptr<IMonitorPlugin>> instantiateAllPlugins();

// ---- 内置插件注册入口（server.cpp 启动前调用） ----

/// 注册 9 个传统监控器插件（iface/using_iface/rtt/jitter/rssi/tcp_loss/traffic/quality/bluetooth）
void registerBuiltinPlugins();

/// 注册 6 个 eBPF 监控器插件（dns/wifi_loss/http_latency/process_profiler/tcp_retrans/tcp_conn）
void registerEbpfPlugins();

}  // namespace weaknet_dbus
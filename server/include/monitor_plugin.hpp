/**
 * @file monitor_plugin.hpp
 * @brief 监控器插件生命周期接口
 *
 * 将 15 个监控器线程从 server.cpp 的硬编码启动序列，抽象为注册表驱动的插件。
 * 每个插件实现 init / start / stop 三阶段生命周期：
 *   - init   阶段1：加载资源、解析配置（不开线程）
 *   - start  阶段2：启动线程（含 enabled 守卫）
 *   - stop   阶段3：请求该插件停止并 join 其线程；服务级 join 仍由协调器负责
 *
 * 当前阶段由 MonitorManager 长期持有插件实例；线程/资源的独立所有权迁移
 * 在后续阶段完成。静态注册表不等于 dlopen 动态加载。
 */

#pragma once

#include <string>
#include <vector>

namespace weaknet_dbus {

class ServerContext;

/// 监控器插件生命周期接口
class IMonitorPlugin {
public:
    virtual ~IMonitorPlugin() = default;

    /// 插件唯一名（如 "rtt"、"dns"）；用于日志与注册表去重
    virtual const char* name() const = 0;

    /// 启动优先级：小者先启动（start 正序、stop 逆序）；默认 100
    virtual int order() const { return 100; }

    /// 返回本插件依赖的其他插件名称；默认无依赖。
    virtual std::vector<std::string> dependencies() const { return {}; }

    /// 阶段1：加载资源、解析配置；失败时 start 不得执行。
    virtual bool init(ServerContext* ctx) = 0;

    /// 阶段2：启动线程（内部应做 enabled 守卫）。
    /// @return true 成功；false 失败（不影响其他插件启动）
    virtual bool start(ServerContext* ctx) = 0;

    /// 阶段3：停止线程、释放插件自身持有的资源。
    /// 由 server.cpp 在 join 全部线程后、~ServerContext 前按 order() 逆序调用。
    /// 当前各插件实现为空（资源由 ctx 的 unique_ptr 统一回收），预留给未来自管资源的插件使用。
    virtual void stop() = 0;
};

}  // namespace weaknet_dbus
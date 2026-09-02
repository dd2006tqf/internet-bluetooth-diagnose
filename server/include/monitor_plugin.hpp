/**
 * @file monitor_plugin.hpp
 * @brief 监控器插件生命周期接口
 *
 * 将 13 个监控线程从 server.cpp 的硬编码启动序列，抽象为注册表驱动的插件。
 * 每个插件实现 init / start / stop 三阶段生命周期：
 *   - init   阶段1：加载资源、解析配置（不开线程）
 *   - start  阶段2：启动线程（含 enabled 守卫）
 *   - stop   阶段3：停线程、释放资源（join 归属见实现约定）
 *
 * 设计约束：
 *   - 不改监控器内部采集逻辑，只做生命周期包装（迁移成本最低）
 *   - 依赖顺序通过 order() 表达：共享 eBPF 资源（flow_rate）的持有者 order 小
 *   - 静态注册表（非 dlopen），接口形状为将来 dlopen 预留
 */

#pragma once

#include <string>

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

    /// 阶段1：加载资源、解析配置。不开线程。
    /// @param ctx 全局上下文（监控器实例、线程句柄、cfg）
    /// @return true 成功；false 失败（start 阶段将跳过该插件）
    virtual bool init(ServerContext* ctx) = 0;

    /// 阶段2：启动线程（内部应做 enabled 守卫）。
    /// @return true 成功；false 失败（不影响其他插件启动）
    virtual bool start(ServerContext* ctx) = 0;

    /// 阶段3：停止线程、释放资源。停止顺序 = start 逆序。
    virtual void stop() = 0;
};

}  // namespace weaknet_dbus
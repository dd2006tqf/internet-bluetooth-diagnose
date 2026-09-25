/**
 * @file monitor_manager.hpp
 * @brief 进程内监控器插件生命周期协调器
 *
 * 长期持有静态注册表实例，统一初始化、启动、状态记录和收尾。
 * 插件对象与 worker 线程句柄均由插件自身持有：`stop()` 内部置停止标志并
 * join 自己的 worker，因此 `stopAll()` 返回后所有插件线程已经退出，
 * 调用方无需（也不应）在它之后再 join 插件线程。
 * server.cpp 只需在 `stopAll()` 之后 join 两个服务级线程
 * （active_probe_thread / history_thread）。
 */

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "monitor_plugin.hpp"

namespace weaknet_dbus {

class ServerContext;

enum class MonitorState {
    Registered,
    Initializing,
    Initialized,
    Starting,
    Running,
    Stopping,
    Stopped,
    Failed,
    ConfiguredDisabled,
};

struct MonitorStatus {
    std::string name;
    MonitorState state = MonitorState::Registered;
    bool desired_enabled = true;
    std::string error;
    std::string changed_at;
    uint64_t generation = 0;
};

const char* monitorStateName(MonitorState state);

/**
 * @brief 统一持有并协调所有已注册的内置插件
 *
 * 当前对象是非线程安全生命周期的唯一协调入口；方法内部串行化插件
 * 状态迁移，避免同一插件被并发 start/stop。插件实例由本类 owning。
 */
class MonitorManager {
public:
    MonitorManager(ServerContext* ctx,
                   std::vector<std::unique_ptr<IMonitorPlugin>> plugins);
    ~MonitorManager();

    MonitorManager(const MonitorManager&) = delete;
    MonitorManager& operator=(const MonitorManager&) = delete;

    /// 按插件 order 初始化并启动；单个失败不会阻止其他插件尝试启动。
    bool startConfigured();

    /// 逆序停止全部处于 Running/Starting/Failed 的插件。
    /// 每个插件的 stop() 内部会置停止标志并 join 自己的 worker，
    /// 因此本调用返回后插件线程均已退出（无需外部再 join）。
    bool stopAll();

    /// 返回全部插件的稳定状态快照。
    std::vector<MonitorStatus> list() const;
    std::vector<std::string> dependencies(const std::string& name) const;

    /// 设置运行时 override 文件路径；空路径表示不持久化。
    void setOverridePath(std::string path);
    void setDesiredOverride(const std::string& name, bool enabled);
    /// 保存当前 desired_enabled；仅由显式配置持久化入口调用。
    bool saveOverrides(std::string* error) const;
    bool loadOverrides(std::string* error);

    /// 状态变更回调（name, state name）；用于 D-Bus 信号/事件，不持有锁。
    void setStateChangeCallback(std::function<void(const std::string&, const std::string&)> cb);

    bool status(const std::string& name, MonitorStatus* out) const;

    /// 阶段二实现真正的单插件运行时启停前，接口先明确拒绝该操作。
    bool enable(const std::string& name, std::string* error);
    bool disable(const std::string& name, std::string* error);
    bool restart(const std::string& name, std::string* error);

private:
    struct Entry {
        std::unique_ptr<IMonitorPlugin> plugin;
        MonitorStatus status;
    };

    static void invokeStateCallback(
        const std::function<void(const std::string&, const std::string&)>& cb,
        const MonitorStatus& status);

    Entry* findLocked(const std::string& name);
    const Entry* findLocked(const std::string& name) const;

    ServerContext* ctx_ = nullptr;  // non-owning; manager 不跨 context 生命周期
    std::vector<Entry> entries_;    // 已按插件 order 排列
    std::string override_path_;
    std::map<std::string, bool> desired_overrides_;
    std::function<void(const std::string&, const std::string&)> state_change_cb_;
    mutable std::mutex mutex_;
    bool started_ = false;
    bool stopped_ = false;
};

}  // namespace weaknet_dbus

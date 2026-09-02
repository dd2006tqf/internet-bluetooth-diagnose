/**
 * @file monitor_registry.cpp
 * @brief 监控器插件静态注册表实现
 *
 * 内部用函数级静态变量持有注册表（构造时注册，跨编译单元安全）。
 * instantiateAllPlugins 按 order() 升序返回，停止时调用方逆序遍历。
 */

#include "monitor_registry.hpp"

#include <algorithm>
#include <map>
#include <mutex>

#include "logger.hpp"

namespace weaknet_dbus {

namespace {

struct Registry {
    std::map<std::string, PluginFactory> factories;  ///< 名字 → 工厂（map 保序，去重）
    std::mutex mutex;
};

Registry& registry() {
    static Registry r;
    return r;
}

}  // namespace

void registerPlugin(const char* name, PluginFactory factory) {
    if (!name || !factory) return;
    auto& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    if (r.factories.find(name) != r.factories.end()) {
        LOG_WARNING(LogModule::SYSTEM, "monitor registry: duplicate plugin name '" << name
                    << "' ignored");
        return;
    }
    r.factories.emplace(name, std::move(factory));
}

std::vector<std::unique_ptr<IMonitorPlugin>> instantiateAllPlugins() {
    auto& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);

    std::vector<std::unique_ptr<IMonitorPlugin>> plugins;
    plugins.reserve(r.factories.size());
    for (auto& [name, factory] : r.factories) {
        auto plugin = factory();
        if (plugin) {
            plugin->name();  // 触发虚调用，确保动态类型完整
            plugins.push_back(std::move(plugin));
        }
    }

    // 按 order() 升序（依赖在前）
    std::stable_sort(plugins.begin(), plugins.end(),
                     [](const std::unique_ptr<IMonitorPlugin>& a,
                        const std::unique_ptr<IMonitorPlugin>& b) {
                         return a->order() < b->order();
                     });
    return plugins;
}

}  // namespace weaknet_dbus
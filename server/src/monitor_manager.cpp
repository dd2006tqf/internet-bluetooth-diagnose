/**
 * @file monitor_manager.cpp
 * @brief 进程内监控器插件生命周期协调器实现
 */

#include "monitor_manager.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

#include "serializer.hpp"
#include "server.hpp"

namespace weaknet_dbus {

namespace {
std::string nowUtc() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}
}  // namespace

const char* monitorStateName(MonitorState state) {
    switch (state) {
    case MonitorState::Registered: return "registered";
    case MonitorState::Initializing: return "initializing";
    case MonitorState::Initialized: return "initialized";
    case MonitorState::Starting: return "starting";
    case MonitorState::Running: return "running";
    case MonitorState::Stopping: return "stopping";
    case MonitorState::Stopped: return "stopped";
    case MonitorState::Failed: return "failed";
    case MonitorState::ConfiguredDisabled: return "configured_disabled";
    }
    return "unknown";
}

MonitorManager::MonitorManager(
    ServerContext* ctx, std::vector<std::unique_ptr<IMonitorPlugin>> plugins)
    : ctx_(ctx) {
    entries_.reserve(plugins.size());
    for (auto& plugin : plugins) {
        if (!plugin) continue;
        MonitorStatus status;
        status.name = plugin->name() ? plugin->name() : "";
        if (status.name.empty()) continue;
        entries_.push_back(Entry{std::move(plugin), std::move(status)});
    }
    std::stable_sort(entries_.begin(), entries_.end(),
        [](const Entry& lhs, const Entry& rhs) {
            return lhs.plugin->order() < rhs.plugin->order();
        });
}

MonitorManager::~MonitorManager() = default;

void MonitorManager::setOverridePath(std::string path) {
    std::lock_guard<std::mutex> lock(mutex_);
    override_path_ = std::move(path);
}

bool MonitorManager::saveOverrides(std::string* error) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (override_path_.empty()) {
        if (error) *error = "runtime override path is empty";
        return false;
    }
    std::vector<uint8_t> bytes;
    bytes.push_back('W'); bytes.push_back('N'); bytes.push_back('R'); bytes.push_back('O');
    serializeInt32(1, bytes);
    for (const auto& entry : entries_) {
        serializeString(entry.status.name, bytes);
        serializeInt32(entry.status.desired_enabled ? 1 : 0, bytes);
    }

    // 写入数据目录（overrides 是持久化状态，不能走 serializer 的 /tmp 安全限制）。
    const std::string tmp_path = override_path_ + ".tmp";
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "cannot open override file for write: " + tmp_path;
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) {
        if (error) *error = "cannot write override file: " + tmp_path;
        std::remove(tmp_path.c_str());
        return false;
    }
    if (std::rename(tmp_path.c_str(), override_path_.c_str()) != 0) {
        if (error) *error = "cannot commit override file: " + override_path_;
        std::remove(tmp_path.c_str());
        return false;
    }
    return true;
}

bool MonitorManager::loadOverrides(std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (override_path_.empty()) return true;
    std::ifstream in(override_path_, std::ios::binary);
    if (!in) return true;  // 文件不存在不是错误，回落既有配置
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    if (bytes.size() < 8 || bytes[0] != 'W' || bytes[1] != 'N' ||
        bytes[2] != 'R' || bytes[3] != 'O') {
        if (error) *error = "invalid runtime override header";
        return false;
    }
    size_t offset = 4;
    int32_t version = 0;
    if (!deserializeInt32(bytes, offset, version) || version != 1) {
        if (error) *error = "unsupported runtime override version";
        return false;
    }
    while (offset < bytes.size()) {
        std::string name;
        int32_t enabled = 0;
        if (!deserializeString(bytes, offset, name) ||
            !deserializeInt32(bytes, offset, enabled)) {
            if (error) *error = "truncated runtime override";
            return false;
        }
        if (!setMonitorEnabled(ctx_ ? &ctx_->cfg : nullptr, name, enabled != 0)) {
            if (error) *error = "unknown monitor in runtime override: " + name;
            return false;
        }
    }
    return true;
}

MonitorManager::Entry* MonitorManager::findLocked(const std::string& name) {
    for (auto& entry : entries_) {
        if (entry.status.name == name) return &entry;
    }
    return nullptr;
}

const MonitorManager::Entry* MonitorManager::findLocked(const std::string& name) const {
    for (const auto& entry : entries_) {
        if (entry.status.name == name) return &entry;
    }
    return nullptr;
}

bool MonitorManager::startConfigured() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_ || stopped_) return false;

    bool all_ok = true;
    for (auto& entry : entries_) {
        entry.status.state = MonitorState::Initializing;
        entry.status.error.clear();
        entry.status.changed_at = nowUtc();
        if (!entry.plugin->init(ctx_)) {
            entry.status.state = MonitorState::Failed;
            entry.status.error = "plugin initialization failed";
            all_ok = false;
            continue;
        }
        entry.status.state = MonitorState::Initialized;
        bool enabled = true;
        if (ctx_ && !getMonitorEnabled(ctx_->cfg, entry.status.name, &enabled)) {
            entry.status.state = MonitorState::Failed;
            entry.status.error = "missing monitor configuration";
            all_ok = false;
            continue;
        }
        entry.status.desired_enabled = enabled;
        if (!enabled) entry.status.state = MonitorState::ConfiguredDisabled;
    }

    for (auto& entry : entries_) {
        if (entry.status.state != MonitorState::Initialized) continue;
        for (const auto& dependency : entry.plugin->dependencies()) {
            const auto* dependency_entry = findLocked(dependency);
            if (!dependency_entry ||
                (dependency_entry->status.state != MonitorState::Running &&
                 dependency_entry->status.state != MonitorState::Initialized)) {
                entry.status.state = MonitorState::Failed;
                entry.status.error = "dependency is unavailable: " + dependency;
                all_ok = false;
                break;
            }
        }
    }

    for (auto& entry : entries_) {
        if (entry.status.state != MonitorState::Initialized) continue;
        entry.status.state = MonitorState::Starting;
        if (!entry.plugin->start(ctx_)) {
            entry.status.state = MonitorState::Failed;
            entry.status.error = "plugin start failed";
            all_ok = false;
            continue;
        }
        entry.status.state = MonitorState::Running;
        entry.status.changed_at = nowUtc();
        ++entry.status.generation;
    }
    started_ = true;
    return all_ok;
}

bool MonitorManager::stopAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return true;
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (it->status.state == MonitorState::Running ||
            it->status.state == MonitorState::Starting ||
            it->status.state == MonitorState::Failed) {
            it->status.state = MonitorState::Stopping;
            it->plugin->stop();
            it->status.state = MonitorState::Stopped;
            it->status.changed_at = nowUtc();
        }
    }
    stopped_ = true;
    return true;
}

std::vector<MonitorStatus> MonitorManager::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MonitorStatus> result;
    result.reserve(entries_.size());
    for (const auto& entry : entries_) result.push_back(entry.status);
    return result;
}

std::vector<std::string> MonitorManager::dependencies(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* entry = findLocked(name);
    return entry ? entry->plugin->dependencies() : std::vector<std::string>{};
}

void MonitorManager::setStateChangeCallback(
    std::function<void(const std::string&, const std::string&)> cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_change_cb_ = std::move(cb);
}

void MonitorManager::invokeStateCallback(
    const std::function<void(const std::string&, const std::string&)>& cb,
    const MonitorStatus& status) {
    if (cb) cb(status.name, monitorStateName(status.state));
}

bool MonitorManager::status(const std::string& name, MonitorStatus* out) const {
    if (!out) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* entry = findLocked(name);
    if (!entry) return false;
    *out = entry->status;
    return true;
}

bool MonitorManager::enable(const std::string& name, std::string* error) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto* entry = findLocked(name);
    if (!entry) {
        if (error) *error = "unknown monitor: " + name;
        return false;
    }
    if (stopped_) {
        if (error) *error = "monitor manager is stopped";
        return false;
    }
    if (entry->status.state == MonitorState::Running) {
        if (error) *error = "monitor already running: " + name;
        return false;
    }
    for (const auto& dependency : entry->plugin->dependencies()) {
        const auto* dependency_entry = findLocked(dependency);
        if (!dependency_entry || dependency_entry->status.state != MonitorState::Running) {
            if (error) *error = "dependency is not running: " + dependency;
            return false;
        }
    }
    if (entry->status.state == MonitorState::Starting ||
        entry->status.state == MonitorState::Stopping) {
        if (error) *error = "monitor transition in progress: " + name;
        return false;
    }
    if (ctx_) {
        setMonitorEnabled(&ctx_->cfg, name, true);
    }
    entry->status.desired_enabled = true;
    entry->status.state = MonitorState::Starting;
    entry->status.error.clear();
    if (!entry->plugin->start(ctx_)) {
        entry->status.state = MonitorState::Failed;
        entry->status.error = "plugin start failed";
        entry->status.changed_at = nowUtc();
        if (error) *error = entry->status.error;
        return false;
    }
    entry->status.state = MonitorState::Running;
    entry->status.changed_at = nowUtc();
    ++entry->status.generation;
    const auto callback = state_change_cb_;
    const MonitorStatus status = entry->status;
    lock.unlock();
    invokeStateCallback(callback, status);
    return true;
}

bool MonitorManager::disable(const std::string& name, std::string* error) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto* entry = findLocked(name);
    if (!entry) {
        if (error) *error = "unknown monitor: " + name;
        return false;
    }
    if (entry->status.state == MonitorState::Stopped ||
        entry->status.state == MonitorState::ConfiguredDisabled ||
        entry->status.state == MonitorState::Initialized) {
        if (error) *error = "monitor already stopped: " + name;
        return false;
    }
    if (entry->status.state != MonitorState::Running &&
        entry->status.state != MonitorState::Failed) {
        if (error) *error = "monitor is not stoppable: " + name;
        return false;
    }
    for (const auto& candidate : entries_) {
        for (const auto& dependency : candidate.plugin->dependencies()) {
            if (dependency == name && candidate.status.state == MonitorState::Running) {
                if (error) *error = "monitor is required by: " + candidate.status.name;
                return false;
            }
        }
    }
    if (ctx_) setMonitorEnabled(&ctx_->cfg, name, false);
    entry->status.desired_enabled = false;
    entry->status.state = MonitorState::Stopping;
    entry->plugin->stop();
    entry->status.state = MonitorState::Stopped;
    entry->status.changed_at = nowUtc();
    const auto callback = state_change_cb_;
    const MonitorStatus status = entry->status;
    lock.unlock();
    invokeStateCallback(callback, status);
    return true;
}

bool MonitorManager::restart(const std::string& name, std::string* error) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto* entry = findLocked(name);
    if (!entry) {
        if (error) *error = "unknown monitor: " + name;
        return false;
    }
    if (stopped_ || entry->status.state == MonitorState::Starting ||
        entry->status.state == MonitorState::Stopping) {
        if (error) *error = stopped_ ? "monitor manager is stopped"
                                     : "monitor transition in progress: " + name;
        return false;
    }
    for (const auto& dependency : entry->plugin->dependencies()) {
        const auto* dependency_entry = findLocked(dependency);
        if (!dependency_entry || dependency_entry->status.state != MonitorState::Running) {
            if (error) *error = "dependency is not running: " + dependency;
            return false;
        }
    }
    if (entry->status.state == MonitorState::Running ||
        entry->status.state == MonitorState::Failed) {
        entry->status.state = MonitorState::Stopping;
        entry->plugin->stop();
        entry->status.state = MonitorState::Stopped;
    }
    if (ctx_) setMonitorEnabled(&ctx_->cfg, name, true);
    entry->status.desired_enabled = true;
    entry->status.state = MonitorState::Starting;
    entry->status.error.clear();
    if (!entry->plugin->start(ctx_)) {
        entry->status.state = MonitorState::Failed;
        entry->status.error = "plugin start failed";
        entry->status.changed_at = nowUtc();
        if (error) *error = entry->status.error;
        return false;
    }
    entry->status.state = MonitorState::Running;
    entry->status.changed_at = nowUtc();
    ++entry->status.generation;
    const auto callback = state_change_cb_;
    const MonitorStatus status = entry->status;
    lock.unlock();
    invokeStateCallback(callback, status);
    return true;
}

}  // namespace weaknet_dbus

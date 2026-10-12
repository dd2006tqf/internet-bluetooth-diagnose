#include "bt_hci_connection_tracker.hpp"
#include <algorithm>
#include <cctype>

namespace weaknet {

namespace {

std::string makeSlotKey(const std::string& controller, uint16_t handle) {
    return controller + ":" + std::to_string(handle & 0x0FFF);
}

std::string normalizeMac(const std::string& mac) {
    if (mac.empty()) return "";
    std::string out;
    out.reserve(mac.size());
    for (char c : mac) {
        out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return out;
}

} // namespace

HciConnectionKey BtHciConnectionTracker::onConnectionEstablished(
    const std::string& controller,
    uint16_t handle,
    const std::string& mac,
    const std::string& addr_type) {

    uint16_t valid_handle = handle & 0x0FFF;
    std::string slot_key = makeSlotKey(controller, valid_handle);
    std::string clean_mac = normalizeMac(mac);

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = active_map_.find(slot_key);
    if (it != active_map_.end() && it->second.is_active) {
        // 重复通知幂等处理：若 MAC 一致，直接复用当前代次
        if (!clean_mac.empty() && it->second.device_mac == clean_mac) {
            return it->second.key;
        }
        // 若 MAC 发生冲突或前连接未正常收到 disconnect，强制递增代次
        it->second.is_active = false;
        it->second.disconnected_at = std::chrono::steady_clock::now();
    }

    uint32_t next_gen = ++generation_counter_[slot_key];

    HciConnectionKey key{controller, valid_handle, next_gen};
    HciConnectionRecord record;
    record.key = key;
    record.device_mac = clean_mac;
    record.address_type = addr_type.empty() ? "UNKNOWN" : addr_type;
    record.attribution_scope = clean_mac.empty() ? "controller" : "device";
    record.connected_at = std::chrono::steady_clock::now();
    record.is_active = true;

    active_map_[slot_key] = record;
    return key;
}

bool BtHciConnectionTracker::onConnectionDisconnected(
    const std::string& controller, uint16_t handle) {

    uint16_t valid_handle = handle & 0x0FFF;
    std::string slot_key = makeSlotKey(controller, valid_handle);

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = active_map_.find(slot_key);
    if (it == active_map_.end() || !it->second.is_active) {
        return false;
    }

    it->second.is_active = false;
    it->second.disconnected_at = std::chrono::steady_clock::now();
    return true;
}

std::optional<HciConnectionRecord> BtHciConnectionTracker::getActiveConnection(
    const std::string& controller, uint16_t handle) const {

    uint16_t valid_handle = handle & 0x0FFF;
    std::string slot_key = makeSlotKey(controller, valid_handle);

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = active_map_.find(slot_key);
    if (it != active_map_.end() && it->second.is_active) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<HciConnectionRecord> BtHciConnectionTracker::getActiveConnections(
    const std::string& controller) const {

    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<HciConnectionRecord> res;
    for (const auto& [k, v] : active_map_) {
        if (v.is_active && v.key.controller_id == controller) {
            res.push_back(v);
        }
    }
    return res;
}

void BtHciConnectionTracker::resetController(const std::string& controller) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    for (auto& [k, v] : active_map_) {
        if (v.key.controller_id == controller && v.is_active) {
            v.is_active = false;
            v.disconnected_at = now;
            // 控制器复位时提前递增代次计数，避免硬件重启后立即重分配同句柄造成串号
            generation_counter_[k]++;
        }
    }
}

size_t BtHciConnectionTracker::activeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const auto& [k, v] : active_map_) {
        if (v.is_active) count++;
    }
    return count;
}

} // namespace weaknet

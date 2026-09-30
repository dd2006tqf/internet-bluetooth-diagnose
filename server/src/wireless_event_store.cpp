/**
 * @file wireless_event_store.cpp
 * @brief 规范化无线设备事件存储实现
 */

#include "wireless_event_store.hpp"

#include <algorithm>

#include "database_manager.hpp"
#include "logger.hpp"

namespace weaknet_dbus {

WirelessEventStore::WirelessEventStore(DatabaseManager* db, WirelessEventStoreConfig cfg)
    : db_(db), cfg_(std::move(cfg)) {
    // 环形缓冲按容量预留，避免运行期反复扩容
    (void)0;
}

bool WirelessEventStore::recordEvent(const WirelessDeviceEvent& event) {
    std::lock_guard<std::mutex> lock(mu_);

    // 补齐来源身份：Phase 1 运行模型是"1 Gateway = 1 Site"，
    // 事件若未自带 site_id/gateway_id（如来自测试或未来的其它生产者），
    // 用 store 配置兜底，保证落库行永远可回答"这个事件是哪台探针看到的"。
    WirelessDeviceEvent ev = event;
    if (ev.gateway_id.empty()) ev.gateway_id = cfg_.gateway_id;
    if (ev.site_id.empty()) {
        ev.site_id = cfg_.site_id.empty() ? ev.gateway_id : cfg_.site_id;
    }

    ring_.push_back(ev);
    while (ring_.size() > cfg_.ring_capacity) {
        ring_.pop_front();
    }
    ++total_recorded_;

    if (db_) {
        const bool ok = db_->insertDeviceEvent(
            ev.event_id,
            static_cast<int64_t>(ev.timestamp_ms),
            ev.site_id,
            ev.gateway_id,
            toString(ev.protocol),
            ev.device_address,
            toString(ev.address_type),
            ev.hci_index,
            toString(ev.event_type),
            ev.rssi_at_event_dbm,
            ev.raw_reason_code,
            toString(ev.reason),
            toString(ev.source),
            ev.source_detail,
            ev.suspected_cause,
            ev.details_json);
        if (!ok) {
            ++persist_failures_;
            LOG_WARNING(LogModule::BLUETOOTH,
                        "WirelessEventStore: failed to persist event " << ev.event_id
                        << " (device=" << ev.device_address
                        << ", type=" << toString(ev.event_type) << ")");
        }
    }

    return true;
}

std::vector<WirelessDeviceEvent> WirelessEventStore::recentEvents(
    const std::string& device_address, size_t limit) const {
    std::lock_guard<std::mutex> lock(mu_);

    std::vector<WirelessDeviceEvent> out;
    if (limit == 0) return out;

    // ring_ 是时间正序，这里倒序遍历以获得"最近优先"
    for (auto it = ring_.rbegin(); it != ring_.rend(); ++it) {
        if (!device_address.empty() && it->device_address != device_address) continue;
        out.push_back(*it);
        if (out.size() >= limit) break;
    }
    return out;
}

uint64_t WirelessEventStore::totalRecorded() const {
    std::lock_guard<std::mutex> lock(mu_);
    return total_recorded_;
}

uint64_t WirelessEventStore::persistFailures() const {
    std::lock_guard<std::mutex> lock(mu_);
    return persist_failures_;
}

std::string WirelessEventStore::queryPersisted(const std::string& device_address,
                                               const std::string& event_type,
                                               int64_t start_ms,
                                               int64_t end_ms,
                                               int limit) const {
    if (!db_) return "[]";
    // 刻意不持有本类 mutex_：DB 查询走 DatabaseManager 自己的锁，
    // 避免"store 锁 -> db 锁"的嵌套顺序造成潜在死锁。
    return db_->queryDeviceEvents(device_address, event_type, start_ms, end_ms, limit);
}

bool WirelessEventStore::saveDeviceBaseline(const DeviceLinkProfile& profile) {
    if (!db_) return false;
    return db_->upsertDeviceBaseline(
        profile.key.site_id,
        profile.key.gateway_id,
        profile.key.hci_index,
        toString(profile.key.protocol),
        toString(profile.key.address_type),
        profile.key.device_address,
        profile.baseline_rssi_dbm,
        profile.min_seen_rssi_dbm,
        profile.max_seen_rssi_dbm,
        profile.baseline_sample_count,
        static_cast<int64_t>(profile.first_seen_ms),
        static_cast<int64_t>(profile.last_seen_ms),
        toString(profile.state),
        static_cast<int64_t>(profile.updated_at_ms));
}

std::string WirelessEventStore::queryDeviceBaselines(const std::string& device_address,
                                                    int limit) const {
    if (!db_) return "[]";
    return db_->queryDeviceBaselines(device_address, limit);
}

void WirelessEventStore::clearMemory() {
    std::lock_guard<std::mutex> lock(mu_);
    ring_.clear();
}

}  // namespace weaknet_dbus

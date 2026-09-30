/**
 * @file wireless_event_store.cpp
 * @brief 规范化无线设备事件存储实现
 */

#include "wireless_event_store.hpp"

#include <algorithm>

#include "database_manager.hpp"
#include "logger.hpp"
#include "site_incident_correlator.hpp"

namespace weaknet_dbus {

WirelessEventStore::WirelessEventStore(DatabaseManager* db, WirelessEventStoreConfig cfg)
    : db_(db), cfg_(std::move(cfg)) {
    // 环形缓冲按容量预留，避免运行期反复扩容
    (void)0;
}

bool WirelessEventStore::recordEvent(const WirelessDeviceEvent& event) {
    // 待投递给关联器的事件快照。**必须在锁内拷贝**：调用方可能在栈上复用
    // 同一个 event 对象，锁外再引用它就成了悬垂读（这不是假设——单元测试与
    // 消费循环都会循环复用同一个变量）。
    std::optional<WirelessDeviceEvent> to_observe;

    {
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

        // 只有真的落库成功的事件才参与区域关联：关联窗口的证据回链要求
        // site_incident_events 能 JOIN 到 device_events 行，未落库的事件
        // 关联出来的 incident 查不出设备清单。db_ 为空（纯内存模式）时同样跳过。
        bool persist_ok = false;
        if (db_) {
            persist_ok = db_->insertDeviceEvent(
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
            if (!persist_ok) {
                ++persist_failures_;
                LOG_WARNING(LogModule::BLUETOOTH,
                            "WirelessEventStore: failed to persist event " << ev.event_id
                            << " (device=" << ev.device_address
                            << ", type=" << toString(ev.event_type) << ")");
            }
        }

        // Phase 3a：把同一条事件投递给区域级关联器，产出/推进 SiteIncident。
        // 关联发生在**落库之后**：即使关联器失败，设备事件这一**事实**已经持久化。
        if (correlator_ && persist_ok) {
            to_observe = std::move(ev);
        }
    }

    // 关联调用刻意放在锁外：correlator 有独立 mutex_，且会去拿
    // DatabaseManager::mutex_。在持有 mu_ 时调用就形成
    // store.mu_ -> correlator.mutex_ -> db.mutex_ 的三段嵌套，
    // 一旦任何一处反向加锁即死锁。
    if (to_observe.has_value()) {
        correlator_->observe(*to_observe);
    }

    return true;
}

void WirelessEventStore::setIncidentCorrelator(SiteIncidentCorrelator* correlator) {
    std::lock_guard<std::mutex> lock(mu_);
    correlator_ = correlator;
}

size_t WirelessEventStore::tickIncidents(uint64_t now_ms) {
    // 关联器指针只在插件 start/stop 时变更，且 stop 会先停消费线程，
    // 因此这里不加锁读取是安全的（与 queryPersisted 同一纪律：不制造
    // store 锁 -> 关联器锁的嵌套）。
    if (!correlator_) return 0;
    return correlator_->tick(now_ms).size();
}

std::string WirelessEventStore::querySiteIncidents(const std::string& state,
                                                   int64_t start_ms,
                                                   int64_t end_ms,
                                                   int limit) const {
    if (!correlator_) return "[]";
    return correlator_->queryPersisted(state, start_ms, end_ms, limit, true);
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

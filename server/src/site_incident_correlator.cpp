/**
 * @file site_incident_correlator.cpp
 * @brief 区域级异常关联器实现
 *
 * 锁纪律（修改前必须保持）：
 *   WirelessEventStore::recordEvent 在**释放自身锁之后**调用 observe()，
 *   因此全局的加锁方向恒为
 *
 *       （无）-> correlator.mutex_ -> DatabaseManager::mutex_
 *
 *   本文件只按这一方向加锁，绝不反向持有 db 锁再去取 correlator 锁。
 *
 * 时间纪律：关联窗口与静默期全部以**事件时间**（WirelessDeviceEvent::timestamp_ms）
 * 为准。关联器不在内部读墙钟——读时钟会把判定结果绑到调用节奏上，
 * 也让单元测试无法确定性地构造时间线。
 */

#include "site_incident_correlator.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

#include "database_manager.hpp"
#include "logger.hpp"

namespace weaknet_dbus {

namespace {

/// incident_id 里内嵌 site_id 时，把非字母数字字符换成 '_'（ID 要可读、可 grep）
std::string sanitizeIdPart(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    }
    return out.empty() ? std::string("unknown") : out;
}

}  // namespace

// ============================================================================
// 构造 / 诊断
// ============================================================================

SiteIncidentCorrelator::SiteIncidentCorrelator(DatabaseManager* db, SiteIncidentConfig cfg)
    : db_(db), cfg_(std::move(cfg)) {}

uint64_t SiteIncidentCorrelator::totalIncidents() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_incidents_;
}

std::vector<SiteIncident> SiteIncidentCorrelator::activeIncidents() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SiteIncident> out;
    out.reserve(active_.size());
    for (const auto& kv : active_) {
        SiteIncident inc = kv.second.incident;
        inc.affected_devices = kv.second.deviceCount();
        inc.affected_device_ids.clear();
        for (const auto& ev : kv.second.event_device) {
            inc.affected_device_ids.push_back(ev.second);
        }
        std::sort(inc.affected_device_ids.begin(), inc.affected_device_ids.end());
        inc.affected_device_ids.erase(
            std::unique(inc.affected_device_ids.begin(), inc.affected_device_ids.end()),
            inc.affected_device_ids.end());
        out.push_back(std::move(inc));
    }
    return out;
}

size_t SiteIncidentCorrelator::ActiveIncident::deviceCount() const {
    // 受影响设备数 = 去重后的 device_key 个数。
    // 同一设备的多条事件（如先劣化再断连）只算一台，绝不重复计数。
    std::vector<std::string> keys;
    keys.reserve(event_device.size());
    for (const auto& kv : event_device) keys.push_back(kv.second);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys.size();
}

// ============================================================================
// 身份与分母
// ============================================================================

std::string SiteIncidentCorrelator::deviceKeyOf(const WirelessDeviceEvent& event) {
    // 复用既有复合键的字符串形式（含 protocol 与 address_type）：
    // 同一 MAC 的 BR/EDR 与 LE Random 是两条不同身份，绝不能算成一台设备。
    WirelessDeviceKey key;
    key.site_id = event.site_id;
    key.gateway_id = event.gateway_id;
    key.hci_index = event.hci_index;
    key.protocol = event.protocol;
    key.address_type = event.address_type;
    key.device_address = event.device_address;
    return key.toString();
}

std::string SiteIncidentCorrelator::makeIncidentId(const std::string& site_id,
                                                   uint64_t started_at_ms) const {
    // incident_id 是**确定性**的：只由 (site_id, started_at_ms) 决定，
    // 刻意**不带**进程实例随机码（对比 device_events.event_id 的做法）。
    //
    // 原因：重启回放会重新推导出同一份 incident，必须写回同一行（UPSERT）
    // 而不是插入新行——否则"重启一次多一起事故"。确定性 ID 让回放天然幂等。
    //
    // 同一 site 内 started_at_ms 不会重复：新 incident 只能由一条新的合格异常
    // 开启（窗口缓冲在产出时清空），其时间戳严格晚于上一起的
    // last_event_ms + quiet_window_ms >= 上一轮的 started_at_ms。
    std::ostringstream oss;
    oss << cfg_.incident_id_prefix << "_" << sanitizeIdPart(site_id)
        << "_" << started_at_ms;
    return oss.str();
}

size_t SiteIncidentCorrelator::activeDeviceCount(uint64_t now_ms) const {
    // 动态分母：只统计"活跃设备记忆"里仍然新鲜的设备。
    // 绝不使用数据库中的设备总数——一台三个月前离线、再未出现的设备会把比率
    // 永久稀释到永不达标（关联器静默失效，且没有任何报错）。
    const uint64_t floor_ms =
        now_ms > cfg_.active_device_memory_ms ? now_ms - cfg_.active_device_memory_ms : 0;
    size_t count = 0;
    for (const auto& kv : device_memory_) {
        if (kv.second >= floor_ms) ++count;
    }
    return count;
}

// ============================================================================
// 观察事件
// ============================================================================

std::optional<SiteIncident> SiteIncidentCorrelator::observe(const WirelessDeviceEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (event.timestamp_ms == 0) return std::nullopt;  // 无时间戳无法参与时空关联
    return observeLocked(event, event.timestamp_ms);
}

std::optional<SiteIncident> SiteIncidentCorrelator::observeLocked(
    const WirelessDeviceEvent& event, uint64_t now_ms) {
    // 1) 活跃设备记忆：**任何**事件（含非合格、含计划内断开）都说明该设备
    //    此刻在现场活跃。分母回答"这片区域此刻有几台设备在被观测"。
    device_memory_[deviceKeyOf(event)] = now_ms;

    // 2) 先用本事件的时间推进静默期/过期清理（结案早于判定，
    //    避免已结案的事故继续吸收证据）。结案结果由 tick() 收集，
    //    这里只做状态推进，不把它混进本方法的返回值。
    (void)tickLocked(now_ms);

    // 3) 合格性判定：非合格异常到此结束（它已贡献了分母，但不构成事故证据）
    if (!cfg_.policy.qualify(event)) {
        return std::nullopt;
    }

    const std::string site_key =
        event.site_id.empty() ? cfg_.site_id : event.site_id;
    auto it = active_.find(site_key);

    // 4) 已有活跃 incident：窗口内继续吸收，绝不新开
    if (it != active_.end()) {
        ActiveIncident& active = it->second;

        // 幂等：同一 event_id 只计一次。否则重复投递会把受影响设备数灌水到
        // 门槛之上，凭空造出一起"区域事故"。仅刷新活跃时间不改变语义。
        if (active.event_device.count(event.event_id) != 0) {
            return std::nullopt;
        }

        if (now_ms <= active.incident.last_event_ms + cfg_.correlation_window_ms) {
            const bool was_open = active.incident.state == IncidentState::Open;
            mergeInto(active, event);
            if (was_open) active.incident.state = IncidentState::Ongoing;
            persistLocked(active);
            return active.incident;
        }

        // 超出关联窗口且静默期未满（quiet_window > correlation_window 时才可能）：
        // 本事件不是同一事故的延续，先结案再走下面的开新流程。
        (void)resolveLocked(active);
        active_.erase(it);
    }

    // 5) 无活跃 incident：累计本次合格异常后重新评估双门槛
    SiteIncident opened;
    if (!tryOpenLocked(event, now_ms, opened)) {
        return std::nullopt;
    }
    return opened;
}

// ============================================================================
// 静默期推进
// ============================================================================

std::vector<SiteIncident> SiteIncidentCorrelator::tick(uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    return tickLocked(now_ms);
}

std::vector<SiteIncident> SiteIncidentCorrelator::tickLocked(uint64_t now_ms) {
    // 过期清理：关联窗口外的合格异常样本不再参与门槛判定
    const uint64_t qualify_floor =
        now_ms > cfg_.correlation_window_ms ? now_ms - cfg_.correlation_window_ms : 0;
    while (!recent_qualifying_.empty() && recent_qualifying_.front().ts_ms < qualify_floor) {
        recent_qualifying_.pop_front();
    }
    // 活跃设备记忆清理（分母只认新鲜设备）
    const uint64_t memory_floor =
        now_ms > cfg_.active_device_memory_ms ? now_ms - cfg_.active_device_memory_ms : 0;
    for (auto it = device_memory_.begin(); it != device_memory_.end();) {
        it = (it->second < memory_floor) ? device_memory_.erase(it) : std::next(it);
    }

    std::vector<SiteIncident> resolved;
    for (auto it = active_.begin(); it != active_.end();) {
        // 静默期以事件时间为准：窗口内再无新合格异常 = 这片区域已平静下来。
        if (now_ms > it->second.incident.last_event_ms + cfg_.quiet_window_ms) {
            resolved.push_back(resolveLocked(it->second));
            it = active_.erase(it);
        } else {
            ++it;
        }
    }
    return resolved;
}

// ============================================================================
// 开 / 并入 / 结案
// ============================================================================

bool SiteIncidentCorrelator::tryOpenLocked(const WirelessDeviceEvent& event,
                                           uint64_t now_ms,
                                           SiteIncident& out) {
    // 本条合格异常先进入窗口缓冲，再统计窗口内的受影响设备集合。
    recent_qualifying_.push_back(
        QualifyingSample{now_ms, event.event_id, deviceKeyOf(event)});

    std::map<std::string, std::string> event_device;  // event_id -> device_key
    uint64_t earliest_ms = now_ms;
    for (const auto& sample : recent_qualifying_) {
        event_device.emplace(sample.event_id, sample.device_key);
        earliest_ms = std::min(earliest_ms, sample.ts_ms);
    }

    const size_t denominator = activeDeviceCount(now_ms);
    if (denominator == 0) return false;

    std::vector<std::string> keys;
    keys.reserve(event_device.size());
    for (const auto& kv : event_device) keys.push_back(kv.second);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

    const double ratio =
        static_cast<double>(keys.size()) / static_cast<double>(denominator);

    // 双门槛：比例 ∧ 绝对数。单设备异常永远开不出事故——这是
    // "一台设备坏了"与"这一片无线环境出了问题"的分界线。
    if (keys.size() < cfg_.min_affected_devices) return false;
    if (ratio < cfg_.min_affected_ratio) return false;

    const std::string site_id = event.site_id.empty() ? cfg_.site_id : event.site_id;

    ActiveIncident active;
    SiteIncident& inc = active.incident;
    inc.incident_id = makeIncidentId(site_id, earliest_ms);
    inc.site_id = site_id;
    inc.gateway_id = event.gateway_id;
    inc.started_at_ms = earliest_ms;   // 现实事故从最早那条异常就开始
    inc.last_event_ms = now_ms;
    inc.resolved_at_ms = std::nullopt;
    inc.affected_devices = keys.size();
    inc.state = IncidentState::Open;
    // 采集与关联层不做诊断：恒为 nullopt，由 Phase 4+ 的诊断器填充。
    inc.suspected_cause = std::nullopt;
    active.event_device = std::move(event_device);

    ++total_incidents_;
    persistLocked(active);

    // 窗口缓冲已被本次事故消费：后续事故必须重新累积证据，
    // 否则同一批旧样本会把下一轮事故立刻"扶着"开出来。
    recent_qualifying_.clear();

    out = inc;
    active_.emplace(site_id, std::move(active));
    return true;
}

void SiteIncidentCorrelator::mergeInto(ActiveIncident& active,
                                       const WirelessDeviceEvent& event) {
    active.event_device[event.event_id] = deviceKeyOf(event);

    // last_event_ms 只右移，绝不因迟到事件回退（回退会让静默期提前结束）
    if (event.timestamp_ms > active.incident.last_event_ms) {
        active.incident.last_event_ms = event.timestamp_ms;
    }
    // started_at_ms 取最早：事故从最早那条异常开始，而不是"达标那一刻"
    if (event.timestamp_ms < active.incident.started_at_ms) {
        active.incident.started_at_ms = event.timestamp_ms;
    }
    active.incident.affected_devices = active.deviceCount();
    active.incident.suspected_cause = std::nullopt;  // 关联层不产出推断
}

SiteIncident SiteIncidentCorrelator::resolveLocked(ActiveIncident& active) {
    // 结案时刻 = 最后一条证据 + 静默窗口：静默期满才敢说"事故结束"。
    // 用"当前时刻"会让恢复时长依赖于调用节奏，也让回放无法确定性重演。
    active.incident.state = IncidentState::Resolved;
    active.incident.resolved_at_ms =
        active.incident.last_event_ms + cfg_.quiet_window_ms;
    active.incident.affected_devices = active.deviceCount();

    persistLocked(active);

    // 本起事故的证据窗口已封存；后续事故必须重新累积。
    recent_qualifying_.clear();
    return active.incident;
}

void SiteIncidentCorrelator::persistLocked(const ActiveIncident& active) {
    if (!db_) return;

    const SiteIncident& inc = active.incident;
    const bool ok = db_->upsertSiteIncident(
        inc.incident_id,
        inc.site_id,
        inc.gateway_id,
        static_cast<int64_t>(inc.started_at_ms),
        static_cast<int64_t>(inc.last_event_ms),
        inc.resolved_at_ms.has_value()
            ? std::optional<int64_t>(static_cast<int64_t>(*inc.resolved_at_ms))
            : std::nullopt,
        static_cast<int64_t>(active.deviceCount()),
        toString(inc.state),
        inc.suspected_cause);
    if (!ok) {
        LOG_WARNING(LogModule::BLUETOOTH,
                    "SiteIncidentCorrelator: failed to persist incident "
                        << inc.incident_id);
    }

    // 事件回链：每条 SiteIncident 都必须能追溯到构成它的 DeviceEvent
    // （可追溯性是工业诊断的基本要求）。INSERT OR IGNORE 使回放/重试天然幂等。
    for (const auto& kv : active.event_device) {
        const bool linked = db_->insertSiteIncidentEvent(inc.incident_id, kv.first);
        if (!linked) {
            LOG_WARNING(LogModule::BLUETOOTH,
                        "SiteIncidentCorrelator: failed to link event " << kv.first
                            << " to incident " << inc.incident_id);
        }
    }
}

// ============================================================================
// 重启回放
// ============================================================================

size_t SiteIncidentCorrelator::recoverFromStore(uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!db_) return 0;

    // 回放窗口 = 关联窗口 + 静默窗口：只有落在这个区间内的事件才可能属于
    // "此刻仍然活跃"的 incident。回放的是**事件**而不是 incident 行——
    // 跨重启连续性由重新推导保证，从而不存在"内存态与库中行不一致"。
    const uint64_t span = cfg_.correlation_window_ms + cfg_.quiet_window_ms;
    const int64_t since_ms = now_ms > span ? static_cast<int64_t>(now_ms - span) : 0;

    // 结案下限：该现场最近一次结案之后的事件才属于新证据。没有这道闸门，
    // 回放会把已结案的 incident 重新拉回活跃态（RESOLVED 行被 UPSERT 回 OPEN），
    // 即"重启复活旧事故"。
    const int64_t resolved_floor = db_->queryLatestIncidentResolvedAt(cfg_.site_id);
    const int64_t from_ms = std::max(since_ms, resolved_floor);

    std::vector<WirelessDeviceEvent> events =
        db_->loadDeviceEventsForReplay(from_ms, static_cast<int64_t>(now_ms));

    // 先清空内存态再重放：回放是"重新推导"，不是"叠加"。否则残留的活跃
    // incident 会让重放出的事件被并入一个本不该继续存在的事故。
    active_.clear();
    recent_qualifying_.clear();
    device_memory_.clear();

    for (const auto& ev : events) {
        (void)observeLocked(ev, ev.timestamp_ms);
    }
    // 重放结束后以当前时刻推进一次：真正已经静默下来的事故在此结案，
    // 不会因为"回放时最后一条证据还没过期"就被误判为仍然活跃。
    (void)tickLocked(now_ms);

    LOG_INFO(LogModule::BLUETOOTH,
             "SiteIncidentCorrelator: replayed " << events.size()
                 << " device events since " << from_ms << "ms; active incidents="
                 << active_.size());
    return active_.size();
}

// ============================================================================
// 查询
// ============================================================================

std::string SiteIncidentCorrelator::queryPersisted(const std::string& state,
                                                   int64_t start_ms,
                                                   int64_t end_ms,
                                                   int limit,
                                                   bool include_devices) const {
    if (!db_) return "[]";
    // 刻意不持有本类 mutex_：DB 查询走 DatabaseManager 自己的锁，
    // 与 WirelessEventStore::queryPersisted 保持同一纪律（避免嵌套持锁）。
    return db_->querySiteIncidents(state, start_ms, end_ms, limit, include_devices);
}

}  // namespace weaknet_dbus

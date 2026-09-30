/**
 * @file bt_link_quality_tracker.cpp
 * @brief 蓝牙链路质量跟踪器实现
 */

#include "bt_link_quality_tracker.hpp"

#include <algorithm>
#include <sstream>

#include "logger.hpp"
#include "utils/json_escape.hpp"

namespace weaknet_dbus {

BtLinkQualityTracker::BtLinkQualityTracker(BtLinkQualityConfig cfg)
    : cfg_(std::move(cfg)) {}

void BtLinkQualityTracker::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    devices_.clear();
    event_seq_ = 0;
}

int16_t BtLinkQualityTracker::calculateMedian(std::vector<int16_t> values) {
    if (values.empty()) return -1000;
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    if (n % 2 == 1) {
        return values[n / 2];
    }
    // 偶数个取中间偏保守（偏小）的值，或者均值
    return static_cast<int16_t>((static_cast<int32_t>(values[(n / 2) - 1]) +
                                 static_cast<int32_t>(values[n / 2])) / 2);
}

std::optional<int16_t> BtLinkQualityTracker::DeviceState::currentReferenceBaseline(
    size_t min_samples) const {
    if (baseline_window.size() >= min_samples) {
        std::vector<int16_t> vals(baseline_window.begin(), baseline_window.end());
        return BtLinkQualityTracker::calculateMedian(vals);
    }
    // 若滑动窗口样本数未满，但有持久化预热基线，作为 warm-start 参考
    if (warm_start_baseline_dbm.has_value()) {
        return warm_start_baseline_dbm;
    }
    return std::nullopt;
}

void BtLinkQualityTracker::registerDeviceSnapshot(
    const WirelessDeviceKey& key,
    uint64_t wall_ms,
    uint64_t mono_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& dev = devices_[key];
    dev.key = key;
    if (dev.first_seen_ms == 0) dev.first_seen_ms = wall_ms;
    dev.last_seen_ms = wall_ms;
    dev.last_seen_mono_ns = mono_ns;
}

std::optional<WirelessDeviceEvent> BtLinkQualityTracker::feedFreshRssi(
    const WirelessDeviceKey& key,
    int16_t rssi_dbm,
    uint64_t wall_ms,
    uint64_t mono_ns) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 过滤无效 RSSI
    if (rssi_dbm <= -1000 || rssi_dbm == 0) {
        return std::nullopt;
    }

    auto& dev = devices_[key];
    dev.key = key;
    if (dev.first_seen_ms == 0) dev.first_seen_ms = wall_ms;

    // 1. 长期离线淘汰与 Relearn Policy 检查
    if (dev.last_seen_ms > 0 &&
        (wall_ms > dev.last_seen_ms) &&
        (wall_ms - dev.last_seen_ms > cfg_.baseline_stale_after_ms)) {
        LOG_INFO(LogModule::BLUETOOTH,
                 "BtLinkQualityTracker: device " << key.device_address
                 << " idle for " << (wall_ms - dev.last_seen_ms) / 1000
                 << "s > stale threshold; resetting to LEARNING");
        dev.state = LinkQualityState::Learning;
        dev.baseline_window.clear();
        dev.warm_start_baseline_dbm = std::nullopt;
        dev.bad_streak = 0;
        dev.good_streak = 0;
    }

    dev.last_seen_ms = wall_ms;
    dev.last_seen_mono_ns = mono_ns;

    // 更新全局极值观测
    if (!dev.min_seen_rssi_dbm.has_value() || rssi_dbm < *dev.min_seen_rssi_dbm) {
        dev.min_seen_rssi_dbm = rssi_dbm;
    }
    if (!dev.max_seen_rssi_dbm.has_value() || rssi_dbm > *dev.max_seen_rssi_dbm) {
        dev.max_seen_rssi_dbm = rssi_dbm;
    }

    // 塞入时序样本环（用于后续断连回填）
    RssiSample sample;
    sample.rssi_dbm = rssi_dbm;
    sample.observed_at_ms = wall_ms;
    sample.monotonic_ns = mono_ns;
    sample.from_signal = true;
    dev.recent_samples.push_back(sample);
    if (dev.recent_samples.size() > DeviceState::kMaxSampleHistory) {
        dev.recent_samples.pop_front();
    }

    // 2. 获取用于本次判定的 reference baseline
    const auto ref_baseline = dev.currentReferenceBaseline(cfg_.min_baseline_samples);

    // 若基线尚未就绪（仍在 LEARNING 且无预热基线），仅累积稳定基线样本，不发告警
    if (!ref_baseline.has_value()) {
        dev.state = LinkQualityState::Learning;
        dev.baseline_window.push_back(rssi_dbm);
        if (dev.baseline_window.size() > 30) dev.baseline_window.pop_front();
        if (dev.baseline_window.size() >= cfg_.min_baseline_samples) {
            dev.state = LinkQualityState::Stable;
        }
        return std::nullopt;
    }

    const int16_t baseline = *ref_baseline;
    std::optional<WirelessDeviceEvent> event_out;

    // 3. 状态判定与时间连续性检查
    const uint64_t max_gap_ns = cfg_.max_fresh_gap_ms * 1000000ULL;

    if (dev.state == LinkQualityState::Stable || dev.state == LinkQualityState::Learning) {
        // 当前基线已可用，正在正常监控。检查是否属于恶化样本候选
        if (rssi_dbm < (baseline - cfg_.degrade_delta_db)) {
            // 检查与上一次 bad 样本的时间连续性
            if (dev.bad_streak > 0 &&
                (mono_ns > dev.last_bad_sample_mono_ns) &&
                (mono_ns - dev.last_bad_sample_mono_ns > max_gap_ns)) {
                // 采样稀疏超期，重新计数连续坏样本
                dev.bad_streak = 1;
            } else {
                ++dev.bad_streak;
            }
            dev.last_bad_sample_mono_ns = mono_ns;
            dev.good_streak = 0; // 中断好样本计数

            // 规则：只要落入 bad candidate，就不学进 reference window！
            if (dev.bad_streak >= cfg_.degrade_streak) {
                dev.state = LinkQualityState::Degraded;
                dev.bad_streak = 0;

                // 产出 LINK_DEGRADED 边沿事件
                WirelessDeviceEvent ev;
                std::ostringstream id_ss;
                id_ss << "btev_deg_" << ++event_seq_;
                ev.event_id = id_ss.str();
                ev.site_id = key.site_id;
                ev.gateway_id = key.gateway_id;
                ev.hci_index = key.hci_index;
                ev.protocol = key.protocol;
                ev.device_address = key.device_address;
                ev.address_type = key.address_type;
                ev.event_type = DeviceEventType::LinkDegraded;
                ev.timestamp_ms = wall_ms;
                ev.monotonic_ns = mono_ns;
                ev.rssi_at_event_dbm = rssi_dbm;
                ev.source = EvidenceSource::Derived;
                ev.source_detail = "bt_link_quality_tracker";
                ev.suspected_cause = std::nullopt; // 采集层不做诊断

                std::ostringstream det;
                det << "{"
                    << "\"baseline_rssi_dbm\":" << baseline << ","
                    << "\"trigger_rssi_dbm\":" << rssi_dbm << ","
                    << "\"delta_db\":" << (rssi_dbm - baseline) << ","
                    << "\"consecutive_samples\":" << cfg_.degrade_streak << ","
                    << "\"state\":\"DEGRADED\","
                    << "\"algorithm_version\":" << cfg_.algorithm_version
                    << "}";
                ev.details_json = det.str();
                event_out = ev;
            }
        } else {
            // 正常样本：清零坏样本 streak，并学入基线窗口
            dev.bad_streak = 0;
            dev.state = LinkQualityState::Stable;
            dev.baseline_window.push_back(rssi_dbm);
            if (dev.baseline_window.size() > 30) {
                dev.baseline_window.pop_front();
            }
            // 若新窗口累积满，旧的 warm-start 基线彻底完成使命注销
            if (dev.baseline_window.size() >= cfg_.min_baseline_samples) {
                dev.warm_start_baseline_dbm = std::nullopt;
            }
        }
    } else if (dev.state == LinkQualityState::Degraded) {
        // 处于 DEGRADED 期间：基线彻底冻结，任何样本严禁塞入 baseline_window！
        // 检查是否满足恢复条件（回升到基线 - 8dBm 以内）
        if (rssi_dbm > (baseline - cfg_.recover_delta_db)) {
            if (dev.good_streak > 0 &&
                (mono_ns > dev.last_good_sample_mono_ns) &&
                (mono_ns - dev.last_good_sample_mono_ns > max_gap_ns)) {
                dev.good_streak = 1;
            } else {
                ++dev.good_streak;
            }
            dev.last_good_sample_mono_ns = mono_ns;

            if (dev.good_streak >= cfg_.recover_streak) {
                dev.state = LinkQualityState::Stable;
                dev.good_streak = 0;
                dev.bad_streak = 0;

                // 恢复时将此正常样本塞入基线窗口
                dev.baseline_window.push_back(rssi_dbm);
                if (dev.baseline_window.size() > 30) dev.baseline_window.pop_front();

                // 产出 LINK_RECOVERED 边沿事件
                WirelessDeviceEvent ev;
                std::ostringstream id_ss;
                id_ss << "btev_rec_" << ++event_seq_;
                ev.event_id = id_ss.str();
                ev.site_id = key.site_id;
                ev.gateway_id = key.gateway_id;
                ev.hci_index = key.hci_index;
                ev.protocol = key.protocol;
                ev.device_address = key.device_address;
                ev.address_type = key.address_type;
                ev.event_type = DeviceEventType::LinkRecovered;
                ev.timestamp_ms = wall_ms;
                ev.monotonic_ns = mono_ns;
                ev.rssi_at_event_dbm = rssi_dbm;
                ev.source = EvidenceSource::Derived;
                ev.source_detail = "bt_link_quality_tracker";
                ev.suspected_cause = std::nullopt;

                std::ostringstream det;
                det << "{"
                    << "\"baseline_rssi_dbm\":" << baseline << ","
                    << "\"recovery_rssi_dbm\":" << rssi_dbm << ","
                    << "\"consecutive_samples\":" << cfg_.recover_streak << ","
                    << "\"state\":\"STABLE\","
                    << "\"algorithm_version\":" << cfg_.algorithm_version
                    << "}";
                ev.details_json = det.str();
                event_out = ev;
            }
        } else {
            // 仍在恶化区间，清零恢复计数
            dev.good_streak = 0;
        }
    }

    return event_out;
}

std::optional<int16_t> BtLinkQualityTracker::getRssiBeforeEvent(
    const WirelessDeviceKey& key,
    uint64_t event_monotonic_ns,
    uint64_t ttl_ns) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(key);
    if (it == devices_.end()) {
        return std::nullopt;
    }

    const auto& samples = it->second.recent_samples;
    // 从后向前倒序检索满足严格因果条件的最近有效样本
    for (auto rit = samples.rbegin(); rit != samples.rend(); ++rit) {
        // 严格因果条件 1：不得取断连时刻之后的样本！
        if (rit->monotonic_ns <= event_monotonic_ns) {
            // 严格因果条件 2：断连前时间差必须在 TTL 之内
            if (event_monotonic_ns - rit->monotonic_ns <= ttl_ns) {
                return rit->rssi_dbm;
            }
            // 超过 TTL 则直接判定失效，由于时间正序，更早的历史也必超时
            break;
        }
    }

    return std::nullopt;
}

void BtLinkQualityTracker::loadWarmStartBaseline(const DeviceLinkProfile& profile) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& dev = devices_[profile.key];
    dev.key = profile.key;
    dev.warm_start_baseline_dbm = profile.baseline_rssi_dbm;
    dev.min_seen_rssi_dbm = profile.min_seen_rssi_dbm;
    dev.max_seen_rssi_dbm = profile.max_seen_rssi_dbm;
    dev.first_seen_ms = profile.first_seen_ms;
    dev.last_seen_ms = profile.last_seen_ms;
    dev.state = profile.state;
    // 不伪造 30 个点，只承载 warm_start_baseline_dbm
    dev.baseline_window.clear();
    dev.bad_streak = 0;
    dev.good_streak = 0;
}

std::optional<DeviceLinkProfile> BtLinkQualityTracker::getProfile(
    const WirelessDeviceKey& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = devices_.find(key);
    if (it == devices_.end()) return std::nullopt;

    const auto& dev = it->second;
    DeviceLinkProfile p;
    p.key = dev.key;
    p.baseline_rssi_dbm = dev.currentReferenceBaseline(cfg_.min_baseline_samples);
    p.min_seen_rssi_dbm = dev.min_seen_rssi_dbm;
    p.max_seen_rssi_dbm = dev.max_seen_rssi_dbm;
    p.baseline_sample_count = dev.baseline_window.size();
    p.first_seen_ms = dev.first_seen_ms;
    p.last_seen_ms = dev.last_seen_ms;
    p.state = dev.state;
    p.updated_at_ms = dev.last_seen_ms;
    return p;
}

std::vector<DeviceLinkProfile> BtLinkQualityTracker::getAllProfiles() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DeviceLinkProfile> out;
    out.reserve(devices_.size());
    for (const auto& [k, dev] : devices_) {
        DeviceLinkProfile p;
        p.key = dev.key;
        p.baseline_rssi_dbm = dev.currentReferenceBaseline(cfg_.min_baseline_samples);
        p.min_seen_rssi_dbm = dev.min_seen_rssi_dbm;
        p.max_seen_rssi_dbm = dev.max_seen_rssi_dbm;
        p.baseline_sample_count = dev.baseline_window.size();
        p.first_seen_ms = dev.first_seen_ms;
        p.last_seen_ms = dev.last_seen_ms;
        p.state = dev.state;
        p.updated_at_ms = dev.last_seen_ms;
        out.push_back(p);
    }
    return out;
}

}  // namespace weaknet_dbus

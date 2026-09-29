/**
 * @file bt_event_normalizer.cpp
 * @brief 蓝牙原始观测归一化器实现
 *
 * 核心不变式（修改本文件前必须读懂）：
 *   - 每个 kprobe 只产生 RawBtObservation，绝不产生 WirelessDeviceEvent
 *   - 合并键含 gateway_id / hci_index / address_type，同 MAC 跨控制器不合并
 *   - 窗口外不合并：两次真实断连必须产生两个 canonical 事件
 *   - 原因定案遵循优先级（mgmt raw > 其余 raw > supplemental hint > Unknown），
 *     但所有观测一律进入 raw_evidence，优先级不覆盖原始事实
 *   - reason 的归一化按**来源域**选择映射：mgmt 来源用 MGMT_DEV_DISCONN_* 枚举，
 *     其余来源用 HCI error code；两域取值空间不同不可互套
 *   - 不产出 suspected_cause（那是诊断器的职责）
 */

#include "bt_event_normalizer.hpp"

#include <algorithm>
#include <iomanip>
#include <random>
#include <sstream>
#include <tuple>

namespace weaknet_dbus {

// ============================================================================
// MergeKey
// ============================================================================

bool BtEventNormalizer::MergeKey::operator<(const MergeKey& other) const {
    // 逐字段字典序比较，等价于对五元组做全序排序。
    // 用 std::tie 而非手写嵌套 if，避免遗漏字段导致"看起来不同的事件被合并"。
    return std::tie(gateway_id, hci_index, address_type, device_address, event_type) <
           std::tie(other.gateway_id, other.hci_index, other.address_type,
                    other.device_address, other.event_type);
}

// ============================================================================
// 构造 / 诊断
// ============================================================================

BtEventNormalizer::BtEventNormalizer(BtNormalizerConfig cfg) : cfg_(std::move(cfg)) {
    // 每实例一个随机短码（8 个 hex 字符）。不同进程/不同重启之间碰撞概率
    // 极低，配合 seq 即可保证 event_id 跨重启唯一（见头文件成员注释）。
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;
    std::ostringstream nonce;
    nonce << std::hex << std::setfill('0') << std::setw(8)
          << (dis(gen) & 0xFFFFFFFFULL);
    instance_nonce_ = nonce.str();
}

size_t BtEventNormalizer::pendingCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return pending_.size();
}

uint64_t BtEventNormalizer::normalizedCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return normalized_total_;
}

void BtEventNormalizer::reset() {
    std::lock_guard<std::mutex> lock(mu_);
    pending_.clear();
    seq_ = 0;
    normalized_total_ = 0;
}

// ============================================================================
// 投递观测
// ============================================================================

BtEventNormalizer::MergeKey BtEventNormalizer::makeKey(const RawBtObservation& obs) {
    MergeKey key;
    key.gateway_id = obs.gateway_id;
    key.hci_index = obs.hci_index;
    key.address_type = obs.address_type;
    key.device_address = obs.device_address;
    key.event_type = obs.event_type;
    return key;
}

void BtEventNormalizer::submit(const RawBtObservation& obs) {
    std::lock_guard<std::mutex> lock(mu_);

    const MergeKey key = makeKey(obs);
    auto it = pending_.find(key);

    // 已存在 pending 且仍在合并窗口内 -> 归入同一 canonical 事件
    if (it != pending_.end() &&
        obs.timestamp_ms <= it->second.last_obs_ms + cfg_.merge_window_ms) {
        applyObservation(it->second, obs);
        return;
    }

    // 窗口外（或首次）-> 定案旧事件，另开新的
    //
    // 注意：这里刻意不把旧事件立即返回给调用方，而是留在 pending_ 里等
    // flushExpired() 统一取走——submit 的语义是"吸收观测"，不是"产出事件"。
    // 但旧事件若非空，必须先落定，否则同键的两条记录会互相覆盖。
    if (it != pending_.end()) {
        // 旧事件已过窗口：标记为可刷出。这里把它移到 completed_ 队列。
        completed_.push_back(finalize(it->second, seq_++));
        pending_.erase(it);
    }

    // 建立新 pending
    PendingEvent pending;
    pending.event.site_id = obs.gateway_id.empty() ? "" : "";  // site 由 Store 层补齐
    pending.event.gateway_id = obs.gateway_id;
    pending.event.hci_index = obs.hci_index;
    pending.event.protocol = WirelessProtocol::Bluetooth;
    pending.event.device_address = obs.device_address;
    pending.event.address_type = obs.address_type;
    pending.event.event_type = obs.event_type;
    pending.event.timestamp_ms = obs.timestamp_ms;
    applyObservation(pending, obs);

    // 容量保护：超过上限时强制定案最老的一条，防止内存无界增长
    if (pending_.size() >= cfg_.max_pending) {
        auto oldest = pending_.begin();
        for (auto i = pending_.begin(); i != pending_.end(); ++i) {
            if (i->second.last_obs_ms < oldest->second.last_obs_ms) oldest = i;
        }
        completed_.push_back(finalize(oldest->second, seq_++));
        pending_.erase(oldest);
    }

    pending_.emplace(makeKey(obs), std::move(pending));
}

// ============================================================================
// 原因定案优先级
// ============================================================================

int BtEventNormalizer::reasonRank(const RawBtObservation& obs) {
    // mgmt 层的原始码（MGMT_DEV_DISCONN_* 域）—— 最权威的事实来源
    if (obs.raw_reason_code.has_value() && obs.source == EvidenceSource::KernelMgmt) {
        return 3;
    }
    // 其余来源携带的原始码（HCI error 域，如 hci_disconnect 的 reason）
    // 原始码是事实，必须排在推断 hint 之上——否则先到的 raw 会被后到的 hint 覆盖
    if (obs.raw_reason_code.has_value()) {
        return 2;
    }
    // supplemental hook 带来归一化原因（如 hci_conn_timeout -> ConnectionTimeout）
    if (obs.reason_hint != DisconnectReason::Unknown) {
        return 1;
    }
    return 0;
}

// ============================================================================
// 观测 -> pending 事件
// ============================================================================

void BtEventNormalizer::applyObservation(PendingEvent& pending, const RawBtObservation& obs) {
    // 事件时间为窗口中**最早**一条观测的时刻：现实故障发生在最早观测到的那一刻，
    // 而不是最后一条证据到达的时刻。
    if (pending.evidence.empty() || obs.timestamp_ms < pending.event.timestamp_ms) {
        pending.event.timestamp_ms = obs.timestamp_ms;
    }
    if (obs.timestamp_ms > pending.last_obs_ms) {
        pending.last_obs_ms = obs.timestamp_ms;
    }

    // RSSI：只取第一个有值的观测。断连瞬间的链路质量比最后一条证据更有诊断价值。
    if (!pending.event.rssi_at_event_dbm.has_value() && obs.rssi_dbm.has_value()) {
        pending.event.rssi_at_event_dbm = obs.rssi_dbm;
    }

    // 原因定案：仅当本条观测的优先级严格高于当前定案时才覆盖。
    // 这保证 mgmt 的原始 reason 不会被后到的 supplemental hint 冲掉。
    const int rank = reasonRank(obs);
    if (rank > pending.reason_rank) {
        pending.reason_rank = rank;
        pending.event.source = obs.source;
        pending.event.source_detail = obs.source_detail;
        if (obs.raw_reason_code.has_value()) {
            // 原始 code 无损保存，归一化解释由**按来源域**的映射函数给出：
            //   KernelMgmt -> MGMT_DEV_DISCONN_*（内核 mgmt 枚举，非 HCI error）
            //   其余       -> HCI error code
            // 两个域的取值空间完全不同（如 0x02 在 mgmt 域=本地断开，
            // 在 HCI 域=Unknown Connection Identifier），混用会得出错误原因。
            pending.event.raw_reason_code = *obs.raw_reason_code;
            pending.event.reason =
                (obs.source == EvidenceSource::KernelMgmt)
                    ? disconnectReasonFromMgmtCode(*obs.raw_reason_code)
                    : disconnectReasonFromHciCode(*obs.raw_reason_code);
        } else if (obs.reason_hint != DisconnectReason::Unknown) {
            // 该来源没有原始 code（如 hci_conn_timeout），只用它的归一化结论
            pending.event.reason = obs.reason_hint;
        }
    }

    // 无条件追加原始证据：优先级只影响 canonical 字段，不影响证据留存
    pending.evidence.push_back(obs);
}

// ============================================================================
// 定案
// ============================================================================

WirelessDeviceEvent BtEventNormalizer::finalize(PendingEvent& pending, uint64_t seq) {
    WirelessDeviceEvent ev = pending.event;

    // event_id = <prefix>_<实例随机码>_<seq>：进程内单调、跨进程（服务重启）不碰撞。
    // device_events.event_id 是 UNIQUE 且 INSERT OR IGNORE——跨重启重复即静默丢事件，
    // 所以唯一性必须在生成方保证（早期版本只有 prefix+seq，重启后必撞，已修）。
    std::ostringstream id;
    id << cfg_.event_id_prefix << "_" << instance_nonce_ << "_" << seq;
    ev.event_id = id.str();

    // 拼接 raw_evidence 数组
    std::string details;
    for (const auto& obs : pending.evidence) {
        details = appendRawEvidence(details, obs);
    }
    ev.details_json = details;

    // 采集层不做诊断：suspected_cause 恒为空，
    // 由 Phase 2/3 的 LinkAnomalyDetector / SiteIncidentCorrelator 填充。
    ev.suspected_cause = std::nullopt;

    ++normalized_total_;
    return ev;
}

// ============================================================================
// 刷出
// ============================================================================

std::vector<WirelessDeviceEvent> BtEventNormalizer::flushExpired(uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mu_);

    std::vector<WirelessDeviceEvent> out = std::move(completed_);
    completed_.clear();

    for (auto it = pending_.begin(); it != pending_.end();) {
        if (now_ms > it->second.last_obs_ms + cfg_.merge_window_ms) {
            out.push_back(finalize(it->second, seq_++));
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

std::vector<WirelessDeviceEvent> BtEventNormalizer::flushAll() {
    std::lock_guard<std::mutex> lock(mu_);

    std::vector<WirelessDeviceEvent> out = std::move(completed_);
    completed_.clear();

    for (auto& kv : pending_) {
        out.push_back(finalize(kv.second, seq_++));
    }
    pending_.clear();
    return out;
}

}  // namespace weaknet_dbus

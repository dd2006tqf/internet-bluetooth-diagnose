#include "assurance/dns_transaction_tracker.hpp"
#include "logger.hpp"
#include <algorithm>

namespace weaknet {

DnsTransactionTracker::DnsTransactionTracker(Config cfg)
    : cfg_(cfg) {}

void DnsTransactionTracker::advanceBindingEpoch(uint64_t new_epoch) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (new_epoch <= current_binding_epoch_) {
        return;
    }
    current_binding_epoch_ = new_epoch;
    revision_++;
    // SR-4: 路由/Resolver 变化推进 binding_epoch，旧路径在途事务直接失效，避免污染新网络
    active_txs_.clear();
    tombstones_.clear();
}

uint64_t DnsTransactionTracker::currentBindingEpoch() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_binding_epoch_;
}

void DnsTransactionTracker::recordDeliveryLoss(uint64_t lost_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    event_delivery_loss_ += lost_count;
    total_captured_events_ += lost_count;
    revision_++;
}

bool DnsTransactionTracker::onQueryCaptured(const DnsCanonicalKey& key,
                                            FingerprintQuality quality,
                                            std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    query_capture_attempts_++;
    total_captured_events_++;

    auto it = active_txs_.find(key);
    if (it != active_txs_.end()) {
        // IR-2: 重传去重 —— 仅更新 last_sent_at 与 attempt_count，first_sent_at 保持不变
        it->second.last_sent_at = now;
        it->second.attempt_count++;
        revision_++;
        return true;
    }

    // IR-1: 容量溢出防御
    if (active_txs_.size() >= cfg_.capacity) {
        tracker_insert_failures_++;
        revision_++;
        return false;
    }

    // 检查墓碑：如果刚才已超时，但在墓碑期内又发起了相同 key（极罕见端口与TxID复用），清理旧墓碑
    tombstones_.erase(key);

    DnsTransactionRecord rec;
    rec.key = key;
    rec.fingerprint_quality = quality;
    rec.transaction_generation = ++generation_counter_;
    rec.binding_epoch = current_binding_epoch_;
    rec.first_sent_at = now;
    rec.last_sent_at = now;
    rec.attempt_count = 1;
    rec.state = DnsTransactionState::ACTIVE;

    active_txs_.emplace(key, std::move(rec));
    revision_++;
    return true;
}

void DnsTransactionTracker::onResponseCaptured(const DnsCanonicalKey& key,
                                               uint8_t rcode,
                                               bool tc,
                                               bool is_malformed,
                                               std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    response_match_attempts_++;
    total_captured_events_++;

    auto it = active_txs_.find(key);
    if (it == active_txs_.end()) {
        // PARTIAL capture events may omit one side of the socket tuple. Claim
        // only a unique same-TxID candidate inside the current epoch; multiple
        // candidates are explicit ambiguity, never a guessed match (SR-7/SR-8).
        auto candidate = active_txs_.end();
        size_t candidates = 0;
        for (auto scan = active_txs_.begin(); scan != active_txs_.end(); ++scan) {
            if (scan->second.key.txid != key.txid ||
                scan->second.binding_epoch != current_binding_epoch_) {
                continue;
            }
            // Reject candidates contradicted by any known endpoint field.
            if (key.resolver_ip != 0 && scan->second.key.resolver_ip != 0 &&
                key.resolver_ip != scan->second.key.resolver_ip) {
                continue;
            }
            if (key.client_ip != 0 && scan->second.key.client_ip != 0 &&
                key.client_ip != scan->second.key.client_ip) {
                continue;
            }
            if (key.client_port != 0 && scan->second.key.client_port != 0 &&
                key.client_port != scan->second.key.client_port) {
                continue;
            }
            ++candidates;
            candidate = scan;
        }
        if (candidates == 1) {
            it = candidate;
        } else if (candidates > 1) {
            tracking_ambiguous_count_++;
            revision_++;
            return;
        }
    }

    if (it != active_txs_.end()) {
        // 匹配成功，推进终态 (Atomic Claim)
        DnsTransactionRecord terminal = std::move(it->second);
        active_txs_.erase(it);

        terminal.completed_at = now;
        terminal.rcode = rcode;
        terminal.tc = tc;

        // 计算端到端延迟（从 first_sent_at 到 now）
        auto dur = std::chrono::duration_cast<std::chrono::microseconds>(now - terminal.first_sent_at).count();
        terminal.latency_ms = static_cast<double>(dur) / 1000.0;
        if (terminal.latency_ms < 0.0) terminal.latency_ms = 0.0;

        // SR-10, SR-14 & 修正 #4: 互斥分类优先级
        // 1. is_malformed -> RESPONSE_OTHER
        // 2. tc == true   -> RESPONSE_TRUNCATED
        // 3. 否则由 rcode 分类
        if (is_malformed) {
            terminal.state = DnsTransactionState::RESPONSE_OTHER;
        } else if (tc) {
            terminal.state = DnsTransactionState::RESPONSE_TRUNCATED;
        } else {
            switch (rcode) {
                case 0: terminal.state = DnsTransactionState::NOERROR; break;
                case 2: terminal.state = DnsTransactionState::SERVFAIL; break;
                case 3: terminal.state = DnsTransactionState::NXDOMAIN; break;
                case 5: terminal.state = DnsTransactionState::REFUSED; break;
                default: terminal.state = DnsTransactionState::RESPONSE_OTHER; break;
            }
        }

        // 如果指纹质量是 PARTIAL 且没有 qname_hash 校验，标记为可能存在歧义 (SR-8)
        if (terminal.fingerprint_quality == FingerprintQuality::PARTIAL) {
            tracking_ambiguous_count_++;
        }

        terminal_history_.push_back(terminal);
        revision_++;
        return;
    }

    // 检查是否落入 15s 墓碑 (IR-2: 区分迟到响应与未匹配响应)
    auto t_it = tombstones_.find(key);
    if (t_it != tombstones_.end()) {
        late_responses_count_++;
        DnsTransactionRecord late_rec;
        late_rec.key = key;
        late_rec.binding_epoch = t_it->second.binding_epoch;
        late_rec.completed_at = now;
        late_rec.state = DnsTransactionState::LATE_RESPONSE;
        late_rec.rcode = rcode;
        late_rec.tc = tc;
        terminal_history_.push_back(late_rec);
        revision_++;
        return;
    }

    // 无任何在途记录且无墓碑 -> UNMATCHED (SR-8)
    unmatched_count_++;
    revision_++;
}

size_t DnsTransactionTracker::sweepTimeouts(std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t timed_out_count = 0;

    // 1. 清理过期墓碑 (> 15s)
    for (auto it = tombstones_.begin(); it != tombstones_.end(); ) {
        if (now >= it->second.expired_at && (now - it->second.expired_at) > cfg_.tombstone_retention) {
            it = tombstones_.erase(it);
        } else {
            ++it;
        }
    }

    // 2. 扫描在途请求 (Atomic Claim: scan -> find -> claim -> terminalize)
    std::vector<DnsCanonicalKey> expired_keys;
    for (const auto& kv : active_txs_) {
        const auto& rec = kv.second;
        // 无论是否重传，以 first_sent_at 超过 query_timeout 裁决事务级超时 (SR-9)
        if (now >= rec.first_sent_at && (now - rec.first_sent_at) >= cfg_.query_timeout) {
            expired_keys.push_back(kv.first);
        }
    }

    for (const auto& key : expired_keys) {
        auto it = active_txs_.find(key);
        if (it == active_txs_.end()) continue;

        DnsTransactionRecord terminal = std::move(it->second);
        active_txs_.erase(it);

        terminal.completed_at = now;
        terminal.state = DnsTransactionState::TIMEOUT_EXPIRED;
        auto dur = std::chrono::duration_cast<std::chrono::microseconds>(now - terminal.first_sent_at).count();
        terminal.latency_ms = static_cast<double>(dur) / 1000.0;

        // 移入 15s 墓碑 (IR-2)
        TombstoneRecord tb;
        tb.key = key;
        tb.binding_epoch = terminal.binding_epoch;
        tb.expired_at = now;
        tb.original_state = DnsTransactionState::TIMEOUT_EXPIRED;
        tombstones_[key] = tb;

        terminal_history_.push_back(terminal);
        timed_out_count++;
    }

    // 3. 清理超出 120s 窗口的终态记录
    while (!terminal_history_.empty()) {
        const auto& oldest = terminal_history_.front();
        if (now >= oldest.completed_at && (now - oldest.completed_at) > cfg_.window_retention) {
            terminal_history_.pop_front();
        } else {
            break;
        }
    }

    if (timed_out_count > 0) {
        revision_++;
    }

    return timed_out_count;
}

DnsTrackerSnapshot DnsTransactionTracker::getSnapshot(std::chrono::steady_clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    DnsTrackerSnapshot snap;
    snap.binding_epoch = current_binding_epoch_;
    snap.revision = revision_;
    snap.cutoff = now;

    // SR-13 & 修正 #3: 仅统计当前 binding_epoch 下 ACTIVE 的事务
    size_t inflight = 0;
    for (const auto& kv : active_txs_) {
        if (kv.second.binding_epoch == current_binding_epoch_ && kv.second.state == DnsTransactionState::ACTIVE) {
            inflight++;
        }
    }
    snap.current_inflight = inflight;
    return snap;
}

DnsMetricWindow DnsTransactionTracker::getWindowMetrics(std::chrono::milliseconds window_duration,
                                                       std::chrono::steady_clock::time_point cutoff_time) const {
    std::lock_guard<std::mutex> lock(mutex_);
    DnsMetricWindow w;
    w.binding_epoch = current_binding_epoch_;
    w.evaluation_cutoff = cutoff_time;
    w.evidence_revision = revision_;

    // SR-13: current_inflight 由 authoritative Tracker 实时状态提供
    size_t inflight = 0;
    for (const auto& kv : active_txs_) {
        if (kv.second.binding_epoch == current_binding_epoch_ && kv.second.state == DnsTransactionState::ACTIVE) {
            inflight++;
        }
    }
    w.current_inflight = inflight;

    // 筛选位于 [cutoff_time - window_duration, cutoff_time] 内且处于同一 binding_epoch 的终态事务
    for (const auto& rec : terminal_history_) {
        if (rec.binding_epoch != current_binding_epoch_) {
            continue; // SR-4: 隔离非当前 epoch 证据
        }
        if (rec.completed_at > cutoff_time) {
            continue;
        }
        if (cutoff_time >= rec.completed_at && (cutoff_time - rec.completed_at) <= window_duration) {
            w.queries_started++; // 每次纳入窗口的终态事务对应一次发起
            switch (rec.state) {
                case DnsTransactionState::NOERROR:
                    w.responses_noerror++;
                    w.latencies_ms.push_back(rec.latency_ms);
                    break;
                case DnsTransactionState::NXDOMAIN:
                    w.responses_nxdomain++;
                    w.latencies_ms.push_back(rec.latency_ms);
                    break;
                case DnsTransactionState::SERVFAIL:
                    w.responses_servfail++;
                    break;
                case DnsTransactionState::REFUSED:
                    w.responses_refused++;
                    break;
                case DnsTransactionState::RESPONSE_TRUNCATED:
                    w.responses_truncated++;
                    break;
                case DnsTransactionState::RESPONSE_OTHER:
                    w.responses_other++;
                    break;
                case DnsTransactionState::TIMEOUT_EXPIRED:
                    w.timeouts++;
                    break;
                case DnsTransactionState::LATE_RESPONSE:
                    w.late_responses++;
                    break;
                default:
                    break;
            }
        }
    }

    w.unmatched = unmatched_count_;
    w.tracking_ambiguous = tracking_ambiguous_count_;
    w.tracker_insert_failures = tracker_insert_failures_;
    w.event_delivery_loss = event_delivery_loss_;
    w.response_match_attempts = response_match_attempts_;
    w.query_capture_attempts = query_capture_attempts_;
    w.total_captured_events = total_captured_events_;

    return w;
}

std::vector<DnsTransactionRecord> DnsTransactionTracker::getRecentTerminals(size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DnsTransactionRecord> res;
    if (terminal_history_.empty()) return res;

    size_t count = std::min(limit, terminal_history_.size());
    res.reserve(count);
    auto it = terminal_history_.rbegin();
    for (size_t i = 0; i < count && it != terminal_history_.rend(); ++i, ++it) {
        res.push_back(*it);
    }
    std::reverse(res.begin(), res.end()); // 调整为从老到新顺序
    return res;
}

void DnsTransactionTracker::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    active_txs_.clear();
    tombstones_.clear();
    terminal_history_.clear();
    unmatched_count_ = 0;
    tracking_ambiguous_count_ = 0;
    late_responses_count_ = 0;
    tracker_insert_failures_ = 0;
    event_delivery_loss_ = 0;
    response_match_attempts_ = 0;
    query_capture_attempts_ = 0;
    total_captured_events_ = 0;
    revision_++;
}

} // namespace weaknet

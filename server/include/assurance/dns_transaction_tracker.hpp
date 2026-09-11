#pragma once

#include "assurance/dns_types.hpp"
#include <unordered_map>
#include <deque>
#include <mutex>
#include <chrono>
#include <vector>
#include <memory>
#include <functional>

namespace weaknet {

struct DnsTrackerConfig {
    size_t capacity{2048};                                       // IR-1: Peak QPS * Timeout * Safety Factor
    std::chrono::milliseconds query_timeout{5000};              // 5s 超时
    std::chrono::milliseconds tombstone_retention{15000};       // IR-2: 15s 墓碑保留期（区分迟到响应与未匹配响应）
    std::chrono::milliseconds window_retention{120000};         // 120s 历史窗口老化
};

/**
 * @brief DNS 事务权威生命周期追踪器 (SR-11: Userspace Tracker 为唯一 Authority)
 *
 * 核心逻辑与硬约束：
 * - IR-1: 容量超限拒绝并累加 tracker_insert_failures
 * - IR-2: Atomic Claim、Direction Normalization、15s Tombstone、重传仅累加 attempt_count
 * - SR-4: 支持 binding_epoch 推进，旧 epoch 事务被丢弃或隔离
 * - SR-8: 精确统计 unmatched, tracking_ambiguous, late_responses, tracker_insert_failures, event_delivery_loss
 * - SR-10 & SR-14 & 修正 #4: 互斥分类：malformed -> OTHER; TC==1 -> TRUNCATED; 否则按 RCODE 分类
 * - 修正 #3: getSnapshot() 与 getWindowMetrics() 协同保证 snapshot 一致性
 */
class DnsTransactionTracker {
public:
    using Config = DnsTrackerConfig;

    explicit DnsTransactionTracker(Config cfg = Config());

    // 推进网络/Resolver 绑定 Epoch (SR-4)
    void advanceBindingEpoch(uint64_t new_epoch);
    uint64_t currentBindingEpoch() const;

    // 记录 Event Transport 丢失 (修正 #2: perf/ring buffer 消费失败或 drop)
    void recordDeliveryLoss(uint64_t lost_count = 1);

    /**
     * @brief 记录一轮传输层观测增量（由 DnsMonitor 从 BPF 计数器与 drain 统计取差得出）。
     *
     * 两级分别计量：capture 是 BPF 输出阶段，delivered/lost 是 perf 投递阶段。
     * 两者是否描述同一轮拥塞尚未验证，故不合并。
     */
    void recordTransportDelta(uint64_t capture_attempts,
                              uint64_t capture_emit_failures,
                              uint64_t delivered_events,
                              uint64_t perf_lost_events);

    /**
     * @brief 捕获 DNS Query 请求事件
     * @param key 客户端视角的 Canonical Key
     * @param quality 指纹完备度 (ENRICHED vs PARTIAL)
     * @param now 捕获单调时间戳
     * @return true=成功纳管或重传更新; false=容量溢出被拒绝 (IR-1)
     */
    bool onQueryCaptured(const DnsCanonicalKey& key,
                         FingerprintQuality quality = FingerprintQuality::ENRICHED,
                         std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    /**
     * @brief 捕获 DNS Response 响应事件
     * @param key 客户端视角的 Canonical Key (若底层送入对端视角，调用方或此函数需归一化)
     * @param rcode DNS 响应码 (0=NOERROR, 2=SERVFAIL, 3=NXDOMAIN, 5=REFUSED 等)
     * @param tc 是否截断 (TC=1, SR-14)
     * @param is_malformed 报文是否畸变损坏
     * @param now 捕获单调时间戳
     */
    void onResponseCaptured(const DnsCanonicalKey& key,
                            uint8_t rcode,
                            bool tc,
                            bool is_malformed = false,
                            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    /**
     * @brief 周期性扫描并裁决超时事务 (IR-2: Atomic Claim, scan -> find -> claim -> terminalize)
     * @param now 当前单调时间戳
     * @return 本轮发生超时的事务数量
     */
    size_t sweepTimeouts(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    /**
     * @brief 获取权威当前快照 (修正 #3)
     */
    DnsTrackerSnapshot getSnapshot(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const;

    /**
     * @brief 导出指定窗口期内的聚合指标 (SR-1, SR-8, SR-10, SR-14)
     * @param window_duration 回溯窗口时长 (默认 120s)
     * @param cutoff_time 统一的 evaluation_cutoff 时间戳
     *
     * 非 const：Observer 质量字段按窗口增量输出，导出即推进窗口基线。
     */
    DnsMetricWindow getWindowMetrics(std::chrono::milliseconds window_duration = std::chrono::milliseconds(120000),
                                     std::chrono::steady_clock::time_point cutoff_time = std::chrono::steady_clock::now());

    /**
     * @brief 获取最近终态事务列表 (老到新)，用于 SR-9 突发连续判定
     */
    std::vector<DnsTransactionRecord> getRecentTerminals(size_t limit = 100) const;

    void reset();

private:
    struct TombstoneRecord {
        DnsCanonicalKey key;
        uint64_t binding_epoch{0};
        std::chrono::steady_clock::time_point expired_at;
        DnsTransactionState original_state{DnsTransactionState::TIMEOUT_EXPIRED};
    };

    Config cfg_;
    mutable std::mutex mutex_;

    uint64_t current_binding_epoch_{1};
    uint64_t revision_{0};
    uint64_t generation_counter_{0};

    // 在途事务表 (ACTIVE)
    std::unordered_map<DnsCanonicalKey, DnsTransactionRecord, DnsCanonicalKeyHash> active_txs_;

    // 15s 墓碑表 (区分 LATE_RESPONSE 和 UNMATCHED)
    std::unordered_map<DnsCanonicalKey, TombstoneRecord, DnsCanonicalKeyHash> tombstones_;

    // 终态事务环形记录列表（用于窗口统计与突发检测）
    std::deque<DnsTransactionRecord> terminal_history_;

    // 观测质量累加器
    uint64_t unmatched_count_{0};
    uint64_t tracking_ambiguous_count_{0};
    uint64_t late_responses_count_{0};
    uint64_t tracker_insert_failures_{0};
    uint64_t event_delivery_loss_{0};
    uint64_t response_match_attempts_{0};
    uint64_t query_capture_attempts_{0};
    uint64_t total_captured_events_{0};

    // --- 窗口基线：Observer 质量评价使用窗口增量，而非 lifetime 累计 ---
    // 累计比率会被历史大样本稀释（当前已坏却测不出），也会被早期故障长期污染。
    uint64_t window_base_unmatched_{0};
    uint64_t window_base_ambiguous_{0};
    uint64_t window_base_insert_failures_{0};
    uint64_t window_base_response_matches_{0};
    uint64_t window_base_query_attempts_{0};
    uint64_t window_base_capture_attempts_{0};
    uint64_t window_base_capture_emit_failures_{0};
    uint64_t window_base_delivered_events_{0};
    uint64_t window_base_perf_lost_events_{0};
    std::chrono::steady_clock::time_point window_base_at_{std::chrono::steady_clock::now()};

    // 传输层累计值（由 recordTransportDelta 累加）
    uint64_t capture_attempts_{0};
    uint64_t capture_emit_failures_{0};
    uint64_t delivered_events_{0};
    uint64_t perf_lost_events_{0};

    void advanceWindowBaselineLocked(std::chrono::steady_clock::time_point now);
};

} // namespace weaknet

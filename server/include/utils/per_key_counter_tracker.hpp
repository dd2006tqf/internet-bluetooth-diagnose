/**
 * @file per_key_counter_tracker.hpp
 * @brief 逐 key 累计计数器的差分追踪（eBPF Map 驱逐可见性）
 *
 * 背景：LRU_HASH 类型的 eBPF Map 在容量饱和时会驱逐最久未用的条目。
 * 被驱逐的 key 若随后再次出现，其累计计数从 0 重新开始——用户态做
 * delta 时会看到"计数器倒退"，直接使用会产生负增量或伪造的尖峰。
 *
 * 本追踪器在用户态保留上一轮的 key→value 快照，对当前轮做差分：
 *   - new         ：上轮无、本轮有
 *   - disappeared ：上轮有、本轮无（驱逐 或 正常结束，无法区分）
 *   - reset       ：两轮都有但 value 变小（驱逐后重建 / 内核清零）
 *   - 其余        ：正常增长
 *
 * 线程模型：非线程安全。每个 map 的扫描循环独占一个实例（与
 * CounterNormalizer 在 wifi_loss 线程内的用法同构）。
 */

#pragma once

#include <cstdint>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace weaknet {

/// 一次差分的结果
struct PerKeyStats {
    uint64_t new_keys{0};          ///< 上轮无、本轮有
    uint64_t disappeared_keys{0};  ///< 上轮有、本轮无（驱逐或正常结束）
    uint64_t reset_keys{0};        ///< 两轮都有但计数倒退（驱逐后重建）
    uint64_t entries{0};           ///< 本轮条目数
    uint64_t max_entries{0};       ///< 该 map 的 resolved 容量
    double watermark_pct{0.0};     ///< entries / max_entries × 100
    bool eviction_limited{false};  ///< 水位过高且消失量达阈 → 观测受驱逐限制
};

/// 水位与消失量判据：水位 ≥90% 且本窗口消失 key 数达该阈值
constexpr double kEvictionWatermarkPct = 90.0;
constexpr uint64_t kEvictionDisappearThreshold = 100;

/**
 * @brief 逐 key 累计计数器的差分追踪器
 *
 * 用法：
 * @code
 *   PerKeyCounterTracker tracker;
 *   std::vector<std::pair<std::string, uint64_t>> snap;
 *   // ... 遍历 eBPF map 填充 snap ...
 *   PerKeyStats st = tracker.update(snap, resolved_max_entries);
 * @endcode
 */
class PerKeyCounterTracker {
public:
    PerKeyCounterTracker() = default;

    /**
     * @brief 消费本轮快照，与上一轮差分
     *
     * @param current     本轮 (key, 累计值) 列表
     * @param max_entries 该 map 的 resolved 容量（0 = 未知，不计水位）
     * @return 本轮差分统计；首轮全部计入 new_keys
     */
    template <typename Container>
    PerKeyStats update(const Container& current, uint64_t max_entries = 0) {
        PerKeyStats st;
        st.entries = static_cast<uint64_t>(std::distance(current.begin(), current.end()));
        st.max_entries = max_entries;

        // 本轮出现过的 key，用于判定上轮有、本轮无的"消失"
        seen_.clear();
        seen_.reserve(static_cast<size_t>(st.entries));

        for (const auto& item : current) {
            const std::string& key = item.first;
            const uint64_t value = static_cast<uint64_t>(item.second);
            seen_.insert(key);

            auto it = prev_.find(key);
            if (it == prev_.end()) {
                ++st.new_keys;
            } else if (value < it->second) {
                // 计数倒退：LRU 驱逐后 key 重建，累计值从 0 重来。
                // 这正是本追踪器存在的理由——直接做 delta 会得到负增量。
                ++st.reset_keys;
            }
            // 其余为正常增长，不单独计数
        }

        // 上轮有、本轮无：驱逐 或 正常结束（两者无法区分，合并计数）
        for (const auto& kv : prev_) {
            if (seen_.find(kv.first) == seen_.end()) ++st.disappeared_keys;
        }

        // 本轮快照成为下一轮基线
        prev_.clear();
        prev_.reserve(static_cast<size_t>(st.entries));
        for (const auto& item : current) {
            prev_.emplace(item.first, static_cast<uint64_t>(item.second));
        }
        seen_.clear();

        // 水位：容量未知（max_entries=0）时不作判定，避免凭空断言观测受损
        if (max_entries > 0) {
            st.watermark_pct = static_cast<double>(st.entries) * 100.0 /
                               static_cast<double>(max_entries);
            st.eviction_limited = st.watermark_pct >= kEvictionWatermarkPct &&
                                  st.disappeared_keys >= kEvictionDisappearThreshold;
        }
        return st;
    }

    /// 清空基线（restart / shutdown / clearHistory 时调用）
    void reset() { prev_.clear(); seen_.clear(); }

    /// 当前基线条目数（测试与诊断用）
    size_t baselineSize() const { return prev_.size(); }

private:
    std::unordered_map<std::string, uint64_t> prev_;
    /// 复用缓冲，避免每轮重新分配
    std::unordered_set<std::string> seen_;
};

} // namespace weaknet

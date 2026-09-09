#pragma once

#include <cstdint>
#include <optional>

namespace weaknet {

/**
 * @brief 处理累计计数器转周期丢包率的规约器
 *
 * 专门针对 wifi_loss 等内核 tracepoint 累计 Counter：
 * 处理开机首轮、probe 重载、网卡重置、Counter 倒退与零活跃度。
 */
class CounterNormalizer {
public:
    struct CounterSample {
        uint64_t total_packets{0};
        uint64_t dropped_packets{0};
    };

    struct RateResult {
        double rate_percent{0.0};
        uint64_t delta_packets{0};
        uint64_t delta_drops{0};
    };

    CounterNormalizer() = default;

    /**
     * @brief 消费一次新的累计快照，返回周期增量丢包率
     * @return 若为首轮、计数器重置或无包发送，返回 std::nullopt，避免生成伪 rate
     */
    std::optional<RateResult> update(uint64_t total_packets, uint64_t dropped_packets) {
        if (!has_prev_) {
            prev_total_ = total_packets;
            prev_drops_ = dropped_packets;
            has_prev_ = true;
            return std::nullopt; // 首轮建立基线
        }

        // 检测计数器倒退（重载、网卡重启等）
        if (total_packets < prev_total_ || dropped_packets < prev_drops_) {
            prev_total_ = total_packets;
            prev_drops_ = dropped_packets;
            return std::nullopt; // 重置基线，本轮丢弃
        }

        uint64_t delta_total = total_packets - prev_total_;
        uint64_t delta_drops = dropped_packets - prev_drops_;

        prev_total_ = total_packets;
        prev_drops_ = dropped_packets;

        if (delta_total == 0) {
            return std::nullopt; // 无发包活动
        }

        RateResult res;
        res.delta_packets = delta_total;
        res.delta_drops = delta_drops;
        res.rate_percent = (static_cast<double>(delta_drops) * 100.0) / static_cast<double>(delta_total);
        if (res.rate_percent > 100.0) res.rate_percent = 100.0;
        return res;
    }

    void reset() {
        has_prev_ = false;
        prev_total_ = 0;
        prev_drops_ = 0;
    }

private:
    bool has_prev_{false};
    uint64_t prev_total_{0};
    uint64_t prev_drops_{0};
};

} // namespace weaknet

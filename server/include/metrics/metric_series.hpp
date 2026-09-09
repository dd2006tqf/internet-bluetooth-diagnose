#pragma once

#include "metrics/metric_sample.hpp"
#include <vector>
#include <mutex>
#include <optional>
#include <algorithm>

namespace weaknet {

/**
 * @brief 单指标定长环形缓冲序列
 *
 * Capacity 由设计准则推导：
 * retention_duration (120s) / min_supported_sampling_interval (1s) + margin = 120 + 40 = 160
 */
class MetricSeries {
public:
    static constexpr size_t kDefaultCapacity = 160;

    explicit MetricSeries(MetricDescriptor desc, size_t capacity = kDefaultCapacity)
        : descriptor_(std::move(desc)), capacity_(capacity > 0 ? capacity : kDefaultCapacity), buffer_(capacity_) {}

    void push(const MetricSample& sample) {
        std::lock_guard<std::mutex> lock(mutex_);
        MetricSample s = sample;
        s.sequence = ++sequence_counter_;
        buffer_[head_] = s;
        head_ = (head_ + 1) % capacity_;
        if (size_ < capacity_) {
            size_++;
        }
    }

    std::optional<MetricSample> latest() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (size_ == 0) return std::nullopt;
        size_t latest_idx = (head_ == 0) ? (capacity_ - 1) : (head_ - 1);
        return buffer_[latest_idx];
    }

    /**
     * @brief 提取时间窗口内所有的样本（按时间从老到新排序的拷贝）
     *
     * HR-8: 拷贝后立释互斥锁，Evaluator 在锁外执行计算。
     */
    std::vector<MetricSample> window(std::chrono::milliseconds duration,
                                     std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const {
        std::vector<MetricSample> result;
        std::lock_guard<std::mutex> lock(mutex_);
        if (size_ == 0) return result;

        result.reserve(size_);
        size_t start_idx = (size_ < capacity_) ? 0 : head_;
        for (size_t i = 0; i < size_; ++i) {
            size_t idx = (start_idx + i) % capacity_;
            const auto& sample = buffer_[idx];
            if (now >= sample.observed_at && (now - sample.observed_at) <= duration) {
                result.push_back(sample);
            }
        }
        return result;
    }

    const MetricDescriptor& descriptor() const { return descriptor_; }
    uint64_t currentSequence() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sequence_counter_;
    }

private:
    MetricDescriptor descriptor_;
    const size_t capacity_;
    std::vector<MetricSample> buffer_;
    size_t head_{0};
    size_t size_{0};
    uint64_t sequence_counter_{0};
    mutable std::mutex mutex_;
};

} // namespace weaknet

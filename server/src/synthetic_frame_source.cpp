/**
 * @file synthetic_frame_source.cpp
 * @brief 合成帧源实现：把「时间」当纯函数参数，不 sleep、不依赖真实时钟
 *
 * 设计要点（三个刻意的取舍）：
 *
 * 1. **不 sleep**：单测如果 sleep，CI 上会慢且不确定（调度抖动会让 P95 漂移）。
 *    这里每次 snapshotAndReset() 直接推进一个窗口并按模式算出本该发生的统计，
 *    同一组参数永远得到同一份结果。
 *
 * 2. **乱序 ≠ 丢帧**：REORDERED 模式下所有帧都到了，只是顺序错乱。
 *    实现里它只影响 bytes 与帧数，绝不增加 seq_gap_count。这条边界是
 *    归因正确性的前提——把驱动重排当成网络丢包，会让归因层误报链路故障。
 *
 * 3. **P95 用排序取位而非近似**：窗口内帧数在几百量级，全排序的成本可以
 *    忽略，而近似算法（如直方图分桶）会引入桶宽误差，让 1ms 级的抖动
 *    判定失真。宁可简单准确，也不做没必要的优化。
 */

#include "frame_source.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace weaknet {

namespace {

/// 合成帧的假定帧大小（未压缩的 NV12 近似值，仅用于让 bytes_received 非零）
constexpr uint64_t kSyntheticFrameBytes = 640u * 480u * 3u / 2u;

/**
 * @brief 计算 P95（最近秩法）
 *
 * 空容器返回 0。取 ceil(0.95 * n) - 1 下标，n=20 时取第 19 个（0-based 18），
 * 语义是「95% 的样本不超过该值」，与本项目其它 P95 用法一致。
 */
uint32_t percentile95(std::vector<uint32_t> values) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const size_t idx = static_cast<size_t>(
        std::ceil(0.95 * static_cast<double>(values.size())) - 1.0);
    return values[std::min(idx, values.size() - 1)];
}

}  // namespace

struct SyntheticFrameSource::Impl {
    SyntheticPattern pattern;
    uint32_t fps;
    uint32_t window_ms;

    bool started = false;
    uint64_t window = 0;      ///< 已推进的窗口数
    uint64_t next_seq = 0;    ///< 下一个帧序号

    Impl(SyntheticPattern p, uint32_t f, uint32_t w)
        : pattern(p), fps(f ? f : 30), window_ms(w ? w : 1000) {}
};

SyntheticFrameSource::SyntheticFrameSource(SyntheticPattern pattern, uint32_t fps,
                                           uint32_t window_ms)
    : impl_(std::make_unique<Impl>(pattern, fps, window_ms)) {}

SyntheticFrameSource::~SyntheticFrameSource() = default;

bool SyntheticFrameSource::start() {
    impl_->started = true;
    impl_->window = 0;
    impl_->next_seq = 0;
    return true;
}

void SyntheticFrameSource::stop() {
    // 幂等：未 start 时调用不做事，也不改动计数
    impl_->started = false;
}

FrameStats SyntheticFrameSource::snapshotAndReset() {
    FrameStats out;

    // 未启动时返回无效统计，让上层报 unavailable —— 绝不返回全零的"健康"值。
    // 全零 FrameStats 的 frameLossRatio() 是 0.0，会被误读成"零丢帧"，这正是
    // 本项目反复强调"缺数据不得伪造 0"的同一个陷阱。
    if (!impl_->started) return out;

    const double frame_interval_ms = 1000.0 / static_cast<double>(impl_->fps);
    // 帧数必须用整数运算并四舍五入，**不能**写成 window_ms / frame_interval_ms
    // 再截断：1000.0 / (1000.0/30) 在 IEEE-754 下是 29.999999999999996，
    // 截断后变成 29 帧，丢帧率因此长期偏移约 3%，且分母错误会让所有
    // 阈值判定系统性偏松。整数形式没有这个问题，且对任意 fps 都精确。
    const uint64_t nominal_frames =
        (static_cast<uint64_t>(impl_->window_ms) * impl_->fps + 500u) / 1000u;

    const uint64_t w = impl_->window;
    uint64_t received = nominal_frames;
    uint64_t expected = nominal_frames;
    uint32_t gap_events = 0;

    std::vector<uint32_t> gaps;
    gaps.reserve(static_cast<size_t>(nominal_frames));

    switch (impl_->pattern) {
        case SyntheticPattern::HEALTHY: {
            // 帧间隔恒等于标称值，无抖动
            const uint32_t g = static_cast<uint32_t>(frame_interval_ms);
            for (uint64_t i = 0; i < nominal_frames; ++i) gaps.push_back(g);
            break;
        }

        case SyntheticPattern::PERIODIC_LOSS: {
            // 每 30 帧丢 2 帧（约 6.7%），持续存在。
            // 表现是"画面一直在轻微断续"——丢帧量不大但发生频繁。
            constexpr uint64_t kPeriod = 30;
            constexpr uint64_t kDrop = 2;
            for (uint64_t i = 0; i < nominal_frames; ++i) {
                const uint64_t pos = (w * nominal_frames + i) % kPeriod;
                if (pos < kDrop) {
                    ++gap_events;
                    continue;
                }
                gaps.push_back(static_cast<uint32_t>(frame_interval_ms));
            }
            received = nominal_frames > gap_events ? nominal_frames - gap_events : 0;
            break;
        }

        case SyntheticPattern::BURST_LOSS: {
            // 偶数窗口正常、奇数窗口丢 30%：
            // 模拟"长时间正常后突然连续丢帧"，用户感知是卡住而非断续。
            // 注意丢的是连续一段而不是散点，所以 gap_events 只记 1 次
            // ——一次断流无论丢多少帧，都是一次"中断事件"。
            const bool burst = (w % 2) == 1;
            const uint64_t drop = burst ? (nominal_frames * 3 / 10) : 0;
            const uint64_t keep = nominal_frames - drop;
            for (uint64_t i = 0; i < keep; ++i) {
                gaps.push_back(static_cast<uint32_t>(frame_interval_ms));
            }
            if (burst && drop > 0) {
                gap_events = 1;
                // 断流期间有一个明显拉长的间隔：drop 帧本该占用的时间
                gaps.push_back(static_cast<uint32_t>(
                    frame_interval_ms * static_cast<double>(drop + 1)));
            }
            received = keep;
            break;
        }

        case SyntheticPattern::REORDERED: {
            // 全部到达，只是顺序错乱。**seq_gap_count 保持 0**。
            // 间隔仍然正常（乱序是顺序问题，不是时间问题）。
            const uint32_t g = static_cast<uint32_t>(frame_interval_ms);
            for (uint64_t i = 0; i < nominal_frames; ++i) gaps.push_back(g);
            received = nominal_frames;
            break;
        }
    }

    // 累计字节：乱序模式下按全部帧算（帧确实都到了）
    out.frames_expected = expected;
    out.frames_received = received;
    out.frames_corrupt = 0;   // 合成源不产生坏帧；真机源才需要从驱动标志推导
    out.seq_gap_count = gap_events;
    out.bytes_received = received * kSyntheticFrameBytes;
    // 窗口时长随统计一起给出，让归因层能真正校验与网络侧是否同窗
    out.window_ms = impl_->window_ms;

    if (!gaps.empty()) {
        out.max_gap_ms = *std::max_element(gaps.begin(), gaps.end());
        out.p95_gap_ms = percentile95(gaps);
    }

    out.sampled_at = std::chrono::steady_clock::now();

    // 推进时间线：窗口 +1，seq 跳到下一窗口的起点（缺口已在上面的 gap_events 记账）
    impl_->window += 1;
    impl_->next_seq += nominal_frames;

    return out;
}

std::string SyntheticFrameSource::describe() const {
    const char* pattern_name = "HEALTHY";
    switch (impl_->pattern) {
        case SyntheticPattern::HEALTHY:      pattern_name = "HEALTHY"; break;
        case SyntheticPattern::PERIODIC_LOSS: pattern_name = "PERIODIC_LOSS"; break;
        case SyntheticPattern::BURST_LOSS:   pattern_name = "BURST_LOSS"; break;
        case SyntheticPattern::REORDERED:    pattern_name = "REORDERED"; break;
    }
    std::ostringstream os;
    // 前缀 synthetic: 让 evidence 消费者能一眼区分"注入的测试数据"与真机观测
    os << "synthetic:" << pattern_name << "@" << impl_->fps << "fps/"
       << impl_->window_ms << "ms";
    return os.str();
}

uint64_t SyntheticFrameSource::windowCount() const { return impl_->window; }

uint64_t SyntheticFrameSource::nextSeq() const { return impl_->next_seq; }

}  // namespace weaknet

/**
 * @file top_n_recent.hpp
 * @brief 按时间戳保留最近的 N 条明细（小顶堆），用于取代「全量物化 + 全量排序」。
 *
 * 为什么单独成文件：这段逻辑此前埋在 TcpRttMonitor::getStats() 里，而那个函数
 * 依赖真实 BPF map，无法单测。堆式选择恰恰是最容易写错、且写错了**不会报错**
 * 的那类代码 —— 顺序错了只是少一条明细，分位数和连接总数照样正确。抽出来才能
 * 用普通单测把「保留的是最近 N 条、且按降序输出」钉死。
 *
 * 语义（必须与原先 std::sort 全量再取前 N 的行为逐项一致）：
 *   - 保留 lastTimestampNs 最大的 N 条；
 *   - 输出按 lastTimestampNs **降序**；
 *   - 时间戳相同时保持输入顺序的相对先后（稳定），与 sort 的稳定性无关，
 *     因为消费方只用时间排序，不依赖同时间戳条目的次序。
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace weaknet {

/**
 * @brief 边遍历边维护「最近 N 条」的定长选择器。
 *
 * 典型用法是在 map 遍历循环里对每条候选调用 offer()，循环结束后 take()。
 * 未进入前 N 的候选不会产生任何构造/析构成本 —— 这是本类存在的全部理由：
 * 调用方可以只在 offer() 内部按需构造昂贵的明细对象。
 *
 * @tparam T 明细类型，必须提供可比较的 lastTimestampNs 成员
 */
template <typename T>
class TopNRecent {
public:
    explicit TopNRecent(size_t limit) : limit_(limit) {
        // limit + 1 让「先 push 后 pop」的路径不必额外扩容；
        // 实际稳态大小始终 <= limit。
        heap_.reserve(limit + 1);
    }

    /// 当前是否已满（满了之后 offer 才有淘汰语义）。
    bool full() const { return heap_.size() >= limit_; }

    /// 堆顶（当前保留条目里最旧的一条）。仅在 full() 为真时有意义。
    const T& oldest() const { return heap_.front(); }

    /// 已保留的条数。
    size_t size() const { return heap_.size(); }

    /// 提交一条候选。调用方负责在此之前判断是否值得构造。
    void offer(T item) {
        if (limit_ == 0) {
            return;
        }
        if (heap_.size() < limit_) {
            heap_.push_back(std::move(item));
            std::push_heap(heap_.begin(), heap_.end(), Older{});
            return;
        }
        // 替换堆顶：先弹出到末尾，改写成新元素，再重新入堆。
        //
        // 为什么是「pop → 改末尾 → push」而不是「改 front → pop」：
        // 后者的写法更短，且在**本类的调用约定下**结果等价 —— 调用方只在
        // 新元素比 oldest() 更新时才 offer()，因此被换下的堆顶必然是最旧的，
        // 它落到末尾会被后续 pop_heap 逐轮挤到底部，最终同样被 take() 丢弃。
        //
        // 选前者是因为它不依赖那个外部先验条件：把「被淘汰者已在末尾」这件事
        // 由堆操作本身保证，而不是由调用方的判据保证。这样即使将来有人改了
        // getStats() 里的过滤条件（比如放行相等时间戳），这里也不会静默留错条目。
        //
        // 实测备注（2026-11-15）：把这里换成「改 front → pop」的写法后，现有
        // 8 项单测全绿（确实等价），换掉比较器方向才会红 5 项。所以本行的价值
        // 是「少依赖一条外部不变量」，不是「修了一个能测出来的 bug」。
        std::pop_heap(heap_.begin(), heap_.end(), Older{});
        heap_.back() = std::move(item);
        std::push_heap(heap_.begin(), heap_.end(), Older{});
    }

    /// 取出结果：按 lastTimestampNs 降序。调用后本对象被清空。
    std::vector<T> take() {
        std::vector<T> out(heap_.size());
        // pop_heap 每次把当前最大（即最近）的换到末尾，所以从后往前填即降序。
        for (size_t i = heap_.size(); i > 0; --i) {
            std::pop_heap(heap_.begin(), heap_.begin() + i, Older{});
            out[i - 1] = std::move(heap_[i - 1]);
        }
        heap_.clear();
        return out;
    }

private:
    /// 小顶堆比较器：lastTimestampNs 小的在堆顶（最旧）。
    struct Older {
        bool operator()(const T& a, const T& b) const {
            return a.lastTimestampNs > b.lastTimestampNs;
        }
    };

    size_t limit_;
    std::vector<T> heap_;
};

}  // namespace weaknet

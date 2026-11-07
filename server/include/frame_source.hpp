/**
 * @file frame_source.hpp
 * @brief 帧源抽象与帧统计（视频 QoE 的 L2/L3 层，零硬件可实现）
 *
 * 为什么要有这层抽象（而不是直接调 V4L2）：
 *
 *   视频质量判定需要两类完全不同的东西——「帧到底有没有按时到」和
 *   「为什么没到」。前者必须在真机上采，后者是纯推理。如果把两者
 *   焊死在一起，摄像头没到货就一行代码都写不了，摄像头到了又只能
 *   在真机上调试推理逻辑。
 *
 *   所以按 驱动面方案 §6.2.1 把两者切开：
 *
 *     L4  VideoQoeAttribution   ← 纯函数，只吃 FrameStats
 *     L3  FrameStats            ← 本文件定义的数据契约
 *     L2  IFrameSource          ← 本文件定义的接口（唯一分界线）
 *         ├─ SyntheticFrameSource   合成源，无硬件即可测透 L3/L4
 *         └─ V4l2FrameSource        真机源，待摄像头到货
 *     L1  内核 vind / csi / isp / sensor
 *
 *   IFrameSource 是唯一的分界线。它的契约稳定之后，换实现（合成→真机）
 *   不需要改动任何上层代码。
 *
 * 重要语义边界（不要越过）：
 *   FrameStats 是**原始事实**，不是结论。丢帧不等于网络故障，卡顿不等于
 *   链路劣化。它只能说明「这一路视频在采集端表现如何」，成因判定是
 *   VideoQoeAttribution 的职责，两者不可混淆。
 *
 *   同理，帧统计**不能**修改 OverallPolicy 的判决——它只产出归因。
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace weaknet {

/**
 * @brief 一个采样周期内的帧级统计（原始事实）
 *
 * 字段刻意只保留能从帧序号与到达时刻推导出来的量，不含任何「结论」。
 * 特别地：**没有 healthy / degraded 这类布尔字段**——那属于归因层。
 *
 * 时间语义：所有 *gap* 字段使用 std::chrono::steady_clock，不受系统
 * 时间跳变影响。这不是可选项：本项目的目标场景是工业现场，NTP 跳变
 * 会把 wall clock 差值直接变成几十年的假间隔。
 */
struct FrameStats {
    /// 按帧率推算「本周期应该到多少帧」
    ///
    /// 为什么需要它而不是直接用 frames_received：没有期望值就无法算丢帧率。
    /// 合成源可以精确给出；真机源需要从 V4L2 的 timeperframe 或配置推导。
    uint64_t frames_expected = 0;

    /// 实际收到的帧数
    uint64_t frames_received = 0;

    /// 序号不连续但内容可用的帧数（如驱动重排、丢帧后补齐）
    uint64_t frames_corrupt = 0;

    /// 序号跳变次数（≈ 丢帧次数，按「事件」计而非按「帧」计）
    ///
    /// 与 frames_expected - frames_received 的区别：后者是丢帧总量，
    /// 前者是丢帧发生的**次数**。一次丢 30 帧和三十次各丢 1 帧，
    /// 总量相同但 QoE 天差地别——前者是拔线，后者是拥塞。
    uint32_t seq_gap_count = 0;

    /// 最长帧间隔（毫秒）
    uint32_t max_gap_ms = 0;

    /// 帧间隔 P95（毫秒）——用于判断「整体卡顿」而非「偶发尖刺」
    uint32_t p95_gap_ms = 0;

    /// 本周期收到的总字节数（用于推算实际码率）
    uint64_t bytes_received = 0;

    /// 本统计窗口的时长（毫秒）
    ///
    /// 为什么必须在这里而不是由调用方另外记：方案 §6.2.4 要求「帧统计与网络
    /// 指标的时间窗必须对齐，否则四象限会误判」。窗口时长若不跟着统计一起走，
    /// 调用方就得自己记住"上一次采样是哪一档窗口"，一旦网络侧与帧侧用了
    /// 不同档位，归因会在毫无提示的情况下得出反向结论。
    ///
    /// 0 表示未声明（真机源实现若无法确定就保持 0，归因层会拒绝归因而非乱猜）。
    uint32_t window_ms = 0;

    /// 采样时刻。默认构造表示「从未采样」
    std::chrono::steady_clock::time_point sampled_at;

    /// 是否是一份有效采样
    ///
    /// 语义边界：valid()==false 时上层**必须**报 unavailable，
    /// 绝不能把全零字段当成「零丢帧、零延迟」——那是把「不知道」
    /// 伪装成「很好」，比缺数据更危险。
    bool valid() const {
        return sampled_at != std::chrono::steady_clock::time_point{};
    }

    /// 本周期丢帧数（不足 0 时钳到 0，防止期望值配置错误导致负数）
    uint64_t framesLost() const {
        return frames_expected > frames_received ? frames_expected - frames_received : 0;
    }

    /// 本周期丢帧率（0.0 ~ 1.0）。期望为 0 时返回 0，不产生 NaN
    double frameLossRatio() const {
        if (frames_expected == 0) return 0.0;
        return static_cast<double>(framesLost()) / static_cast<double>(frames_expected);
    }
};

/**
 * @brief 帧源接口（L2 层，唯一的分界线）
 *
 * 生命周期约定：
 *   start() → [ snapshotAndReset() ... ] → stop()
 *
 *   snapshotAndReset() 是「取走并清零」语义，不是「读取当前值」。
 *   这样每个统计窗口互不重叠，避免多次调用拿到重复数据导致丢帧率被
 *   重复计算。真机实现内部需要维护两个计数器（一个累计、一个上次快照），
 *   合成实现在推进时间线后直接返回增量。
 *
 * 线程约定：
 *   实现**不要求**线程安全。调用方（插件）单线程轮询即可，
 *   这也避免了在接口层引入锁的开销与死锁面。
 */
class IFrameSource {
public:
    virtual ~IFrameSource() = default;

    /// 开始采集。返回 false 表示无法启动（设备不存在、权限不足等）。
    ///
    /// 语义边界：start() 失败**不是**错误状态，是"这个能力不可用"。
    /// 上层应报 unavailable 并让业务链路照常运行，不得中断采集服务。
    virtual bool start() = 0;

    /// 停止采集。必须可重入（未 start 时调用不得崩溃）。
    virtual void stop() = 0;

    /// 取走本周期的统计并清零。无数据时返回 valid()==false 的 FrameStats。
    virtual FrameStats snapshotAndReset() = 0;

    /// 人类可读的来源描述，用于 evidence 溯源。
    ///
    /// 为什么要它：同一个 FrameStats 可能来自合成源或真机源，若不标明来源，
    /// 事后无法区分「这条证据是真的观测」还是「注入的测试数据」。
    /// 这是本项目 evidence 纪律的一部分。
    virtual std::string describe() const = 0;
};

/// 合成源的丢帧模式（用于在无硬件时覆盖全部 QoE 分支）
enum class SyntheticPattern {
    /// 正常：按标称帧率稳定出帧，无丢帧、无抖动
    HEALTHY,

    /// 周期性丢帧：每隔 N 帧丢一批，模拟稳定的网络拥塞
    ///
    /// 典型现场：无线链路带宽刚好不够，编码器超时后周期性放弃。
    PERIODIC_LOSS,

    /// 突发丢帧：长时间正常后突然连续丢帧，模拟链路瞬断
    ///
    /// 与 PERIODIC_LOSS 的区别在于 QoE 含义：一次丢 30 帧的用户感知
    /// 是「画面卡住了」，而周期性每次丢 1 帧是「画面一直在轻微断续」。
    BURST_LOSS,

    /// 乱序：帧都到了但顺序错乱（驱动重排 / 多路径）
    ///
    /// 关键语义：**乱序不是丢帧**。若实现把乱序计入 seq_gap，会让
    /// 归因层把「驱动行为」误判成「网络故障」，这是本模式存在的意义。
    REORDERED,
};

/**
 * @brief 合成帧源（无硬件即可验证整条 L3/L4 链路）
 *
 * 时间驱动而非真实计时：构造时给定帧率与采样周期，每次
 * snapshotAndReset() 推进一个窗口并直接算出本该发生的统计。
 *
 * 为什么不 sleep：sleep 会让单测变慢且不确定（CI 上尤其明显）。
 * 本实现把"时间"当作纯函数参数，同一组参数永远得到同一份 FrameStats，
 * 这正是纯函数可测性的价值。真机源才需要真实时钟。
 */
class SyntheticFrameSource : public IFrameSource {
public:
    /// @param pattern   丢帧模式
    /// @param fps       标称帧率（必须 > 0）
    /// @param window_ms 每个统计窗口的时长（必须 > 0）
    explicit SyntheticFrameSource(SyntheticPattern pattern = SyntheticPattern::HEALTHY,
                                  uint32_t fps = 30,
                                  uint32_t window_ms = 1000);

    /// 析构必须在 .cpp 中定义（pimpl：此处 Impl 尚不完整）
    ~SyntheticFrameSource() override;

    bool start() override;
    void stop() override;
    FrameStats snapshotAndReset() override;
    std::string describe() const override;

    /// 已推进的窗口数（测试用：确认真的在推进而不是返回常量）
    uint64_t windowCount() const;

    /// 帧序号（测试用：真机源的 seq 来自驱动，合成源需要自己维护）
    uint64_t nextSeq() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace weaknet

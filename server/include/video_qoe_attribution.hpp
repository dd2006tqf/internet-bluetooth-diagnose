/**
 * @file video_qoe_attribution.hpp
 * @brief 视频 QoE 归因：用帧级事实校验网络指标，并把「我们自己错了」暴露出来
 *
 * ============================================================================
 * 与 ObserverAttribution 同构
 * ============================================================================
 *
 * 两个问题的形状完全一样：**用一路证据去校验另一路证据，且绝不互相污染**。
 * 所以结构、纪律、纯函数性质都刻意保持一致（见 assurance/observer_attribution.hpp）：
 *
 *   ObserverAttribution：eBPF 观测质量  ← 校验 →  CPU 压力
 *   VideoQoeAttribution：帧级 QoE 事实  ← 校验 →  网络指标
 *
 * ============================================================================
 * 为什么它是一个纯函数，而不是被塞进 QualityAssessor
 * ============================================================================
 *
 * 若把帧统计塞进质量评估器，会让**被观测对象的画面状态污染链路健康度判定**：
 * 同一个网络窗口，在编码器卡时和正常时会得出不同结论。那会让本项目的
 * 同源纪律与证据可复现性全部失效，也让单元测试失去可判定性。
 *
 * 正确分层：
 *   第 1 层 QualityAssessor / SLE 评估 —— 只回答「链路自己怎么样」
 *   第 2 层 VideoQoeAttribution（本模块）—— 回答「用户看到的画面跟链路指标是否一致」
 *
 * 本模块**不修改**任何 OverallPolicy / SLE 的 state/coverage/reason，
 * 只在一旁追加一个独立归因结论。上层可以同时看到两句话。
 *
 * ============================================================================
 * 语义边界（不得越过，逐条有单测守护）
 * ============================================================================
 *
 * - 帧统计缺失（无摄像头 / start 失败 / 未采样）→ 返回 NONE，**绝不**
 *   返回 CONSISTENT_HEALTHY。「不知道」不得伪装成「一切正常」。
 * - 帧统计**不能**修改 OverallPolicy 的判决，只产出归因。
 * - 帧统计与网络指标的时间窗**必须**对齐。窗口时长相同时才允许归因，
 *   不一致直接返回 NONE 并说明原因——四象限在错位窗口下必然误判。
 * - 画面好 + 网络差 = 怀疑**我们自己**（NETWORK_UNVERIFIED），不是给网络开脱，
 *   而是指向我们自己观测方法的缺陷。这是本设计最锋利的地方。
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "assurance/health_state.hpp"   // EvidenceItem
#include "frame_source.hpp"

namespace weaknet {

/**
 * @brief 视频 QoE 归因象限
 *
 * 方案 §6.2.4 的表格实际有五行（标题写「四象限」，NETWORK_UNVERIFIED 是后加的
 * 反向校验行）。这里按五行实现，外加 NONE 表示「无法归因」。
 */
enum class VideoQoeAttribution {
    /// 无法归因：帧统计缺失、窗口错位、或网络指标不可用
    ///
    /// 注意它**不是**一个"差"的结论，而是"没有结论"。
    /// 上游必须把 NONE 呈现为 unavailable，而不是 healthy。
    NONE,

    /// 画面好、网络好 —— 一切正常（唯一一个可以这么说出口的组合）
    CONSISTENT_HEALTHY,

    /// 画面差、网络好 —— 查编码器 / 内存 / CPU / sensor，不是网络
    APP_LAYER_SUSPECT,

    /// 画面差、网络也差，但**尚未持续** —— 可能是瞬态，先观察
    NETWORK_TRANSIENT,

    /// 画面差、网络差且持续多窗口 —— 双重确认，高置信链路故障
    ///
    /// 与 NETWORK_TRANSIENT 的唯一区别是持续性。一次丢帧就说链路坏了，
    /// 现场人员会被派去查一条其实没问题的线。
    CONFIRMED_LINK_FAULT,

    /// 画面好、网络指标却差 —— 反向校验失败，最可能是我们的观测方法有问题
    /// （采样过密、探针冲突、观测者过载）。应转去查 CPU / thermal 归因。
    NETWORK_UNVERIFIED,
};

inline const char* videoQoeAttributionToString(VideoQoeAttribution a) {
    switch (a) {
        case VideoQoeAttribution::NONE: return "NONE";
        case VideoQoeAttribution::CONSISTENT_HEALTHY: return "CONSISTENT_HEALTHY";
        case VideoQoeAttribution::APP_LAYER_SUSPECT: return "APP_LAYER_SUSPECT";
        case VideoQoeAttribution::NETWORK_TRANSIENT: return "NETWORK_TRANSIENT";
        case VideoQoeAttribution::CONFIRMED_LINK_FAULT: return "CONFIRMED_LINK_FAULT";
        case VideoQoeAttribution::NETWORK_UNVERIFIED: return "NETWORK_UNVERIFIED";
        default: return "NONE";
    }
}

/**
 * @brief 与帧统计同窗的网络侧观测
 *
 * 刻意不带任何「网络是否健康」的最终判定——只带症状与持续性，
 * 判定留在本模块，避免调用方替我们下结论。
 */
struct VideoNetworkObservation {
    /// 网络侧指标是否可用。不可用时无法做四象限判定（返回 NONE）
    bool available{false};

    /// 本窗口网络侧是否呈现劣化症状（丢包/重传/丢包率超阈值等）
    bool degraded{false};

    /// 连续劣化窗口数（含本窗口）。用于区分「瞬态」与「持续」
    ///
    /// 为什么由调用方传入而不是本模块自己累计：纯函数不得持有跨调用状态，
    /// 否则同一个输入会因历史不同得到不同输出，单测不可判定。
    uint32_t consecutive_degraded_windows{0};

    /// 该网络窗口的时长（毫秒）。必须与帧窗口一致，否则拒绝归因
    uint32_t interval_ms{0};
};

/**
 * @brief 判定阈值
 */
struct VideoQoeThresholds {
    /// 丢帧率达到多少算「画面差」
    double frame_loss_ratio_bad{0.05};

    /// 帧间隔 P95 超过多少毫秒算「卡顿」
    uint32_t frame_gap_p95_bad_ms{100};

    /// 坏帧率达到多少算「画面差」
    double frame_corrupt_ratio_bad{0.01};

    /// 网络劣化持续多少个窗口才算「持续」（用于 CONFIRMED_LINK_FAULT）
    uint32_t confirm_windows{3};
};

/**
 * @brief 归因结果
 */
struct VideoQoeAttributionResult {
    VideoQoeAttribution attribution{VideoQoeAttribution::NONE};

    /// 是否真的做了一次归因。false 表示输入不足（缺帧统计 / 窗口错位 / 网络不可用），
    /// 此时 attribution 必为 NONE 且 reason 说明原因。
    bool evaluated{false};

    /// 画面侧是否判为差（供证据展示，不代表链路结论）
    bool frames_bad{false};

    /// 未归因的具体原因（机器可读短句）
    std::string reason;

    /// 人类可读解释（进 warnings / 证据 detail）
    std::string explanation;

    /// 机器可读证据条目，直接挂到 SleResult.evidence 上
    std::vector<EvidenceItem> evidence;
};

/**
 * @brief 视频 QoE 归因判定（无状态纯函数）
 *
 * 判定顺序：
 *   1. 帧统计无效 → NONE / frames_unavailable
 *   2. 窗口时长不一致 → NONE / window_mismatch（不做近似对齐）
 *   3. 网络指标不可用 → NONE / network_unavailable
 *   4. 画面好 + 网络好  → CONSISTENT_HEALTHY
 *   5. 画面差 + 网络好  → APP_LAYER_SUSPECT
 *   6. 画面好 + 网络差  → NETWORK_UNVERIFIED
 *   7. 画面差 + 网络差，持续 < confirm_windows → NETWORK_TRANSIENT
 *   8. 画面差 + 网络差，持续 >= confirm_windows → CONFIRMED_LINK_FAULT
 *
 * 第 4~8 条是穷举的，没有落到默认分支的输入。任何新增分支都必须同时
 * 更新这里的顺序说明与单测，否则会静默改变归因含义。
 */
VideoQoeAttributionResult attributeVideoQoe(
    const FrameStats& frames,
    const VideoNetworkObservation& network,
    const VideoQoeThresholds& thresholds = VideoQoeThresholds{});

/**
 * @brief 画面侧是否判为差（暴露出来便于上游做展示，不含链路结论）
 */
bool isFrameQualityBad(const FrameStats& frames,
                       const VideoQoeThresholds& thresholds = VideoQoeThresholds{});

}  // namespace weaknet

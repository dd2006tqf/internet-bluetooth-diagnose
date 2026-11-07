/**
 * @file video_qoe_attribution.cpp
 * @brief 视频 QoE 归因实现：五行真值表 + 三条拒绝归因的前置检查
 *
 * 实现上刻意保持「无状态、无分支副作用、无默认兜底」：
 *
 * - **无默认兜底**：所有正常路径都是穷举的 if/else，末尾不留 `return NONE`。
 *   留兜底会让「新增了一个象限但忘了接上」这件事静默通过编译，
 *   而正确的行为是编译告警 / 单测失败。
 *
 * - **拒绝归因先于归因**：帧缺失、窗口错位、网络不可用这三种情况都在
 *   判定四象限之前返回，且各自给不同的 reason。混在一起会让现场无法区分
 *   「摄像头没插」与「我们自己的窗口算错了」。
 *
 * - **阈值只判画面侧**：本模块不判网络侧好坏，网络侧的 degraded 由调用方
 *   用链路侧的既有阈值算好传进来。理由见头文件——避免两套阈值互相打架。
 */

#include "video_qoe_attribution.hpp"

#include <algorithm>
#include <sstream>

namespace weaknet {

namespace {

/// 把 double 格式化成稳定的小数串（证据里不用科学计数法，便于人工阅读）
std::string fmt(double v) {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(4);
    os << v;
    return os.str();
}

std::string fmtMs(uint32_t v) {
    return std::to_string(v) + "ms";
}

}  // namespace

bool isFrameQualityBad(const FrameStats& frames,
                       const VideoQoeThresholds& thresholds) {
    // 注意：无效统计一律视为「不知道」，由调用方决定怎么呈现。
    // 这里返回 false 是为了让 isFrameQualityBad 只回答「是否明确地差」，
    // 而不是把「不知道」也算进「差」——否则上游会把 unavailable 报成故障。
    if (!frames.valid()) return false;

    if (frames.frameLossRatio() >= thresholds.frame_loss_ratio_bad) return true;

    if (frames.p95_gap_ms >= thresholds.frame_gap_p95_bad_ms) return true;

    if (frames.frames_expected > 0) {
        const double corrupt_ratio =
            static_cast<double>(frames.frames_corrupt) /
            static_cast<double>(frames.frames_expected);
        if (corrupt_ratio >= thresholds.frame_corrupt_ratio_bad) return true;
    }

    return false;
}

VideoQoeAttributionResult attributeVideoQoe(
    const FrameStats& frames,
    const VideoNetworkObservation& network,
    const VideoQoeThresholds& thresholds) {
    VideoQoeAttributionResult result;

    // ---- 前置检查 1：帧统计缺失 ----
    //
    // 这是最重要的一条纪律：无摄像头时归因必须是 NONE，绝不能因为
    // frames_expected==0 导致丢帧率算出 0.0 就报 CONSISTENT_HEALTHY。
    // 「不知道」伪装成「一切正常」比缺数据更危险。
    if (!frames.valid()) {
        result.attribution = VideoQoeAttribution::NONE;
        result.evaluated = false;
        result.reason = "frames_unavailable";
        result.explanation =
            "帧统计不可用（无摄像头 / 未启动 / 未采样），无法做视频 QoE 归因";
        return result;
    }

    // ---- 前置检查 2：时间窗对齐 ----
    //
    // 方案 §6.2.4 纪律：帧统计与网络指标的时间窗必须对齐，否则四象限会误判。
    // 这里选择**拒绝归因**而不是尝试近似对齐：一个 200ms 的帧窗口配 5s 的
    // 网络窗口，"画面差但网络好"完全可能是窗口错位造成的假象。
    //
    // 两侧任一方未声明窗口时长都拒绝归因：把「没填」当成「一致」正是
    // 方案里点名要避免的误判来源。
    if (frames.window_ms == 0 || network.interval_ms == 0) {
        result.attribution = VideoQoeAttribution::NONE;
        result.evaluated = false;
        result.frames_bad = isFrameQualityBad(frames, thresholds);
        result.reason = "window_length_undeclared";
        result.explanation =
            "帧统计或网络侧未声明统计窗口时长（frames.window_ms=" +
            std::to_string(frames.window_ms) + ", network.interval_ms=" +
            std::to_string(network.interval_ms) +
            "），无法确认两路证据同窗，拒绝归因";
        return result;
    }
    if (frames.window_ms != network.interval_ms) {
        result.attribution = VideoQoeAttribution::NONE;
        result.evaluated = false;
        result.frames_bad = isFrameQualityBad(frames, thresholds);
        result.reason = "window_mismatch";
        result.explanation =
            "帧窗口 " + std::to_string(frames.window_ms) + "ms 与网络窗口 " +
            std::to_string(network.interval_ms) +
            "ms 不一致，四象限在错位窗口下必然误判，拒绝归因";
        return result;
    }

    // ---- 前置检查 3：网络指标不可用 ----
    if (!network.available) {
        result.attribution = VideoQoeAttribution::NONE;
        result.evaluated = false;
        result.frames_bad = isFrameQualityBad(frames, thresholds);
        result.reason = "network_unavailable";
        result.explanation =
            "网络侧指标不可用，无法做双向校验（只有画面一路证据时不做链路归因）";
        return result;
    }

    // ================= 五行真值表 =================
    const bool frames_bad = isFrameQualityBad(frames, thresholds);
    const bool network_bad = network.degraded;
    result.frames_bad = frames_bad;
    result.evaluated = true;

    // 证据：画面侧原始事实。数值原样保留，不做任何「修匀」。
    result.evidence.push_back(EvidenceItem{
        "video_frame_loss_ratio", frames.frameLossRatio(),
        "lost=" + std::to_string(frames.framesLost()) + "/" +
            std::to_string(frames.frames_expected)});
    result.evidence.push_back(
        EvidenceItem{"video_frame_gap_p95_ms", static_cast<double>(frames.p95_gap_ms),
                     "max=" + fmtMs(frames.max_gap_ms)});
    result.evidence.push_back(
        EvidenceItem{"video_seq_gap_count", static_cast<double>(frames.seq_gap_count),
                     "gap events, not lost frames"});

    // ---- 画面好 + 网络好 ----
    if (!frames_bad && !network_bad) {
        result.attribution = VideoQoeAttribution::CONSISTENT_HEALTHY;
        result.reason = "frames_good_network_good";
        result.explanation = "画面与网络侧指标一致地正常";
        return result;
    }

    // ---- 画面差 + 网络好 ----
    if (frames_bad && !network_bad) {
        result.attribution = VideoQoeAttribution::APP_LAYER_SUSPECT;
        result.reason = "frames_bad_network_good";
        result.explanation =
            "画面劣化但网络侧指标正常，问题在应用侧或采集侧"
            "（编码器 / 内存 / CPU / sensor / ISP），不是链路";
        return result;
    }

    // ---- 画面好 + 网络差（反向校验）----
    //
    // 这是本设计最锋利的一条：不给网络开脱，而是指向我们自己。
    // 画面完全正常而协议栈说网络很差，最可能的解释是观测方法本身有问题
    // （采样过密、探针冲突、观测者过载）。
    if (!frames_bad && network_bad) {
        result.attribution = VideoQoeAttribution::NETWORK_UNVERIFIED;
        result.reason = "frames_good_network_bad";
        result.explanation =
            "网络侧指标劣化但画面正常，反向校验失败：高度怀疑观测方法本身有问题"
            "（采样过密 / 探针冲突 / 观测者过载），应转查 CPU 与 thermal 归因";
        return result;
    }

    // ---- 画面差 + 网络差：按持续性分裂 ----
    //
    // 一次丢帧就说链路坏了，会让现场人员去查一条其实没问题的线。
    // 持续多窗口才升级为高置信。
    if (network.consecutive_degraded_windows >= thresholds.confirm_windows) {
        result.attribution = VideoQoeAttribution::CONFIRMED_LINK_FAULT;
        result.reason = "frames_bad_network_bad_sustained";
        result.explanation =
            "画面与网络侧指标同时劣化且已持续 " +
            std::to_string(network.consecutive_degraded_windows) + " 个窗口（阈值 " +
            std::to_string(thresholds.confirm_windows) + "），双重确认的链路故障";
        return result;
    }

    // 到这里必然是「画面差 + 网络差 + 未持续」
    result.attribution = VideoQoeAttribution::NETWORK_TRANSIENT;
    result.reason = "frames_bad_network_bad_transient";
    result.explanation =
        "画面与网络侧指标同时劣化，但仅持续 " +
        std::to_string(network.consecutive_degraded_windows) + " 个窗口（阈值 " +
        std::to_string(thresholds.confirm_windows) + "），先观察不急于定性";
    return result;
}

}  // namespace weaknet

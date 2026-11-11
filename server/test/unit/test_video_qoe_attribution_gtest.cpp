/**
 * @file test_video_qoe_attribution_gtest.cpp
 * @brief VideoQoeAttribution 五行真值表与拒绝归因边界的契约测试
 *
 * 这份测试守的是「归因的含义」，而不是「某个数恰好是多少」。
 * 每条断言对应一个会让现场人员查错方向的失效模式：
 *
 *   1. 无摄像头时返回 NONE 而不是 CONSISTENT_HEALTHY —— 缺数据伪装成健康
 *   2. 窗口不一致时拒绝归因而不是硬算 —— 错位窗口得出反向结论
 *   3. 画面好+网络差 → NETWORK_UNVERIFIED —— 指向我们自己而非给网络开脱
 *   4. 一次丢帧不升级为 CONFIRMED_LINK_FAULT —— 误报链路故障导致乱派工
 *   5. 归因结论不写进任何 OverallPolicy 相关字段 —— 帧统计不得影响链路判决
 */

#include <gtest/gtest.h>

#include <chrono>
#include <set>
#include <string>

#include "video_qoe_attribution.hpp"

using namespace weaknet;

namespace {

/// 构造一份「画面完全正常」的帧统计
FrameStats goodFrames(uint32_t window_ms = 1000) {
    FrameStats f;
    f.frames_expected = 30;
    f.frames_received = 30;
    f.seq_gap_count = 0;
    f.max_gap_ms = 33;
    f.p95_gap_ms = 33;
    f.bytes_received = 1000000;
    f.window_ms = window_ms;
    f.sampled_at = std::chrono::steady_clock::now();
    return f;
}

/// 构造一份「画面很差」的帧统计（丢一半）
FrameStats badFrames(uint32_t window_ms = 1000) {
    FrameStats f = goodFrames(window_ms);
    f.frames_received = 15;
    f.seq_gap_count = 15;
    f.p95_gap_ms = 500;
    f.max_gap_ms = 900;
    return f;
}

VideoNetworkObservation netGood(uint32_t interval_ms = 1000) {
    VideoNetworkObservation n;
    n.available = true;
    n.degraded = false;
    n.consecutive_degraded_windows = 0;
    n.interval_ms = interval_ms;
    return n;
}

VideoNetworkObservation netBad(uint32_t interval_ms = 1000, uint32_t consecutive = 1) {
    VideoNetworkObservation n;
    n.available = true;
    n.degraded = true;
    n.consecutive_degraded_windows = consecutive;
    n.interval_ms = interval_ms;
    return n;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 缺数据不得伪装成健康
// ---------------------------------------------------------------------------

TEST(VideoQoeAttribution, MissingFramesYieldsNoneNotHealthy) {
    // 这是整份文件最重要的一条。方案 §6.2.4 纪律明确要求：
    // 帧统计缺失时归因返回 NONE，不伪装成 CONSISTENT_HEALTHY。
    //
    // 陷阱在于：默认 FrameStats 的 frameLossRatio() 是 0.0，看起来"完美"。
    // 若不先检查 valid()，一个没插摄像头的板子会报"画面与网络一致正常"。
    FrameStats missing;

    auto r = attributeVideoQoe(missing, netGood());

    EXPECT_EQ(r.attribution, VideoQoeAttribution::NONE);
    EXPECT_FALSE(r.evaluated);
    EXPECT_EQ(r.reason, "frames_unavailable");
    EXPECT_FALSE(r.explanation.empty()) << "拒绝归因必须说明原因，不能静默";
    EXPECT_TRUE(r.evidence.empty()) << "没有采到任何东西时不得伪造证据条目";
}

TEST(VideoQoeAttribution, MissingFramesIsNoneEvenWhenNetworkIsBad) {
    // 网络差 + 没画面，也不能猜成 NETWORK_TRANSIENT —— 那等于用单路证据下结论
    auto r = attributeVideoQoe(FrameStats{}, netBad(1000, 10));

    EXPECT_EQ(r.attribution, VideoQoeAttribution::NONE);
    EXPECT_EQ(r.reason, "frames_unavailable");
}

TEST(VideoQoeAttribution, NetworkUnavailableYieldsNone) {
    VideoNetworkObservation no_net;
    no_net.available = false;
    no_net.interval_ms = 1000;

    auto r = attributeVideoQoe(goodFrames(), no_net);

    EXPECT_EQ(r.attribution, VideoQoeAttribution::NONE);
    EXPECT_FALSE(r.evaluated);
    EXPECT_EQ(r.reason, "network_unavailable");
    // 画面侧判定仍然要如实给出，便于上层单独展示"画面是好的"
    EXPECT_FALSE(r.frames_bad);
}

// ---------------------------------------------------------------------------
// 2. 窗口对齐（方案点名的误判来源）
// ---------------------------------------------------------------------------

TEST(VideoQoeAttribution, WindowMismatchRefusesToAttribute) {
    auto r = attributeVideoQoe(goodFrames(200), netGood(5000));

    EXPECT_EQ(r.attribution, VideoQoeAttribution::NONE);
    EXPECT_FALSE(r.evaluated);
    EXPECT_EQ(r.reason, "window_mismatch");
    EXPECT_NE(r.explanation.find("200"), std::string::npos)
        << "解释里必须写出实际窗口，现场才能定位是哪一侧配错了";
    EXPECT_NE(r.explanation.find("5000"), std::string::npos);
}

TEST(VideoQoeAttribution, UndeclaredWindowRefusesToAttribute) {
    // 把"没填"当成"一致"是最隐蔽的错误来源
    auto r = attributeVideoQoe(goodFrames(0), netGood(1000));
    EXPECT_EQ(r.attribution, VideoQoeAttribution::NONE);
    EXPECT_EQ(r.reason, "window_length_undeclared");

    auto r2 = attributeVideoQoe(goodFrames(1000), netGood(0));
    EXPECT_EQ(r2.attribution, VideoQoeAttribution::NONE);
    EXPECT_EQ(r2.reason, "window_length_undeclared");
}

// ---------------------------------------------------------------------------
// 3. 五行真值表
// ---------------------------------------------------------------------------

TEST(VideoQoeAttribution, BothGoodIsConsistentHealthy) {
    auto r = attributeVideoQoe(goodFrames(), netGood());

    EXPECT_EQ(r.attribution, VideoQoeAttribution::CONSISTENT_HEALTHY);
    EXPECT_TRUE(r.evaluated);
    EXPECT_FALSE(r.frames_bad);
    EXPECT_EQ(r.reason, "frames_good_network_good");
}

TEST(VideoQoeAttribution, FramesBadNetworkGoodIsAppLayerSuspect) {
    auto r = attributeVideoQoe(badFrames(), netGood());

    EXPECT_EQ(r.attribution, VideoQoeAttribution::APP_LAYER_SUSPECT);
    EXPECT_TRUE(r.frames_bad);
    EXPECT_EQ(r.reason, "frames_bad_network_good");
    // 指向应用侧，必须能从解释里看出"不是链路"
    EXPECT_NE(r.explanation.find("不是链路"), std::string::npos);
}

TEST(VideoQoeAttribution, FramesGoodNetworkBadIsNetworkUnverified) {
    // 反向校验：这是本设计最锋利的一条
    auto r = attributeVideoQoe(goodFrames(), netBad());

    EXPECT_EQ(r.attribution, VideoQoeAttribution::NETWORK_UNVERIFIED);
    EXPECT_FALSE(r.frames_bad);
    EXPECT_EQ(r.reason, "frames_good_network_bad");
    // 指向我们自己，而不是说"网络确实坏了"
    EXPECT_NE(r.explanation.find("观测方法"), std::string::npos);
}

TEST(VideoQoeAttribution, BothBadShortIsTransient) {
    auto r = attributeVideoQoe(badFrames(), netBad(1000, 1));

    EXPECT_EQ(r.attribution, VideoQoeAttribution::NETWORK_TRANSIENT);
    EXPECT_EQ(r.reason, "frames_bad_network_bad_transient");
}

TEST(VideoQoeAttribution, BothBadSustainedIsConfirmedLinkFault) {
    auto r = attributeVideoQoe(badFrames(), netBad(1000, 3));

    EXPECT_EQ(r.attribution, VideoQoeAttribution::CONFIRMED_LINK_FAULT);
    EXPECT_EQ(r.reason, "frames_bad_network_bad_sustained");
}

TEST(VideoQoeAttribution, TransientAndConfirmedDifferOnlyBySustainedness) {
    // 这两行的分界线只有持续性。若实现里写成两套不同的判定，会在某天
    // 悄悄改变含义而不被任何测试发现。
    auto transient = attributeVideoQoe(badFrames(), netBad(1000, 2));
    auto confirmed = attributeVideoQoe(badFrames(), netBad(1000, 3));

    EXPECT_EQ(transient.attribution, VideoQoeAttribution::NETWORK_TRANSIENT);
    EXPECT_EQ(confirmed.attribution, VideoQoeAttribution::CONFIRMED_LINK_FAULT);
    // 两侧都判为画面差，唯一变量就是窗口数
    EXPECT_EQ(transient.frames_bad, confirmed.frames_bad);
}

TEST(VideoQoeAttribution, ConfirmThresholdIsConfigurable) {
    VideoQoeThresholds t;
    t.confirm_windows = 5;

    auto r = attributeVideoQoe(badFrames(), netBad(1000, 3), t);

    EXPECT_EQ(r.attribution, VideoQoeAttribution::NETWORK_TRANSIENT)
        << "阈值调到 5 时，持续 3 窗口不算确认";
}

// ---------------------------------------------------------------------------
// 4. 证据条目
// ---------------------------------------------------------------------------

TEST(VideoQoeAttribution, EvidenceCarriesRawFacts) {
    auto r = attributeVideoQoe(badFrames(), netGood());

    ASSERT_FALSE(r.evidence.empty()) << "归因必须留下可追溯的原始事实";

    bool has_loss = false, has_p95 = false, has_gap = false;
    for (const auto& e : r.evidence) {
        if (e.metric == "video_frame_loss_ratio") has_loss = true;
        if (e.metric == "video_frame_gap_p95_ms") has_p95 = true;
        if (e.metric == "video_seq_gap_count") has_gap = true;
    }
    EXPECT_TRUE(has_loss);
    EXPECT_TRUE(has_p95);
    EXPECT_TRUE(has_gap);
}

TEST(VideoQoeAttribution, EvidenceLossRatioMatchesFrames) {
    FrameStats f = goodFrames();
    f.frames_received = 21;   // 丢 9 帧 / 30 = 0.3

    auto r = attributeVideoQoe(f, netGood());

    for (const auto& e : r.evidence) {
        if (e.metric == "video_frame_loss_ratio") {
            EXPECT_NEAR(e.value, 0.3, 1e-9);
            EXPECT_NE(e.detail.find("9"), std::string::npos)
                << "detail 应写出 丢/总，便于人工复核";
        }
    }
}

// ---------------------------------------------------------------------------
// 5. 画面侧判定：阈值语义
// ---------------------------------------------------------------------------

TEST(VideoQoeAttribution, FrameLossThresholdBoundaryIsAbsoluteFrameCount) {
    VideoQoeThresholds t;
    t.frame_loss_frames_bad = 3;   // 一个窗口内丢满 3 帧才算差

    FrameStats f = goodFrames();
    f.frames_received = 27;   // 丢 3 帧，正好等于阈值

    EXPECT_TRUE(isFrameQualityBad(f, t)) << "达到阈值即算差（>= 语义）";

    f.frames_received = 28;   // 丢 2 帧，低于阈值
    EXPECT_FALSE(isFrameQualityBad(f, t));
}

TEST(VideoQoeAttribution, FrameLossThresholdIsFrameRateIndependent) {
    // ========================================================================
    // 这条是「改用绝对帧数」的核心理由，必须独立钉住
    // ========================================================================
    //
    // 比例阈值会随分母变化：同样是丢 1 帧，18fps 是 5.6%、30fps 是 3.3%，
    // 用 5% 比例阈值会让低帧率设备「丢 1 帧就报警」，而高帧率设备丢 1 帧
    // 不报。本板实测只有 18.56fps（见 sensor-mode-table-fps-lie），
    // 这个偏差是**必现**的，不是理论风险。
    //
    // 绝对帧数阈值没有这个问题：丢 1 帧就是丢 1 帧，与帧率无关。
    VideoQoeThresholds t;
    t.frame_loss_frames_bad = 3;

    // 同为「丢 1 帧」，两种帧率必须给出**相同**结论
    FrameStats slow;                       // 18fps 的板子
    slow.frames_expected = 18;
    slow.frames_received = 17;             // 丢 1 帧 = 5.6%
    slow.window_ms = 1000;
    slow.sampled_at = std::chrono::steady_clock::now();

    FrameStats fast;                       // 30fps 的设备
    fast.frames_expected = 30;
    fast.frames_received = 29;             // 丢 1 帧 = 3.3%
    fast.window_ms = 1000;
    fast.sampled_at = std::chrono::steady_clock::now();

    EXPECT_FALSE(isFrameQualityBad(slow, t))
        << "18fps 丢 1 帧（5.6%）不应因比例高就判差";
    EXPECT_FALSE(isFrameQualityBad(fast, t))
        << "30fps 丢 1 帧（3.3%）同样不判差";
    EXPECT_EQ(isFrameQualityBad(slow, t), isFrameQualityBad(fast, t))
        << "同一「丢 1 帧」事实在不同帧率下必须得到相同结论——"
           "这正是改用绝对帧数的目的";

    // 反向确认：丢满 3 帧时两者都应判差（绝对数阈值不看分母）
    slow.frames_received = 15;
    fast.frames_received = 27;
    EXPECT_TRUE(isFrameQualityBad(slow, t));
    EXPECT_TRUE(isFrameQualityBad(fast, t));
}

TEST(VideoQoeAttribution, FrameLossFallsBackToRatioWhenWindowUnknown) {
    // ========================================================================
    // frames_expected == 0 时的退化路径
    // ========================================================================
    //
    // 绝对帧数阈值的判据是 `loss >= frame_loss_frames_bad`。当
    // frames_expected == 0（窗口异常 / 造数据不全）时 framesLost() 恒为 0，
    // 于是永远不会判差。
    //
    // 这是**正确**的：没有分母就没有可信的丢帧事实，此时报「画面好」是
    // 缺数据伪装健康，报「画面差」是编造事实。两者都错，所以只报**没有丢帧项**
    // ——若其它判据（gap p95 等）也不满足，整体就是「未明确地差」。
    // 换句话说：这条路径下 FRAME_LOSS 缺席，但结论仍由其它三条判据决定。
    VideoQoeThresholds t;
    t.frame_loss_frames_bad = 3;

    FrameStats f = goodFrames();
    f.frames_expected = 0;    // 无分母
    f.frames_received = 0;

    EXPECT_FALSE(isFrameQualityBad(f, t))
        << "无 expected 时不得凭空判成丢帧；FRAME_LOSS 只是不参与判定";

    // 但其它判据仍然生效：gap 抖动不能因为「没分母」就被吞掉
    f.p95_gap_ms = 500;
    EXPECT_TRUE(isFrameQualityBad(f, t))
        << "丢帧项缺席不得连带吞掉帧间隔判据";
}

TEST(VideoQoeAttribution, P95GapAloneCanMakeFramesBad) {
    // 丢帧率为 0 但帧间隔抖得厉害，用户感知依然是卡
    FrameStats f = goodFrames();
    f.p95_gap_ms = 500;

    EXPECT_TRUE(isFrameQualityBad(f, VideoQoeThresholds{}));

    auto r = attributeVideoQoe(f, netGood());
    EXPECT_EQ(r.attribution, VideoQoeAttribution::APP_LAYER_SUSPECT)
        << "无丢帧但严重抖动，应归因应用/采集侧而非判为健康";
}

TEST(VideoQoeAttribution, CorruptFramesCount) {
    VideoQoeThresholds t;
    t.frame_corrupt_ratio_bad = 0.01;

    FrameStats f = goodFrames();
    f.frames_corrupt = 1;   // 1/30 ≈ 0.033 > 0.01

    EXPECT_TRUE(isFrameQualityBad(f, t));
}

TEST(VideoQoeAttribution, InvalidFramesAreNotCalledBad) {
    // 「不知道」既不是好也不是差，必须由调用方单独呈现
    EXPECT_FALSE(isFrameQualityBad(FrameStats{}, VideoQoeThresholds{}));
}

// ---------------------------------------------------------------------------
// 6. 纯函数性质：同一输入必然同一输出
// ---------------------------------------------------------------------------

TEST(VideoQoeAttribution, IsDeterministic) {
    auto a = attributeVideoQoe(badFrames(), netBad(1000, 2));
    auto b = attributeVideoQoe(badFrames(), netBad(1000, 2));

    EXPECT_EQ(a.attribution, b.attribution);
    EXPECT_EQ(a.reason, b.reason);
    EXPECT_EQ(a.explanation, b.explanation);
    EXPECT_EQ(a.evaluated, b.evaluated);
    EXPECT_EQ(a.evidence.size(), b.evidence.size());
}

TEST(VideoQoeAttribution, DoesNotMutateInputs) {
    // 纯函数不得持有或改写输入（防止将来有人"顺手"累加计数器）
    FrameStats f = badFrames();
    VideoNetworkObservation n = netBad(1000, 2);
    const auto f0 = f;
    const auto n0 = n;

    attributeVideoQoe(f, n);

    EXPECT_EQ(f.frames_received, f0.frames_received);
    EXPECT_EQ(f.seq_gap_count, f0.seq_gap_count);
    EXPECT_EQ(f.window_ms, f0.window_ms);
    EXPECT_EQ(n.consecutive_degraded_windows, n0.consecutive_degraded_windows);
    EXPECT_EQ(n.degraded, n0.degraded);
}

// ---------------------------------------------------------------------------
// 6.5 问题清单（issues）：机器可读的成因标签
//
// 这三个字段的形态与 thermal 同构：`<x>_attribution` 是主结论，
// `<x>_issues` 是「同一轮里还发现了哪些具体问题」。
//
// 此前 cases 缺失的后果不是"少一个字段"，而是**孤儿字段**：
// NetworkExperience 有它、legacy_adapter 序列化它、w4 测试赋值它，
// 但 server.cpp 从不赋值 —— 代码算了，运维永远读到空数组。
// ---------------------------------------------------------------------------

TEST(VideoQoeAttribution, IssuesIsEmptyWhenFramesAreGood) {
    auto r = attributeVideoQoe(goodFrames(), netGood());

    EXPECT_EQ(r.attribution, VideoQoeAttribution::CONSISTENT_HEALTHY);
    EXPECT_TRUE(r.issues.empty())
        << "画面与网络都正常时不得产生问题条目——每轮刷一条会变成噪音";
}

TEST(VideoQoeAttribution, IssuesReportEachBadAspectSeparately) {
    // 丢帧 + 卡顿 + 坏帧三种劣化同时发生，三条都要如实列出。
    // 只报主结论会丢掉"画面为什么差"的具体线索。
    FrameStats f = goodFrames();
    f.frames_received = 15;   // 丢 15 帧 >= 阈值 3 帧
    f.p95_gap_ms = 500;       // > 100ms
    f.frames_corrupt = 3;     // 3/30 = 10% > 1%

    auto r = attributeVideoQoe(f, netGood());

    ASSERT_EQ(r.attribution, VideoQoeAttribution::APP_LAYER_SUSPECT);
    ASSERT_EQ(r.issues.size(), 3u) << "三种劣化应各报一条";

    std::set<std::string> got(r.issues.begin(), r.issues.end());
    EXPECT_TRUE(got.count("FRAME_LOSS")) << "丢帧数超阈值必须报";
    EXPECT_TRUE(got.count("FRAME_GAP_P95")) << "帧间隔 P95 超阈值必须报";
    EXPECT_TRUE(got.count("CORRUPT_FRAMES")) << "坏帧率超阈值必须报";
}

TEST(VideoQoeAttribution, IssuesOnlyReportsWhatActuallyExceeded) {
    // 只有丢帧超标，不得顺手把卡顿与坏帧也报上。
    // 误报会让现场去查一个根本不存在的问题。
    FrameStats f = goodFrames();
    f.frames_received = 15;

    auto r = attributeVideoQoe(f, netGood());

    ASSERT_EQ(r.issues.size(), 1u);
    EXPECT_EQ(r.issues[0], "FRAME_LOSS");
}

TEST(VideoQoeAttribution, IssuesIsEmptyWhenRefusingToAttribute) {
    // 拒绝归因时没有采到任何帧，"问题清单"必须是空的——
    // 把"不知道"写成"有问题"与把"不知道"写成"健康"同样有害。
    auto r = attributeVideoQoe(FrameStats{}, netGood());

    EXPECT_EQ(r.attribution, VideoQoeAttribution::NONE);
    EXPECT_TRUE(r.issues.empty()) << "没有帧统计时不得编造成因";
}

TEST(VideoQoeAttribution, IssuesDoNotDependOnNetworkSide) {
    // 网络差但画面好 → 问题在观测方法，不在帧质量。
    // 此时 issues 必须为空：没有任何帧级劣化可报。
    auto r = attributeVideoQoe(goodFrames(), netBad(1000, 5));

    EXPECT_EQ(r.attribution, VideoQoeAttribution::NETWORK_UNVERIFIED);
    EXPECT_TRUE(r.issues.empty())
        << "画面侧没问题时不报帧级 issue，网络侧结论由 attribution 表达";
}

// ---------------------------------------------------------------------------
// 6.6 出队延迟：把「采集端慢」与「我们自己慢」拆开
//
// `p95_dequeue_latency_ms` 如果没有消费者，它就只是一段自我声明的注释。
// 这一节守的是：它真的会改变归因输出（issues 与 evidence），而不只是被赋值。
// ---------------------------------------------------------------------------

TEST(VideoQoeAttribution, HighDequeueLatencyBecomesItsOwnIssue) {
    // 帧间隔正常但出队延迟很高 = 用户态 DQBUF 循环被拖慢。
    // 这是唯一一种「画面卡但采集端没错」的形态，必须能单独报出来。
    //
    // 若没有这条判据，这类故障会被归到 APP_LAYER_SUSPECT，而该结论的解释
    // 是「编码器 / 内存 / CPU / sensor / ISP」——把查自己进程的线索
    // 混在了一堆硬件选项里。
    FrameStats f = goodFrames();
    f.p95_dequeue_latency_ms = 400;   // > 默认阈值 200ms
    // p95_gap_ms 保持 33ms（正常）——正是这种组合才说明问题在用户态

    auto r = attributeVideoQoe(f, netGood());

    ASSERT_EQ(r.attribution, VideoQoeAttribution::APP_LAYER_SUSPECT);
    const std::set<std::string> got(r.issues.begin(), r.issues.end());
    EXPECT_TRUE(got.count("DEQUEUE_LATENCY"))
        << "出队延迟超标必须单独报一条成因";
    EXPECT_FALSE(got.count("FRAME_GAP_P95"))
        << "帧间隔正常时不得顺带报卡顿，否则归因方向就错了";
}

TEST(VideoQoeAttribution, DequeueLatencyAndGapAreDistinctIssues) {
    // 帧间隔高 + 延迟高 = 缓冲区不足。两条都要报，因为它们各自对应
    // 不同的处置动作（加 buffer vs 加快消费）。
    FrameStats f = goodFrames();
    f.p95_gap_ms = 500;
    f.p95_dequeue_latency_ms = 400;
    f.frames_received = 15;   // 顺带丢 15 帧（>= 阈值 3）

    auto r = attributeVideoQoe(f, netGood());

    const std::set<std::string> got(r.issues.begin(), r.issues.end());
    EXPECT_TRUE(got.count("FRAME_GAP_P95"));
    EXPECT_TRUE(got.count("DEQUEUE_LATENCY"));
    EXPECT_TRUE(got.count("FRAME_LOSS"));
}

TEST(VideoQoeAttribution, ZeroDequeueLatencyIsNotReportedAsBad) {
    // 0 表示「没采到」（合成源从不设这个字段），不是「延迟为零」。
    // 阈值配成 0 时尤其危险：`>=` 会把每个不设该字段的源都判成有延迟，
    // 于是所有合成源测试与无摄像头板子都会凭空多出一条成因。
    FrameStats f = goodFrames();
    f.p95_dequeue_latency_ms = 0;

    VideoQoeThresholds t;
    t.dequeue_latency_p95_bad_ms = 0;   // 极端配置

    auto r = attributeVideoQoe(f, netGood(), t);

    const std::set<std::string> got(r.issues.begin(), r.issues.end());
    EXPECT_FALSE(got.count("DEQUEUE_LATENCY"))
        << "0 是「未采样」的哨兵值，不得当作「延迟为零」参与比较";
    EXPECT_EQ(r.attribution, VideoQoeAttribution::CONSISTENT_HEALTHY);
}

TEST(VideoQoeAttribution, DequeueLatencyAppearsInEvidence) {
    // 原始值必须可追溯，否则运维只看到「有延迟」却不知道多大。
    // detail 里带上帧间隔 P95 作对照——两个值一起看才有归因意义。
    FrameStats f = goodFrames();
    f.p95_gap_ms = 120;
    f.p95_dequeue_latency_ms = 350;

    auto r = attributeVideoQoe(f, netGood());

    bool found = false;
    for (const auto& e : r.evidence) {
        if (e.metric == "video_dequeue_latency_p95_ms") {
            found = true;
            EXPECT_DOUBLE_EQ(e.value, 350.0) << "证据必须携带原始毫秒值";
            EXPECT_NE(e.detail.find("120"), std::string::npos)
                << "detail 应带上帧间隔 P95 作对照，单独一个延迟值无法归因";
        }
    }
    EXPECT_TRUE(found) << "出队延迟必须进 evidence，否则无法事后复核";
}

// ---------------------------------------------------------------------------
// 7. 枚举与字符串一致（证据/D-Bus 序列化的稳定契约）
// ---------------------------------------------------------------------------

TEST(VideoQoeAttribution, AllEnumeratorsHaveStringForms) {
    const VideoQoeAttribution all[] = {
        VideoQoeAttribution::NONE,
        VideoQoeAttribution::CONSISTENT_HEALTHY,
        VideoQoeAttribution::APP_LAYER_SUSPECT,
        VideoQoeAttribution::NETWORK_TRANSIENT,
        VideoQoeAttribution::CONFIRMED_LINK_FAULT,
        VideoQoeAttribution::NETWORK_UNVERIFIED,
    };

    std::set<std::string> names;
    for (auto a : all) {
        const std::string s = videoQoeAttributionToString(a);
        EXPECT_FALSE(s.empty());
        EXPECT_EQ(s.rfind("NONE", 0) == 0 && a != VideoQoeAttribution::NONE, false)
            << "只有 NONE 本身可以返回 NONE";
        names.insert(s);
    }
    EXPECT_EQ(names.size(), 6u) << "六个枚举必须映射到六个不同字符串";
    EXPECT_EQ(videoQoeAttributionToString(VideoQoeAttribution::CONSISTENT_HEALTHY),
              "CONSISTENT_HEALTHY");
}

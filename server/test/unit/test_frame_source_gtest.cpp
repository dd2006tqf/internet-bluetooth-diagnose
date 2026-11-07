/**
 * @file test_frame_source_gtest.cpp
 * @brief 帧源接口与合成源的契约真值表
 *
 * 这份测试守的是「L2/L3 层的语义边界」，而不是「实现恰好算出什么数」。
 * 每条断言背后都有一个会导致归因误判的失效模式：
 *
 *   1. 未启动时不得返回"健康"的全零统计 —— 缺数据伪装成好数据
 *   2. 乱序不得计入丢帧 —— 驱动行为被误判成网络故障
 *   3. 一次断流与多次小丢帧必须可区分 —— "卡住" 与 "断续" 的用户感知不同
 *   4. snapshotAndReset 必须真清零 —— 否则丢帧率被重复计算
 *   5. describe() 必须标明来源 —— 合成数据不能被当成真机证据
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <memory>
#include <set>
#include <string>

#include "frame_source.hpp"

using namespace weaknet;

// ---------------------------------------------------------------------------
// 1. 生命周期与缺数据语义
// ---------------------------------------------------------------------------

TEST(FrameStats, DefaultConstructedIsInvalidNotHealthy) {
    // 这是整份文件最重要的一条断言。
    //
    // FrameStats 的默认值全是 0，而 frameLossRatio() 对 expected==0 返回 0.0。
    // 也就是说「从未采样」看起来和「完美运行」完全一样。只有 valid() 能区分。
    // 任何下游代码若不先检查 valid()，就会把「不知道」当成「很好」。
    FrameStats s;

    EXPECT_FALSE(s.valid());
    EXPECT_EQ(s.frames_expected, 0u);
    EXPECT_DOUBLE_EQ(s.frameLossRatio(), 0.0);
    // 显式钉死这个陷阱的存在，防止有人"顺手"删掉 valid()
    EXPECT_EQ(s.framesLost(), 0u);
}

TEST(SyntheticFrameSource, SnapshotBeforeStartIsInvalid) {
    SyntheticFrameSource src(SyntheticPattern::HEALTHY, 30, 1000);

    auto s = src.snapshotAndReset();

    EXPECT_FALSE(s.valid()) << "未 start 就返回有效统计 = 伪造数据";
    EXPECT_EQ(s.frames_expected, 0u);
}

TEST(SyntheticFrameSource, StartThenStopIsIdempotent) {
    SyntheticFrameSource src(SyntheticPattern::HEALTHY, 30, 1000);

    // 未 start 直接 stop 不得崩溃（接口契约要求可重入）
    src.stop();

    ASSERT_TRUE(src.start());
    auto a = src.snapshotAndReset();
    EXPECT_TRUE(a.valid());

    src.stop();
    src.stop();   // 重复 stop

    // stop 之后再取，必须回到 invalid
    auto b = src.snapshotAndReset();
    EXPECT_FALSE(b.valid());
}

TEST(SyntheticFrameSource, RestartResetsTimeline) {
    SyntheticFrameSource src(SyntheticPattern::PERIODIC_LOSS, 30, 1000);

    src.start();
    for (int i = 0; i < 5; ++i) src.snapshotAndReset();
    EXPECT_EQ(src.windowCount(), 5u);

    src.stop();
    src.start();
    EXPECT_EQ(src.windowCount(), 0u) << "重启必须重置窗口计数，否则模式相位错乱";
    EXPECT_EQ(src.nextSeq(), 0u);
}

// ---------------------------------------------------------------------------
// 2. HEALTHY：无丢帧、无抖动
// ---------------------------------------------------------------------------

TEST(SyntheticFrameSource, HealthyHasNoLossAndNoJitter) {
    SyntheticFrameSource src(SyntheticPattern::HEALTHY, 30, 1000);

    ASSERT_TRUE(src.start());
    auto s = src.snapshotAndReset();

    EXPECT_TRUE(s.valid());
    EXPECT_EQ(s.frames_expected, 30u) << "30fps × 1000ms 应为 30 帧";
    EXPECT_EQ(s.frames_received, 30u);
    EXPECT_EQ(s.seq_gap_count, 0u);
    EXPECT_EQ(s.frames_corrupt, 0u);
    EXPECT_DOUBLE_EQ(s.frameLossRatio(), 0.0);

    // 30fps → 每帧 33.33ms，取整后最大间隔也是 33
    EXPECT_EQ(s.max_gap_ms, 33u);
    EXPECT_LE(s.p95_gap_ms, s.max_gap_ms);
    EXPECT_GT(s.bytes_received, 0u);
}

TEST(SyntheticFrameSource, HealthyIsDeterministic) {
    // 同一组参数必须得到同一份结果（不含时间戳）。
    // 不 sleep 的设计价值就在这里：CI 上不会因调度抖动而偶发失败。
    SyntheticFrameSource a(SyntheticPattern::HEALTHY, 30, 1000);
    SyntheticFrameSource b(SyntheticPattern::HEALTHY, 30, 1000);

    a.start();
    b.start();

    for (int i = 0; i < 3; ++i) {
        auto sa = a.snapshotAndReset();
        auto sb = b.snapshotAndReset();
        EXPECT_EQ(sa.frames_expected, sb.frames_expected) << "round " << i;
        EXPECT_EQ(sa.frames_received, sb.frames_received) << "round " << i;
        EXPECT_EQ(sa.seq_gap_count, sb.seq_gap_count) << "round " << i;
        EXPECT_EQ(sa.max_gap_ms, sb.max_gap_ms) << "round " << i;
        EXPECT_EQ(sa.p95_gap_ms, sb.p95_gap_ms) << "round " << i;
    }
}

// ---------------------------------------------------------------------------
// 3. PERIODIC_LOSS：少量但频繁
// ---------------------------------------------------------------------------

TEST(SyntheticFrameSource, PeriodicLossReportsEveryGapEvent) {
    SyntheticFrameSource src(SyntheticPattern::PERIODIC_LOSS, 30, 1000);

    ASSERT_TRUE(src.start());
    auto s = src.snapshotAndReset();

    EXPECT_TRUE(s.valid());
    // 30 帧里每 30 帧丢 2 帧 → 本窗口恰好 1 次周期命中，丢 2 帧
    EXPECT_EQ(s.seq_gap_count, 2u) << "2 帧各算一次序号跳变";
    EXPECT_EQ(s.frames_received, 28u);
    EXPECT_EQ(s.frames_expected, 30u);
    EXPECT_DOUBLE_EQ(s.frameLossRatio(), 2.0 / 30.0);
}

TEST(SyntheticFrameSource, PeriodicLossLossRateIsStableAcrossWindows) {
    // 周期性丢帧的定义就是"每窗口都有、比例稳定"，
    // 若某窗口丢 0 帧说明相位算错了。
    SyntheticFrameSource src(SyntheticPattern::PERIODIC_LOSS, 30, 1000);
    src.start();

    std::set<uint64_t> received_values;
    for (int i = 0; i < 10; ++i) {
        auto s = src.snapshotAndReset();
        received_values.insert(s.frames_received);
        EXPECT_LT(s.frames_received, s.frames_expected) << "round " << i << " 不该有完好窗口";
    }
    // 相位随窗口推进，收到的帧数允许有 1~2 帧波动，但收敛在 28 附近
    EXPECT_LE(received_values.size(), 3u);
}

// ---------------------------------------------------------------------------
// 4. BURST_LOSS：一次断流 vs 多次小丢帧
// ---------------------------------------------------------------------------

TEST(SyntheticFrameSource, BurstLossIsOneEventNotMany) {
    // 关键语义：一次丢 9 帧记 1 次事件，而不是 9 次。
    // 这决定了归因层能否区分"画面卡住了"和"画面在断续"。
    SyntheticFrameSource src(SyntheticPattern::BURST_LOSS, 30, 1000);
    src.start();

    auto even = src.snapshotAndReset();   // 窗口 0：正常
    auto odd  = src.snapshotAndReset();   // 窗口 1：突发丢帧

    EXPECT_EQ(even.seq_gap_count, 0u);
    EXPECT_EQ(even.frames_received, even.frames_expected);

    EXPECT_LT(odd.frames_received, odd.frames_expected);
    EXPECT_EQ(odd.seq_gap_count, 1u) << "连续丢一段只算 1 次中断事件";
    EXPECT_EQ(odd.frames_received, 21u) << "30 帧丢 30% 应剩 21 帧";
}

TEST(SyntheticFrameSource, BurstLossProducesLongGap) {
    SyntheticFrameSource src(SyntheticPattern::BURST_LOSS, 30, 1000);
    src.start();
    src.snapshotAndReset();   // 跳过正常窗口

    auto burst = src.snapshotAndReset();

    // 断流窗口必须出现一个显著拉长的间隔（远大于 33ms 的标称间隔）
    EXPECT_GT(burst.max_gap_ms, 100u)
        << "突发断流必须体现在 max_gap_ms 上，否则与周期丢帧无法区分";
}

TEST(SyntheticFrameSource, BurstLossAlternates) {
    SyntheticFrameSource src(SyntheticPattern::BURST_LOSS, 30, 1000);
    src.start();

    for (int i = 0; i < 6; ++i) {
        auto s = src.snapshotAndReset();
        if (i % 2 == 0) {
            EXPECT_EQ(s.seq_gap_count, 0u) << "窗口 " << i << " 应为正常";
        } else {
            EXPECT_EQ(s.seq_gap_count, 1u) << "窗口 " << i << " 应为一次突发";
        }
    }
}

// ---------------------------------------------------------------------------
// 5. REORDERED：乱序不是丢帧（最容易写错的一条）
// ---------------------------------------------------------------------------

TEST(SyntheticFrameSource, ReorderedIsNotCountedAsLoss) {
    // 这条断言守住归因正确性：
    // 驱动重排是本地行为，若计入丢帧，归因层会把"驱动问题"报成"网络故障"。
    SyntheticFrameSource src(SyntheticPattern::REORDERED, 30, 1000);

    ASSERT_TRUE(src.start());
    auto s = src.snapshotAndReset();

    EXPECT_TRUE(s.valid());
    EXPECT_EQ(s.seq_gap_count, 0u) << "乱序不得计入丢帧事件";
    EXPECT_EQ(s.frames_received, s.frames_expected) << "帧全都到了";
    EXPECT_DOUBLE_EQ(s.frameLossRatio(), 0.0);

    // 间隔也应正常：乱序是顺序问题，不是时间问题
    EXPECT_EQ(s.max_gap_ms, 33u);
}

TEST(SyntheticFrameSource, ReorderedDiffersFromPeriodicLoss) {
    // 反向确认两种模式真的有区别（防止实现里两个分支写成一样）
    SyntheticFrameSource reordered(SyntheticPattern::REORDERED, 30, 1000);
    SyntheticFrameSource periodic(SyntheticPattern::PERIODIC_LOSS, 30, 1000);

    reordered.start();
    periodic.start();

    auto a = reordered.snapshotAndReset();
    auto b = periodic.snapshotAndReset();

    EXPECT_EQ(a.seq_gap_count, 0u);
    EXPECT_GT(b.seq_gap_count, 0u);
    EXPECT_NE(reordered.describe(), periodic.describe());
}

// ---------------------------------------------------------------------------
// 6. snapshotAndReset 的「取走并清零」语义
// ---------------------------------------------------------------------------

TEST(SyntheticFrameSource, SnapshotAdvancesTimeline) {
    SyntheticFrameSource src(SyntheticPattern::HEALTHY, 30, 1000);
    src.start();

    EXPECT_EQ(src.windowCount(), 0u);
    EXPECT_EQ(src.nextSeq(), 0u);

    src.snapshotAndReset();
    EXPECT_EQ(src.windowCount(), 1u);
    EXPECT_EQ(src.nextSeq(), 30u);

    src.snapshotAndReset();
    EXPECT_EQ(src.windowCount(), 2u);
    EXPECT_EQ(src.nextSeq(), 60u);
}

TEST(SyntheticFrameSource, EachSnapshotCoversItsOwnWindowOnly) {
    // 若实现返回累计值而非增量，连续两次快照会把同一批帧算两遍，
    // 导致丢帧率虚高。这里用 BURST 的交替性来检测。
    SyntheticFrameSource src(SyntheticPattern::BURST_LOSS, 30, 1000);
    src.start();

    auto first = src.snapshotAndReset();
    auto second = src.snapshotAndReset();
    auto third = src.snapshotAndReset();

    // 每个窗口的 frames_expected 都应该是独立的 30，不累加
    EXPECT_EQ(first.frames_expected, 30u);
    EXPECT_EQ(second.frames_expected, 30u);
    EXPECT_EQ(third.frames_expected, 30u);

    // 交替模式：0 正常、1 突发、2 正常
    EXPECT_EQ(first.seq_gap_count, 0u);
    EXPECT_EQ(second.seq_gap_count, 1u);
    EXPECT_EQ(third.seq_gap_count, 0u);
}

// ---------------------------------------------------------------------------
// 7. describe()：evidence 溯源
// ---------------------------------------------------------------------------

TEST(SyntheticFrameSource, DescribeMarksItAsSynthetic) {
    // 前缀必须是 synthetic: —— 事后审证据时要能一眼区分
    // "真机观测" 与 "注入的测试数据"。这是本项目 evidence 纪律的一部分。
    SyntheticFrameSource src(SyntheticPattern::BURST_LOSS, 25, 500);

    const std::string d = src.describe();

    EXPECT_EQ(d.rfind("synthetic:", 0), 0) << "实际: " << d;
    EXPECT_NE(d.find("BURST_LOSS"), std::string::npos) << "实际: " << d;
    EXPECT_NE(d.find("25"), std::string::npos) << "应含帧率，实际: " << d;
    EXPECT_NE(d.find("500"), std::string::npos) << "应含窗口时长，实际: " << d;
}

TEST(SyntheticFrameSource, DescribeCoversAllPatterns) {
    struct Case { SyntheticPattern p; const char* name; };
    const Case cases[] = {
        {SyntheticPattern::HEALTHY,       "HEALTHY"},
        {SyntheticPattern::PERIODIC_LOSS, "PERIODIC_LOSS"},
        {SyntheticPattern::BURST_LOSS,    "BURST_LOSS"},
        {SyntheticPattern::REORDERED,     "REORDERED"},
    };

    for (const auto& c : cases) {
        SyntheticFrameSource src(c.p, 30, 1000);
        EXPECT_NE(src.describe().find(c.name), std::string::npos)
            << "pattern " << c.name << " 的描述缺失";
    }
}

// ---------------------------------------------------------------------------
// 8. 参数边界：非法输入不得崩溃或产生 NaN
// ---------------------------------------------------------------------------

TEST(SyntheticFrameSource, ZeroFpsDoesNotDivideByZero) {
    // 构造时 fps=0，实现内部钳到 30。若直接拿 0 做除数，
    // frame_interval_ms 会变成 inf，进而把 nominal_frames 转成 0 或 UB。
    SyntheticFrameSource src(SyntheticPattern::HEALTHY, 0, 1000);

    ASSERT_TRUE(src.start());
    auto s = src.snapshotAndReset();

    EXPECT_TRUE(s.valid());
    EXPECT_GT(s.frames_expected, 0u) << "不应因非法 fps 退化成 0 帧";
    EXPECT_FALSE(std::isnan(s.frameLossRatio()));
}

TEST(SyntheticFrameSource, ZeroWindowDoesNotProduceEmptyWindow) {
    SyntheticFrameSource src(SyntheticPattern::HEALTHY, 30, 0);

    ASSERT_TRUE(src.start());
    auto s = src.snapshotAndReset();

    EXPECT_TRUE(s.valid());
    EXPECT_GT(s.frames_expected, 0u) << "窗口时长为 0 时钳到 1000ms";
}

TEST(FrameStats, LossRatioIsClampedToZeroWhenReceivedExceedsExpected) {
    // 期望值配小了（如配置错误）时，framesLost() 不能返回负数，
    // 否则丢帧率会变成负值，下游阈值判断全部失效。
    FrameStats s;
    s.frames_expected = 10;
    s.frames_received = 15;

    EXPECT_EQ(s.framesLost(), 0u);
    EXPECT_DOUBLE_EQ(s.frameLossRatio(), 0.0);
}

TEST(FrameStats, ValidRequiresSampledAt) {
    FrameStats s;
    s.frames_expected = 100;
    s.frames_received = 100;
    // 数据都填好了但没时间戳 —— 仍然不算有效采样。
    // 时间戳是"这次采样真的发生过"的唯一凭证。
    EXPECT_FALSE(s.valid());

    s.sampled_at = std::chrono::steady_clock::now();
    EXPECT_TRUE(s.valid());
}

// ---------------------------------------------------------------------------
// 9. 多态性：IFrameSource 指针可承载两种实现
// ---------------------------------------------------------------------------

TEST(IFrameSource, UsableThroughBasePointer) {
    // 这条测试的存在意义是锁定接口设计目标：上层只依赖 IFrameSource，
    // 将来加 V4l2FrameSource 时不需要改动任何调用方代码。
    std::unique_ptr<IFrameSource> src =
        std::make_unique<SyntheticFrameSource>(SyntheticPattern::HEALTHY, 30, 1000);

    ASSERT_TRUE(src->start());
    auto s = src->snapshotAndReset();
    EXPECT_TRUE(s.valid());
    EXPECT_FALSE(src->describe().empty());
    src->stop();
}

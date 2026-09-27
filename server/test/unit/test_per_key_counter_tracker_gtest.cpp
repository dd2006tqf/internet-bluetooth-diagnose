/**
 * @file test_per_key_counter_tracker_gtest.cpp
 * @brief PerKeyCounterTracker 差分分类单元测试
 *
 * 覆盖 new / grew / disappeared / reset 四类分类、累计基线不变式，
 * 以及 eviction_limited 的水位与消失量判据。
 *
 * 不依赖 libbpf：纯用户态逻辑，可在 x86 直接运行。
 */

#include <gtest/gtest.h>

#include "utils/per_key_counter_tracker.hpp"

#include <string>
#include <utility>
#include <vector>

using namespace weaknet;

namespace {

using Snapshot = std::vector<std::pair<std::string, uint64_t>>;

} // namespace

// ---------------------------------------------------------------------------
// 首轮与 baseline
// ---------------------------------------------------------------------------

TEST(PerKeyCounterTrackerTest, FirstRoundCountsAllAsNew) {
    PerKeyCounterTracker tracker;
    Snapshot snap{{"a", 10}, {"b", 20}};

    PerKeyStats st = tracker.update(snap, 1000);
    EXPECT_EQ(st.new_keys, 2u);
    EXPECT_EQ(st.disappeared_keys, 0u);
    EXPECT_EQ(st.reset_keys, 0u);
    EXPECT_EQ(st.entries, 2u);
    EXPECT_EQ(tracker.baselineSize(), 2u);
}

TEST(PerKeyCounterTrackerTest, EmptyFirstRoundIsStable) {
    PerKeyCounterTracker tracker;
    PerKeyStats st = tracker.update(Snapshot{}, 1000);
    EXPECT_EQ(st.entries, 0u);
    EXPECT_EQ(st.new_keys, 0u);
    EXPECT_EQ(tracker.baselineSize(), 0u);
}

// ---------------------------------------------------------------------------
// 四类分类
// ---------------------------------------------------------------------------

TEST(PerKeyCounterTrackerTest, GrowsAreNeitherNewNorReset) {
    PerKeyCounterTracker tracker;
    ASSERT_EQ(tracker.update(Snapshot{{"a", 10}, {"b", 20}}, 1000).new_keys, 2u);

    // 两键均正常增长：既不是 new，也不是 reset
    PerKeyStats st = tracker.update(Snapshot{{"a", 30}, {"b", 25}}, 1000);
    EXPECT_EQ(st.new_keys, 0u);
    EXPECT_EQ(st.reset_keys, 0u);
    EXPECT_EQ(st.disappeared_keys, 0u);
    EXPECT_EQ(st.entries, 2u);
}

TEST(PerKeyCounterTrackerTest, NewKeyIsDetected) {
    PerKeyCounterTracker tracker;
    ASSERT_EQ(tracker.update(Snapshot{{"a", 10}}, 1000).new_keys, 1u);

    PerKeyStats st = tracker.update(Snapshot{{"a", 15}, {"b", 3}}, 1000);
    EXPECT_EQ(st.new_keys, 1u);   // 仅 b
    EXPECT_EQ(st.reset_keys, 0u);
    EXPECT_EQ(st.entries, 2u);
}

TEST(PerKeyCounterTrackerTest, DisappearedKeyIsDetected) {
    PerKeyCounterTracker tracker;
    ASSERT_EQ(tracker.update(Snapshot{{"a", 10}, {"b", 20}}, 1000).entries, 2u);

    PerKeyStats st = tracker.update(Snapshot{{"a", 15}}, 1000);
    EXPECT_EQ(st.disappeared_keys, 1u);  // b 消失
    EXPECT_EQ(st.new_keys, 0u);
    EXPECT_EQ(st.reset_keys, 0u);
    EXPECT_EQ(st.entries, 1u);
}

TEST(PerKeyCounterTrackerTest, CounterRegressionIsClassifiedAsReset) {
    // 这是本追踪器存在的根本理由：LRU 驱逐后 key 重建，计数从 0 重来。
    // 若把它当正常增长，delta 会算出巨大的负值或伪造尖峰。
    PerKeyCounterTracker tracker;
    ASSERT_EQ(tracker.update(Snapshot{{"a", 1000}}, 1000).new_keys, 1u);

    PerKeyStats st = tracker.update(Snapshot{{"a", 5}}, 1000);
    EXPECT_EQ(st.reset_keys, 1u);
    EXPECT_EQ(st.new_keys, 0u);        // key 仍在，不是 new
    EXPECT_EQ(st.disappeared_keys, 0u);
    EXPECT_EQ(st.entries, 1u);
}

TEST(PerKeyCounterTrackerTest, AllFourClassesInOneRound) {
    PerKeyCounterTracker tracker;
    ASSERT_EQ(tracker.update(Snapshot{{"grow", 10}, {"reset", 500}, {"gone", 7}}, 1000).entries, 3u);

    PerKeyStats st = tracker.update(
        Snapshot{{"grow", 40}, {"reset", 2}, {"fresh", 1}}, 1000);
    EXPECT_EQ(st.new_keys, 1u);          // fresh
    EXPECT_EQ(st.reset_keys, 1u);        // reset 倒退
    EXPECT_EQ(st.disappeared_keys, 1u);  // gone
    EXPECT_EQ(st.entries, 3u);
}

// ---------------------------------------------------------------------------
// 水位与 eviction_limited
// ---------------------------------------------------------------------------

TEST(PerKeyCounterTrackerTest, WatermarkIsComputedFromEntries) {
    PerKeyCounterTracker tracker;
    Snapshot snap;
    for (int i = 0; i < 50; ++i) snap.emplace_back("k" + std::to_string(i), 1);

    PerKeyStats st = tracker.update(snap, 100);
    EXPECT_EQ(st.entries, 50u);
    EXPECT_EQ(st.max_entries, 100u);
    EXPECT_NEAR(st.watermark_pct, 50.0, 0.01);
    EXPECT_FALSE(st.eviction_limited);
}

TEST(PerKeyCounterTrackerTest, UnknownCapacitySkipsWatermark) {
    PerKeyCounterTracker tracker;
    Snapshot snap{{"a", 1}, {"b", 2}};

    // max_entries=0 表示容量未知：不能凭空判定水位或驱逐受限
    PerKeyStats st = tracker.update(snap, 0);
    EXPECT_EQ(st.max_entries, 0u);
    EXPECT_DOUBLE_EQ(st.watermark_pct, 0.0);
    EXPECT_FALSE(st.eviction_limited);
}

TEST(PerKeyCounterTrackerTest, HighWatermarkAloneIsNotEvictionLimited) {
    // 水位高但没有任何 key 消失 → 只是用满，不是"因驱逐而失真"
    PerKeyCounterTracker tracker;
    Snapshot snap;
    for (int i = 0; i < 95; ++i) snap.emplace_back("k" + std::to_string(i), 1);
    ASSERT_EQ(tracker.update(snap, 100).entries, 95u);

    PerKeyStats st = tracker.update(snap, 100);
    EXPECT_GE(st.watermark_pct, kEvictionWatermarkPct);
    EXPECT_EQ(st.disappeared_keys, 0u);
    EXPECT_FALSE(st.eviction_limited);
}

TEST(PerKeyCounterTrackerTest, MassDisappearanceAtHighWatermarkTriggersEvictionLimited) {
    // 水位 ≥90% 且大量 key 消失：典型 LRU 饱和驱逐签名
    PerKeyCounterTracker tracker;
    Snapshot before;
    for (int i = 0; i < 200; ++i) before.emplace_back("old" + std::to_string(i), 1);
    tracker.update(before, 200);

    Snapshot after;
    for (int i = 0; i < 199; ++i) after.emplace_back("new" + std::to_string(i), 1);
    PerKeyStats st = tracker.update(after, 200);

    EXPECT_GE(st.watermark_pct, kEvictionWatermarkPct);
    EXPECT_GE(st.disappeared_keys, kEvictionDisappearThreshold);
    EXPECT_TRUE(st.eviction_limited);
}

TEST(PerKeyCounterTrackerTest, MassDisappearanceAtLowWatermarkIsNotEvictionLimited) {
    // 大量消失但水位低：更可能是连接正常结束，不是容量驱逐
    PerKeyCounterTracker tracker;
    Snapshot before;
    for (int i = 0; i < 200; ++i) before.emplace_back("old" + std::to_string(i), 1);
    tracker.update(before, 100000);

    PerKeyStats st = tracker.update(Snapshot{}, 100000);
    EXPECT_LT(st.watermark_pct, kEvictionWatermarkPct);
    EXPECT_GE(st.disappeared_keys, kEvictionDisappearThreshold);
    EXPECT_FALSE(st.eviction_limited);
}

// ---------------------------------------------------------------------------
// reset
// ---------------------------------------------------------------------------

TEST(PerKeyCounterTrackerTest, ResetClearsBaselineSoNextRoundIsAllNew) {
    PerKeyCounterTracker tracker;
    ASSERT_EQ(tracker.update(Snapshot{{"a", 10}, {"b", 20}}, 1000).new_keys, 2u);

    tracker.reset();
    EXPECT_EQ(tracker.baselineSize(), 0u);

    PerKeyStats st = tracker.update(Snapshot{{"a", 10}, {"b", 20}}, 1000);
    EXPECT_EQ(st.new_keys, 2u);
    EXPECT_EQ(st.reset_keys, 0u);  // 基线已清空，不判为倒退
}

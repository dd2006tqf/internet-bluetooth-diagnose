/**
 * @file test_bt_link_quality_gtest.cpp
 * @brief 蓝牙链路质量跟踪器单元测试（10 项严苛不变式测试）
 */

#include <gtest/gtest.h>
#include <memory>
#include "bt_link_quality_tracker.hpp"

using namespace weaknet_dbus;

namespace {

WirelessDeviceKey makeKey(const std::string& addr = "AA:BB:CC:DD:EE:01",
                          BtAddressType type = BtAddressType::LeRandom,
                          const std::string& gw = "gw-test") {
    WirelessDeviceKey k;
    k.site_id = "site-test";
    k.gateway_id = gw;
    k.hci_index = 0;
    k.protocol = WirelessProtocol::Bluetooth;
    k.address_type = type;
    k.device_address = addr;
    return k;
}

} // namespace

TEST(BtLinkQualityTrackerTest, LearningUntilMinSamples) {
    BtLinkQualityTracker tracker;
    auto key = makeKey();

    // 灌入 9 个样本（默认 min_baseline_samples = 10），状态仍在 LEARNING
    for (int i = 0; i < 9; ++i) {
        auto ev = tracker.feedFreshRssi(key, -60, 1000 + i * 1000, (1000 + i * 1000) * 1000000ULL);
        EXPECT_FALSE(ev.has_value());
    }

    auto p = tracker.getProfile(key);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->state, LinkQualityState::Learning);

    // 哪怕灌入一个 -95dBm 的坏样本，由于基线尚未就绪，绝不触发告警
    auto ev = tracker.feedFreshRssi(key, -95, 10000, 10000ULL * 1000000ULL);
    EXPECT_FALSE(ev.has_value());
}

TEST(BtLinkQualityTrackerTest, BaselineMedianCalculatedCorrectly) {
    BtLinkQualityTracker tracker;
    auto key = makeKey();

    // 灌入 10 个样本，刚好收敛至 STABLE，验证中位数
    // 样本: -50, -52, -54, -56, -58, -60, -62, -64, -66, -68
    // 排序后中间两个为 -58, -60，中位数取 -59
    std::vector<int16_t> samples = {-50, -52, -54, -56, -58, -60, -62, -64, -66, -68};
    for (size_t i = 0; i < samples.size(); ++i) {
        tracker.feedFreshRssi(key, samples[i], 1000 + i * 1000, (1000 + i * 1000) * 1000000ULL);
    }

    auto p = tracker.getProfile(key);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->state, LinkQualityState::Stable);
    ASSERT_TRUE(p->baseline_rssi_dbm.has_value());
    EXPECT_EQ(*p->baseline_rssi_dbm, -59);
}

TEST(BtLinkQualityTrackerTest, SingleAnomalyDoesNotTriggerDegraded) {
    BtLinkQualityTracker tracker;
    auto key = makeKey();

    for (int i = 0; i < 10; ++i) {
        tracker.feedFreshRssi(key, -60, 1000 + i * 1000, (1000 + i * 1000) * 1000000ULL);
    }

    // 单次出现 -85dBm（瞬时毛刺/叉车遮挡）
    auto ev = tracker.feedFreshRssi(key, -85, 11000, 11000ULL * 1000000ULL);
    EXPECT_FALSE(ev.has_value()) << "单次偶发毛刺不得触发 LINK_DEGRADED";

    auto p = tracker.getProfile(key);
    EXPECT_EQ(p->state, LinkQualityState::Stable);
}

TEST(BtLinkQualityTrackerTest, ConsecutiveBadSamplesTriggerDegraded) {
    BtLinkQualityTracker tracker;
    auto key = makeKey();

    for (int i = 0; i < 10; ++i) {
        tracker.feedFreshRssi(key, -60, 1000 + i * 1000, (1000 + i * 1000) * 1000000ULL);
    }

    // 连续 3 个 bad 样本（低于 -60 - 15 = -75dBm），且时间紧凑
    tracker.feedFreshRssi(key, -80, 11000, 11000ULL * 1000000ULL);
    tracker.feedFreshRssi(key, -81, 12000, 12000ULL * 1000000ULL);
    auto ev = tracker.feedFreshRssi(key, -82, 13000, 13000ULL * 1000000ULL);

    ASSERT_TRUE(ev.has_value());
    EXPECT_EQ(ev->event_type, DeviceEventType::LinkDegraded);
    EXPECT_EQ(ev->device_address, key.device_address);
    EXPECT_EQ(ev->source, EvidenceSource::Derived);
    EXPECT_FALSE(ev->suspected_cause.has_value()); // 采集层不做诊断
    EXPECT_NE(ev->details_json.find("\"baseline_rssi_dbm\":-60"), std::string::npos);

    auto p = tracker.getProfile(key);
    EXPECT_EQ(p->state, LinkQualityState::Degraded);
}

TEST(BtLinkQualityTrackerTest, BaselineFrozenDuringDegraded) {
    BtLinkQualityTracker tracker;
    auto key = makeKey();

    for (int i = 0; i < 10; ++i) {
        tracker.feedFreshRssi(key, -60, 1000 + i * 1000, (1000 + i * 1000) * 1000000ULL);
    }

    // 触发恶化
    tracker.feedFreshRssi(key, -80, 11000, 11000ULL * 1000000ULL);
    tracker.feedFreshRssi(key, -81, 12000, 12000ULL * 1000000ULL);
    tracker.feedFreshRssi(key, -82, 13000, 13000ULL * 1000000ULL);

    // 恶化期间灌入大量 -90dBm 的劣质样本
    for (int i = 0; i < 50; ++i) {
        tracker.feedFreshRssi(key, -90, 14000 + i * 1000, (14000 + i * 1000) * 1000000ULL);
    }

    auto p = tracker.getProfile(key);
    EXPECT_EQ(p->state, LinkQualityState::Degraded);
    ASSERT_TRUE(p->baseline_rssi_dbm.has_value());
    // 关键断言：基线绝不下沉！保持冻结时的 -60
    EXPECT_EQ(*p->baseline_rssi_dbm, -60) << "劣质样本严禁学入基线导致基线下沉！";
}

TEST(BtLinkQualityTrackerTest, SparseSamplesDoNotTrigger) {
    BtLinkQualityTracker tracker;
    auto key = makeKey();

    for (int i = 0; i < 10; ++i) {
        tracker.feedFreshRssi(key, -60, 1000 + i * 1000, (1000 + i * 1000) * 1000000ULL);
    }

    // 注入 3 个 bad 样本，但间隔高达 60 秒（超过 max_fresh_gap_ms = 15s）
    tracker.feedFreshRssi(key, -80, 20000, 20000ULL * 1000000ULL);
    tracker.feedFreshRssi(key, -81, 80000, 80000ULL * 1000000ULL);
    auto ev = tracker.feedFreshRssi(key, -82, 140000, 140000ULL * 1000000ULL);

    EXPECT_FALSE(ev.has_value()) << "时间稀疏跨度大的采样不得计入连续恶化！";
    auto p = tracker.getProfile(key);
    EXPECT_EQ(p->state, LinkQualityState::Stable);
}

TEST(BtLinkQualityTrackerTest, RecoveryHysteresisAndStreak) {
    BtLinkQualityTracker tracker;
    auto key = makeKey();

    for (int i = 0; i < 10; ++i) {
        tracker.feedFreshRssi(key, -60, 1000 + i * 1000, (1000 + i * 1000) * 1000000ULL);
    }
    // 触发恶化
    tracker.feedFreshRssi(key, -80, 11000, 11000ULL * 1000000ULL);
    tracker.feedFreshRssi(key, -81, 12000, 12000ULL * 1000000ULL);
    tracker.feedFreshRssi(key, -82, 13000, 13000ULL * 1000000ULL);

    // 处于滞回带（-69dBm，在 -15dB 与 -8dB 之间）：不触发恢复
    auto ev1 = tracker.feedFreshRssi(key, -69, 14000, 14000ULL * 1000000ULL);
    EXPECT_FALSE(ev1.has_value());

    // 出现 1 次良好信号（-62dBm > -68dBm）：单次好样本不触发恢复（时间防抖）
    auto ev2 = tracker.feedFreshRssi(key, -62, 15000, 15000ULL * 1000000ULL);
    EXPECT_FALSE(ev2.has_value());

    // 连续第 2 次好样本
    auto ev3 = tracker.feedFreshRssi(key, -61, 16000, 16000ULL * 1000000ULL);
    EXPECT_FALSE(ev3.has_value());

    // 连续第 3 次好样本：触发 LINK_RECOVERED
    auto ev4 = tracker.feedFreshRssi(key, -60, 17000, 17000ULL * 1000000ULL);
    ASSERT_TRUE(ev4.has_value());
    EXPECT_EQ(ev4->event_type, DeviceEventType::LinkRecovered);

    auto p = tracker.getProfile(key);
    EXPECT_EQ(p->state, LinkQualityState::Stable);
}

TEST(BtLinkQualityTrackerTest, TimestampOrderingStrictCausality) {
    BtLinkQualityTracker tracker;
    auto key = makeKey();

    // 模拟在 t = 5000ms (5000000000ns) 收到一个有效 RSSI -70
    tracker.feedFreshRssi(key, -70, 5000, 5000000000ULL);

    // 场景 A：事件发生在 t = 6000ms（断连前 1 秒），命中！
    auto r1 = tracker.getRssiBeforeEvent(key, 6000000000ULL, 10000000000ULL);
    ASSERT_TRUE(r1.has_value());
    EXPECT_EQ(*r1, -70);

    // 场景 B：在 t = 7000ms 又来了一个新 RSSI -55（例如设备重连了）
    tracker.feedFreshRssi(key, -55, 7000, 7000000000ULL);

    // 此时针对 t = 6000ms 的断连事件再次查询，绝不能取到重连后的 -55！
    auto r2 = tracker.getRssiBeforeEvent(key, 6000000000ULL, 10000000000ULL);
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(*r2, -70) << "绝不能把断开事件之后产生的新样本回填过去！";

    // 场景 C：针对发生在 t = 16000ms 的事件查询（距离 t=5000 过去 11 秒，超出了 10s TTL）
    // 假设未产生新样本（设备早已静默超时）：必须返回 nullopt
    auto r3 = tracker.getRssiBeforeEvent(key, 19000000000ULL, 10000000000ULL);
    // 注意：t=7000 到 t=19000 差距 12 秒，均超过 10s TTL
    EXPECT_FALSE(r3.has_value()) << "超出 TTL 的陈旧采样必须置空";
}

TEST(BtLinkQualityTrackerTest, DeviceKeyIsolation) {
    BtLinkQualityTracker tracker;
    auto k1 = makeKey("AA:BB:CC:DD:EE:FF", BtAddressType::LeRandom, "gw-1");
    auto k2 = makeKey("AA:BB:CC:DD:EE:FF", BtAddressType::LePublic, "gw-1"); // 地址类型不同
    auto k3 = makeKey("AA:BB:CC:DD:EE:FF", BtAddressType::LeRandom, "gw-2"); // 网关不同

    for (int i = 0; i < 10; ++i) {
        tracker.feedFreshRssi(k1, -50, 1000 + i * 1000, (1000 + i * 1000) * 1000000ULL);
        tracker.feedFreshRssi(k2, -70, 1000 + i * 1000, (1000 + i * 1000) * 1000000ULL);
        tracker.feedFreshRssi(k3, -90, 1000 + i * 1000, (1000 + i * 1000) * 1000000ULL);
    }

    auto p1 = tracker.getProfile(k1);
    auto p2 = tracker.getProfile(k2);
    auto p3 = tracker.getProfile(k3);

    ASSERT_TRUE(p1 && p2 && p3);
    EXPECT_EQ(*p1->baseline_rssi_dbm, -50);
    EXPECT_EQ(*p2->baseline_rssi_dbm, -70);
    EXPECT_EQ(*p3->baseline_rssi_dbm, -90);
}

TEST(BtLinkQualityTrackerTest, WarmStartBehavior) {
    BtLinkQualityTracker tracker;
    auto key = makeKey();

    // 预热载入基线 -65dBm
    DeviceLinkProfile prof;
    prof.key = key;
    prof.baseline_rssi_dbm = -65;
    prof.state = LinkQualityState::Stable;
    prof.first_seen_ms = 1000;
    prof.last_seen_ms = 1000;

    tracker.loadWarmStartBaseline(prof);

    auto p1 = tracker.getProfile(key);
    ASSERT_TRUE(p1.has_value());
    EXPECT_EQ(*p1->baseline_rssi_dbm, -65);
    EXPECT_EQ(p1->baseline_sample_count, 0u); // 不伪造 30 个点

    // 预热后立即能防范恶化（跌破 -65 - 15 = -80）
    tracker.feedFreshRssi(key, -82, 2000, 2000000000ULL);
    tracker.feedFreshRssi(key, -83, 3000, 3000000000ULL);
    auto ev = tracker.feedFreshRssi(key, -84, 4000, 4000000000ULL);
    ASSERT_TRUE(ev.has_value()) << "Warm-start 应在启动初期直接发挥异常拦截作用！";
}

TEST(BtLinkQualityTrackerTest, StaleDeviceRelearns) {
    BtLinkQualityTracker tracker;
    auto key = makeKey();

    // 灌入 10 个样本建立基线 -60
    for (int i = 0; i < 10; ++i) {
        tracker.feedFreshRssi(key, -60, 1000 + i * 1000, (1000 + i * 1000) * 1000000ULL);
    }
    EXPECT_EQ(tracker.getProfile(key)->state, LinkQualityState::Stable);

    // 沉睡 25 小时（> 24h 阈值）
    const uint64_t next_wall_ms = 1000 + 9 * 1000 + 25 * 3600 * 1000ULL;
    const uint64_t next_mono_ns = (1000 + 9 * 1000 + 25 * 3600 * 1000ULL) * 1000000ULL;

    tracker.feedFreshRssi(key, -85, next_wall_ms, next_mono_ns);

    auto p = tracker.getProfile(key);
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->state, LinkQualityState::Learning) << "长期未见设备再次上线必须重新学习，不得沿用旧工位基线！";
}

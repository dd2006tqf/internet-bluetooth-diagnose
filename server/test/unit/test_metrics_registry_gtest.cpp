#include <gtest/gtest.h>
#include "metrics/metrics_registry.hpp"
#include "metrics/metric_normalizer.hpp"
#include <thread>
#include <vector>

using namespace weaknet;
using namespace std::chrono_literals;

TEST(MetricNormalizerTest, HandlesInitialAndIncremental) {
    CounterNormalizer norm;
    // 首轮建立基线，无速率输出
    auto res1 = norm.update(1000, 10);
    EXPECT_FALSE(res1.has_value());

    // 第二轮产生有效增量：发包 100，丢包 5 -> 5.0%
    auto res2 = norm.update(1100, 15);
    ASSERT_TRUE(res2.has_value());
    EXPECT_DOUBLE_EQ(res2->rate_percent, 5.0);
    EXPECT_EQ(res2->delta_packets, 100u);
    EXPECT_EQ(res2->delta_drops, 5u);

    // 无活动发包
    auto res3 = norm.update(1100, 15);
    EXPECT_FALSE(res3.has_value());

    // 计数器倒退（重载/重置）
    auto res4 = norm.update(200, 2);
    EXPECT_FALSE(res4.has_value()); // 重新建立基线

    // 恢复正常增量
    auto res5 = norm.update(300, 4);
    ASSERT_TRUE(res5.has_value());
    EXPECT_DOUBLE_EQ(res5->rate_percent, 2.0);
}

TEST(MetricsRegistryTest, BasicPublishAndWindowQuery) {
    MetricsRegistry registry;
    auto now = std::chrono::steady_clock::now();

    MetricSample s1 = MetricSample::valid(25.0);
    s1.observed_at = now - 50s;

    MetricSample s2 = MetricSample::valid(35.0);
    s2.observed_at = now - 10s;

    MetricSample s_old = MetricSample::valid(99.0);
    s_old.observed_at = now - 150s;

    registry.publish("wlan0", MetricId::RTT_MS, s_old);
    registry.publish("wlan0", MetricId::RTT_MS, s1);
    registry.publish("wlan0", MetricId::RTT_MS, s2);

    auto latest = registry.latest("wlan0", MetricId::RTT_MS);
    ASSERT_TRUE(latest.has_value());
    EXPECT_DOUBLE_EQ(latest->value, 35.0);

    // 提取 120s 窗口：应包含 s1 和 s2，排除 s_old
    auto win = registry.window("wlan0", MetricId::RTT_MS, 120s, now);
    ASSERT_EQ(win.size(), 2u);
    EXPECT_DOUBLE_EQ(win[0].value, 25.0);
    EXPECT_DOUBLE_EQ(win[1].value, 35.0);
}

TEST(MetricsRegistryTest, ConcurrentAccessSafety) {
    MetricsRegistry registry;
    std::vector<std::thread> writers;
    constexpr int kWriters = 4;
    constexpr int kSamplesPerWriter = 100;

    for (int w = 0; w < kWriters; ++w) {
        writers.emplace_back([&registry, w]() {
            for (int i = 0; i < kSamplesPerWriter; ++i) {
                registry.publish("wlan0", MetricId::RTT_MS, MetricSample::valid(10.0 + w + i));
                registry.publish("eth0", MetricId::TCP_LOSS_RATE, MetricSample::valid(1.0));
            }
        });
    }

    for (auto& t : writers) {
        t.join();
    }

    auto wlan_win = registry.window("wlan0", MetricId::RTT_MS, 120s);
    EXPECT_GT(wlan_win.size(), 0u);
    EXPECT_LE(wlan_win.size(), MetricSeries::kDefaultCapacity);
}

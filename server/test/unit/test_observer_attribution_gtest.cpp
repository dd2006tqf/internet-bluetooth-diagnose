/**
 * @file test_observer_attribution_gtest.cpp
 * @brief 观测者降级归因真值表 + CPU 采样器降级语义
 *
 * 覆盖两类必须钉死的语义：
 *   1. 归因四分支 —— 必须能在证据上区分"网络真的坏了"与"采集端自己过载"，
 *                    且数据缺失时绝不猜。
 *   2. 采样器降级 —— 设备节点不存在时必须报不可用，**不得回落成 0**。
 *                    0 会被下游解读成"CPU 很闲"，从而掩盖真实故障。
 */

#include <gtest/gtest.h>

#include "assurance/observer_attribution.hpp"
#include "host_pressure_monitor.hpp"

using namespace weaknet;
using namespace weaknet_dbus;

namespace {

/// 造一个"报 observer 降级"的 SLE 结果
SleResult makeDegraded(const std::string& reason) {
    SleResult s;
    s.state = HealthState::UNKNOWN;
    s.coverage = Coverage::PARTIAL;
    s.reason = reason;
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 不涉及 observer 的 SLE → 不归因
// ---------------------------------------------------------------------------

TEST(ObserverAttribution, HealthyObservationIsNotAttributed) {
    SleResult s;
    s.state = HealthState::GOOD;
    s.reason = "dns_resolution_healthy";

    auto r = attributeObserverDegradation(s, "dns_service", 0.99, true);

    EXPECT_EQ(r.attribution, ObserverAttribution::NONE);
    EXPECT_FALSE(r.evaluated);
    EXPECT_TRUE(r.evidence.empty());
    EXPECT_EQ(r.trigger_sle, "dns_service");
    // 原始 reason 必须原样保留，绝不被改写
    EXPECT_EQ(r.observer_reason, "dns_resolution_healthy");
}

TEST(ObserverAttribution, UnknownWithoutObserverReasonIsNotAttributed) {
    // 只是"没观测到"，不是"观测不可靠"。两者不可混为一谈：
    // 前者 CPU 再忙也与归因无关，乱归因会让 UNKNOWN 变成噪声。
    SleResult s;
    s.state = HealthState::UNKNOWN;
    s.reason = "no_dns_observations";

    auto r = attributeObserverDegradation(s, "dns_service", 0.99, true);

    EXPECT_EQ(r.attribution, ObserverAttribution::NONE);
    EXPECT_FALSE(r.evaluated);
}

// ---------------------------------------------------------------------------
// 2. observer 降级 × 无 CPU 数据 → UNATTRIBUTED（不猜）
// ---------------------------------------------------------------------------

TEST(ObserverAttribution, DegradedWithoutCpuDataIsUnattributed) {
    auto s = makeDegraded("observer_unreliable_capture_emit_failure");

    auto r = attributeObserverDegradation(s, "dns_service", 0.0, false);

    EXPECT_EQ(r.attribution, ObserverAttribution::UNATTRIBUTED);
    EXPECT_TRUE(r.evaluated);
    EXPECT_FALSE(r.cpu_available);
    // 缺数据时不能被当成"CPU 正常"：那会把本机问题甩给网络
    EXPECT_NE(r.attribution, ObserverAttribution::NOT_HOST_CPU_BOUND);
    // 也不能被当成"CPU 过载"：那是凭空指控
    EXPECT_NE(r.attribution, ObserverAttribution::SELF_INFLICTED);
    EXPECT_FALSE(r.explanation.empty());
}

TEST(ObserverAttribution, UnavailableCpuIgnoresCarriedValue) {
    // 即便 cpu_busy 传了 0.99，只要 available=false 就必须忽略该值。
    // 这条防的是"调用方忘了检查 available，顺手把默认值当实测值传进来"。
    auto s = makeDegraded("observer_unreliable_event_loss");

    auto r = attributeObserverDegradation(s, "dns_service", 0.99, false);

    EXPECT_EQ(r.attribution, ObserverAttribution::UNATTRIBUTED);
    EXPECT_FALSE(r.cpu_available);
}

// ---------------------------------------------------------------------------
// 3. observer 降级 × CPU 过载 → SELF_INFLICTED
// ---------------------------------------------------------------------------

TEST(ObserverAttribution, DegradedWithSaturatedCpuIsSelfInflicted) {
    auto s = makeDegraded("observer_unreliable_capture_emit_failure");

    auto r = attributeObserverDegradation(s, "dns_service", 0.95, true);

    EXPECT_EQ(r.attribution, ObserverAttribution::SELF_INFLICTED);
    EXPECT_TRUE(r.evaluated);
    EXPECT_TRUE(r.cpu_available);
    EXPECT_DOUBLE_EQ(r.observed_cpu_busy, 0.95);
    EXPECT_DOUBLE_EQ(r.cpu_threshold, 0.85);

    // 证据必须同时包含"归因结论"与"支撑它的 CPU 实测值"
    bool has_attr = false, has_cpu = false;
    for (const auto& e : r.evidence) {
        if (e.metric == "observer_attribution") has_attr = true;
        if (e.metric == "host_cpu_busy_ratio") has_cpu = true;
    }
    EXPECT_TRUE(has_attr);
    EXPECT_TRUE(has_cpu);
}

TEST(ObserverAttribution, ThresholdIsInclusive) {
    // 恰好等于阈值就算过载：边界值必须可判定，不留"擦边不算"的灰区
    auto s = makeDegraded("tracker_capacity_overflow");

    auto r = attributeObserverDegradation(s, "dns_service", 0.85, true);

    EXPECT_EQ(r.attribution, ObserverAttribution::SELF_INFLICTED);
}

// ---------------------------------------------------------------------------
// 4. observer 降级 × CPU 正常 → NOT_HOST_CPU_BOUND
// ---------------------------------------------------------------------------

TEST(ObserverAttribution, DegradedWithNormalCpuRulesOutHostStarvation) {
    auto s = makeDegraded("observer_unreliable_ambiguity");

    auto r = attributeObserverDegradation(s, "dns_service", 0.20, true);

    EXPECT_EQ(r.attribution, ObserverAttribution::NOT_HOST_CPU_BOUND);
    EXPECT_TRUE(r.evaluated);
    EXPECT_TRUE(r.cpu_available);

    // 语义边界：CPU 正常**不能**推出"网络坏了"，只能排除 CPU 过载这一个成因。
    // 用词必须保守，不能出现断言网络故障的表述。
    EXPECT_EQ(r.explanation.find("suspect self-inflicted"), std::string::npos);
}

TEST(ObserverAttribution, CustomThresholdIsHonored) {
    auto s = makeDegraded("observer_unreliable_event_loss");

    // 阈值为 0.5 时，0.60 应判过载，0.40 应判排除
    auto high = attributeObserverDegradation(s, "dns_service", 0.60, true, 0.50);
    auto low = attributeObserverDegradation(s, "dns_service", 0.40, true, 0.50);

    EXPECT_EQ(high.attribution, ObserverAttribution::SELF_INFLICTED);
    EXPECT_EQ(low.attribution, ObserverAttribution::NOT_HOST_CPU_BOUND);
    EXPECT_DOUBLE_EQ(high.cpu_threshold, 0.50);
}

// ---------------------------------------------------------------------------
// 5. 四种已知 observer 降级原因都必须被识别
// ---------------------------------------------------------------------------

TEST(ObserverAttribution, AllKnownObserverReasonsAreRecognized) {
    const char* reasons[] = {
        "observer_unreliable_capture_emit_failure",
        "observer_unreliable_event_loss",
        "observer_unreliable_ambiguity",
        "tracker_capacity_overflow",
    };

    for (const char* reason : reasons) {
        auto r = attributeObserverDegradation(makeDegraded(reason), "dns_service", 0.10, true);
        EXPECT_TRUE(r.evaluated) << "reason not recognized: " << reason;
        EXPECT_NE(r.attribution, ObserverAttribution::NONE) << "reason: " << reason;
    }
}

TEST(ObserverAttribution, SimilarButUnknownReasonIsNotRecognized) {
    // 前缀相同的未知串不应被吞进归因（保守退化，不误报）
    auto s = makeDegraded("observer_unreliable_something_new");

    auto r = attributeObserverDegradation(s, "dns_service", 0.99, true);

    EXPECT_EQ(r.attribution, ObserverAttribution::NONE);
    EXPECT_FALSE(r.evaluated);
}

TEST(ObserverAttribution, ToStringCoversAllStates) {
    EXPECT_STREQ(observerAttributionToString(ObserverAttribution::NONE), "NONE");
    EXPECT_STREQ(observerAttributionToString(ObserverAttribution::UNATTRIBUTED), "UNATTRIBUTED");
    EXPECT_STREQ(observerAttributionToString(ObserverAttribution::SELF_INFLICTED), "SELF_INFLICTED");
    EXPECT_STREQ(observerAttributionToString(ObserverAttribution::NOT_HOST_CPU_BOUND),
                 "NOT_HOST_CPU_BOUND");
}

// ---------------------------------------------------------------------------
// 6. CPU 采样器降级：设备不存在绝不伪造 0
// ---------------------------------------------------------------------------

TEST(HostCpuPressureSampler, MissingDeviceReportsUnavailableNotZero) {
    HostCpuPressureSampler sampler("/nonexistent/weaknet_cpu_stat_probe");

    auto p = sampler.sample();

    EXPECT_FALSE(p.available);
    EXPECT_EQ(p.reason, "device_absent");
    // 关键断言：不得把"读不到"伪装成"CPU 很闲"
    // （busy_ratio 保持默认 0，但 available=false 才是权威语义）
    EXPECT_EQ(p.sampled_cpus, 0u);
}

TEST(HostCpuPressureSampler, MissingDeviceIsReportedEveryTime) {
    // 懒打开 + 每轮重试：模块后插时应能自愈，但在此之前每轮都必须老实报缺失，
    // 不能第一轮报缺失、之后静默继承旧状态。
    HostCpuPressureSampler sampler("/nonexistent/weaknet_cpu_stat_probe");

    for (int i = 0; i < 3; ++i) {
        auto p = sampler.sample();
        EXPECT_FALSE(p.available) << "round " << i;
        EXPECT_EQ(p.reason, "device_absent") << "round " << i;
    }
}

TEST(HostCpuPressureSampler, ReportsConfiguredDevicePath) {
    HostCpuPressureSampler sampler("/tmp/some_probe_node");
    EXPECT_EQ(sampler.devPath(), "/tmp/some_probe_node");
}

TEST(HostCpuPressureSampler, DefaultDevicePathMatchesKernelModuleContract) {
    HostCpuPressureSampler sampler;
    EXPECT_EQ(sampler.devPath(), "/dev/weaknet_cpu_stat");
}

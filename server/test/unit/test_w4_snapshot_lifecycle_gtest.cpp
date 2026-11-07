/**
 * @file test_w4_snapshot_lifecycle_gtest.cpp
 * @brief W4 快照生命周期与跨消费者一致性回归测试
 *
 * 验证以下硬不变式：
 * 1. 启动初期无快照 -> latest() 返回 nullptr -> 消费者必须输出 UNKNOWN(no_assessment_yet)，绝不假象 GOOD。
 * 2. 配置代变更 (config_generation) -> isCurrent() 判定失效 -> 输出 stale_assessment，绝不返回旧代结论。
 * 3. 网络代更迭 (network_epoch) -> isCurrent() 判定失效 -> 输出 stale_assessment。
 * 4. 同一不可变快照 -> 跨消费者一致性保证（D-Bus HealthCheck、GetNetworkExperience、Web、History）。
 */

#include <gtest/gtest.h>
#include "assessment_snapshot.hpp"
#include "assurance/legacy_adapter.hpp"

using namespace weaknet;

class W4SnapshotLifecycleTest : public ::testing::Test {
protected:
    AssessmentSnapshotStore store;
};

// --- 1. 启动初期无快照 -> nullptr 处理 ---

TEST_F(W4SnapshotLifecycleTest, UninitializedStoreReturnsNullptr) {
    auto snap = store.latest();
    EXPECT_EQ(snap, nullptr)
        << "Daemon 启动后在 quality 线程产生第一份评估前，latest() 必须为 nullptr";
}

// --- 2. 配置代变更使得快照失效 ---

TEST_F(W4SnapshotLifecycleTest, ConfigGenerationInvalidatesSnapshot) {
    auto s = std::make_shared<AssessmentSnapshot>();
    s->sequence_id = 1;
    s->config_generation = 10;
    s->network_epoch = 1;
    s->experience.overall = HealthState::GOOD;
    store.publish(s);

    auto latest = store.latest();
    ASSERT_NE(latest, nullptr);
    EXPECT_TRUE(AssessmentSnapshotStore::isCurrent(*latest, 10, 1));

    // 发生配置热重载 -> config_generation 变为 11
    EXPECT_FALSE(AssessmentSnapshotStore::isCurrent(*latest, 11, 1))
        << "配置代已变，旧代的快照必须被判定为 stale，绝不能继续返回旧 GOOD";
}

// --- 3. 网络代（重连/网卡切换）使得快照失效 ---

TEST_F(W4SnapshotLifecycleTest, NetworkEpochInvalidatesSnapshot) {
    auto s = std::make_shared<AssessmentSnapshot>();
    s->sequence_id = 2;
    s->config_generation = 10;
    s->network_epoch = 1;
    s->experience.overall = HealthState::GOOD;
    store.publish(s);

    auto latest = store.latest();
    ASSERT_NE(latest, nullptr);
    // Wi-Fi 漫游/重连，network_epoch 升至 2
    EXPECT_FALSE(AssessmentSnapshotStore::isCurrent(*latest, 10, 2))
        << "网络 Epoch 改变后旧快照失效，必须等待新 Epoch 下的评估完成";
}

// --- 4. 跨消费者一致性验证：同一快照在 HealthCheck 与 GetNetworkExperience 导出一致 ---

TEST_F(W4SnapshotLifecycleTest, CrossConsumerSnapshotConsistency) {
    auto s = std::make_shared<AssessmentSnapshot>();
    s->sequence_id = 42;
    s->config_generation = 5;
    s->network_epoch = 2;
    s->experience.iface = "wlan0";
    s->experience.overall = HealthState::DEGRADED;
    s->experience.overall_coverage = Coverage::FULL_FOR_PROFILE;
    s->experience.primary_issue = "High latency or excessive jitter";
    s->experience.display_score = 45;
    store.publish(s);

    auto snap = store.latest();
    ASSERT_NE(snap, nullptr);

    // 消费者 A: HealthCheck (Legacy 映射 JSON)
    std::string hc_json = LegacyAdapter::toHealthCheckJson(snap->experience, 4);
    EXPECT_NE(hc_json.find("\"overall_quality\":\"FAIR\""), std::string::npos); // DEGRADED -> FAIR
    EXPECT_NE(hc_json.find("High latency or excessive jitter"), std::string::npos);

    // 消费者 B: GetNetworkExperience (Schema v2 JSON)
    std::string exp_json = LegacyAdapter::toExperienceJsonV2(snap->experience);
    EXPECT_NE(exp_json.find("\"state\":\"DEGRADED\""), std::string::npos);
    EXPECT_NE(exp_json.find("High latency or excessive jitter"), std::string::npos);
    EXPECT_NE(exp_json.find("\"schema_version\":2"), std::string::npos);

    // 结论绝对同源一致
}

// --- 5. 观测者归因字段必须真正走出进程 ---
//
// 这个字段曾经是典型的「孤儿面」：server.cpp 把它写进 NetworkExperience，
// 单元测试也覆盖了判定逻辑，但 toExperienceJsonV2 —— GetNetworkExperience
// 的唯一出口 —— 从不序列化它。于是无论现场发生什么，
// `weaknet-cli experience` 都读不到任何归因结论，机器可读的那一半等于不存在。
TEST_F(W4SnapshotLifecycleTest, ObserverAttributionEscapesThroughV2Json) {
    auto s = std::make_shared<AssessmentSnapshot>();
    s->experience.overall = HealthState::UNKNOWN;
    s->experience.dns_service.state = HealthState::UNKNOWN;
    s->experience.dns_service.reason = "observer_unreliable_capture_emit_failure";
    s->experience.observer_attribution = ObserverAttribution::SELF_INFLICTED;
    s->experience.observer_attribution_detail =
        "Observer degraded while host CPU was saturated";
    store.publish(s);

    auto snap = store.latest();
    ASSERT_NE(snap, nullptr);

    const std::string json = LegacyAdapter::toExperienceJsonV2(snap->experience);
    EXPECT_NE(json.find("\"observer_attribution\":\"SELF_INFLICTED\""),
              std::string::npos)
        << json;
    EXPECT_NE(json.find("\"observer_attribution_detail\":\"Observer degraded"), std::string::npos)
        << json;

    // 无降级时必须稳定输出 NONE，且既有关键字段一字不动
    auto clean = std::make_shared<AssessmentSnapshot>();
    clean->experience.overall = HealthState::GOOD;
    store.publish(clean);
    const std::string clean_json =
        LegacyAdapter::toExperienceJsonV2(store.latest()->experience);
    EXPECT_NE(clean_json.find("\"observer_attribution\":\"NONE\""), std::string::npos);
    EXPECT_NE(clean_json.find("\"schema_version\":2"), std::string::npos);
    EXPECT_NE(clean_json.find("\"observer_attribution_detail\":\"\""), std::string::npos);
}

// --- 6. 热状态归因字段必须同样走出进程 ---
//
// 与 observer_attribution 同款要求，但也有一个不同的陷阱：本字段有两条
// 序列化内容（主结论 + issues 列表）。只序列化主结论会让
// 「风扇坏了但没节流」这种情况在 JSON 里消失——enum 只能存一个值，
// 而两条问题可以同时成立。
TEST_F(W4SnapshotLifecycleTest, ThermalAttributionEscapesThroughV2Json) {
    auto s = std::make_shared<AssessmentSnapshot>();
    s->experience.overall = HealthState::UNKNOWN;
    s->experience.thermal_attribution = ThermalAttribution::THROTTLED;
    s->experience.thermal_attribution_detail =
        "Collector host CPU frequency ceiling is reduced to 50% of hardware max";
    s->experience.thermal_issues = {"CPU_THROTTLED", "LOW_TEMP_FULL_SPEED"};
    store.publish(s);

    auto snap = store.latest();
    ASSERT_NE(snap, nullptr);

    const std::string json = LegacyAdapter::toExperienceJsonV2(snap->experience);
    EXPECT_NE(json.find("\"thermal_attribution\":\"THROTTLED\""), std::string::npos)
        << json;
    EXPECT_NE(json.find("\"thermal_attribution_detail\":\"Collector host"), std::string::npos)
        << json;
    // 两条 issue 都要出现：只留主结论会丢掉"风扇也异常"这条信息
    EXPECT_NE(json.find("\"thermal_issues\":[\"CPU_THROTTLED\",\"LOW_TEMP_FULL_SPEED\"]"),
              std::string::npos)
        << json;

    // 未评估时必须稳定输出 NONE + 空列表，且既有关键字段一字不动
    auto clean = std::make_shared<AssessmentSnapshot>();
    clean->experience.overall = HealthState::GOOD;
    store.publish(clean);
    const std::string clean_json =
        LegacyAdapter::toExperienceJsonV2(store.latest()->experience);
    EXPECT_NE(clean_json.find("\"thermal_attribution\":\"NONE\""), std::string::npos);
    EXPECT_NE(clean_json.find("\"thermal_issues\":[]"), std::string::npos);
    EXPECT_NE(clean_json.find("\"schema_version\":2"), std::string::npos);
    // 既有字段不受影响
    EXPECT_NE(clean_json.find("\"observer_attribution\":\"NONE\""), std::string::npos);
}

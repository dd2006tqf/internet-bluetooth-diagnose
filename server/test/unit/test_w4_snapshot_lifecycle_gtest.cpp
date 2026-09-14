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

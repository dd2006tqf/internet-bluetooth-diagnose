#include <gtest/gtest.h>
#include "assurance/edge_ai_scheduler.hpp"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace weaknet;
using namespace std::chrono_literals;

namespace {

std::shared_ptr<const AssessmentSnapshot> makeSnapshot(
    uint64_t seq, HealthState state, const std::string& issue, const std::string& iface = "wlan0") {
    auto snap = std::make_shared<AssessmentSnapshot>();
    snap->sequence_id = seq;
    snap->network_epoch = 2;
    snap->config_generation = 1;
    snap->experience.iface = iface;
    snap->experience.overall = state;
    if (!issue.empty()) {
        snap->experience.primary_issue = issue;
    }
    return snap;
}

}  // namespace

// 1. 健康快照不触发推理
TEST(EdgeAiSchedulerTest, SkipsHealthySnapshots) {
    int sink_called = 0;
    EdgeAiScheduler sched("dev-01", [&](EdgeAiHypothesisRecord) { sink_called++; });
    sched.start();

    auto healthy = makeSnapshot(1, HealthState::GOOD, "");
    EXPECT_FALSE(sched.submitIfEligible(healthy));
    EXPECT_EQ(sched.stats().skipped_healthy, 1u);
    EXPECT_EQ(sink_called, 0);
    sched.stop();
}

// 2. 同类异常在冷却窗口 (Cooldown) 内去重，不同异常或冷却过后放行
TEST(EdgeAiSchedulerTest, EnforcesCooldownDeduplication) {
    std::mutex mtx;
    std::vector<EdgeAiHypothesisRecord> results;

    EdgeAiScheduler sched(
        "dev-01",
        [&](EdgeAiHypothesisRecord r) {
            std::lock_guard<std::mutex> lock(mtx);
            results.push_back(std::move(r));
        },
        std::chrono::seconds(2), // 2秒冷却供单测验证
        std::chrono::milliseconds(5000));

    // Mock 正常输出的快命令
    sched.setCustomCommandForTest(
        "cat >/dev/null && printf '{\"edge_ai_hypothesis\":{\"hypothesis_id\":\"eh-test01\",\"summary\":\"mock ok\",\"recommended_actions\":[\"PROBE_LOCAL_GATEWAY\"],\"created_at\":\"2026-10-11T12:00:00Z\"}}'");
    sched.start();

    auto snap1 = makeSnapshot(10, HealthState::DEGRADED, "High latency");
    auto snap2 = makeSnapshot(11, HealthState::DEGRADED, "High latency"); // 同一键，冷却期内
    auto snap3 = makeSnapshot(12, HealthState::BAD, "DNS failure");       // 不同异常类型，放行

    EXPECT_TRUE(sched.submitIfEligible(snap1));
    EXPECT_FALSE(sched.submitIfEligible(snap2)); // 被冷却拦截
    EXPECT_EQ(sched.stats().skipped_cooldown, 1u);

    // 等待第 1 个任务执行完毕再提第 3 个
    std::this_thread::sleep_for(200ms);
    EXPECT_TRUE(sched.submitIfEligible(snap3));
    std::this_thread::sleep_for(200ms);

    {
        std::lock_guard<std::mutex> lock(mtx);
        ASSERT_EQ(results.size(), 2u);
        EXPECT_EQ(results[0].sequence_id, 10u);
        EXPECT_EQ(results[0].evidence_refs[0], "snap-dev-01-2-10");
        EXPECT_EQ(results[1].sequence_id, 12u);
    }

    sched.stop();
}

// 3. 15s (单测设为 300ms) 硬超时 SIGKILL 强杀测试：子进程死循环挂起时能被强杀回收且不阻塞主线程
TEST(EdgeAiSchedulerTest, HardTimeoutKillsHangingSubprocess) {
    int sink_called = 0;
    EdgeAiScheduler sched(
        "dev-01",
        [&](EdgeAiHypothesisRecord) { sink_called++; },
        std::chrono::seconds(1),
        std::chrono::milliseconds(300)); // 300ms 硬超时

    // 模拟大模型死锁挂起 30 秒
    sched.setCustomCommandForTest("sleep 30");
    sched.start();

    auto t0 = std::chrono::steady_clock::now();
    auto snap = makeSnapshot(99, HealthState::BAD, "Severe loss");
    EXPECT_TRUE(sched.submitIfEligible(snap));

    // submitIfEligible 必须微秒级非阻塞返回
    auto submit_Cost = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(submit_Cost, 50);

    // 等待超过 300ms 让 watchdog SIGKILL 生效
    std::this_thread::sleep_for(550ms);

    auto st = sched.stats();
    EXPECT_EQ(st.timed_out_killed, 1u);
    EXPECT_EQ(st.failed_or_killed, 1u);
    EXPECT_EQ(sink_called, 0);

    sched.stop();
}

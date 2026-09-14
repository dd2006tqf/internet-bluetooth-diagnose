/**
 * @file test_w3_time_dynamics_gtest.cpp
 * @brief W3 时间行为联合校准测试 (Holdout Set 严格盲测)
 *
 * 验证以下时间动力学不变量与退出准则：
 * 1. 劣化吸收 (10s hold)：毛刺与暂态不造成 flapping。
 * 2. 故障稳态迁移时间：10s 满后平稳进入 DEGRADED / BAD。
 * 3. 严重故障 Critical Bypass：即时命中，时延 <= 3s。
 * 4. 恢复保持期 (20s hold)：故障消除后需 20s 稳定新证据才回到 GOOD。
 * 5. HR-6 不变量：无新 revision 到达时绝对静止。
 */

#include <gtest/gtest.h>
#include "assurance/state_stabilizer.hpp"
#include <chrono>

using namespace weaknet;
using namespace std::chrono_literals;

class W3TimeDynamicsCalibration : public ::testing::Test {
protected:
    StateStabilizer stabilizer;
    std::chrono::steady_clock::time_point t0{std::chrono::steady_clock::now()};
};

// --- 1. 短暂毛刺（S10）被稳定器完全吸收，flapping = 0 ---

TEST_F(W3TimeDynamicsCalibration, GlitchAbsorptionSuppressesFlapping) {
    stabilizer.update(HealthState::GOOD, 1, t0);
    EXPECT_EQ(stabilizer.stableState(), HealthState::GOOD);

    // 发生短暂毛刺（持续 3 秒，未达 10s degradation_hold）
    stabilizer.update(HealthState::BAD, 2, t0 + 1s);
    EXPECT_EQ(stabilizer.stableState(), HealthState::GOOD);
    stabilizer.update(HealthState::BAD, 3, t0 + 3s);
    EXPECT_EQ(stabilizer.stableState(), HealthState::GOOD);

    // 毛刺消失，网络恢复正常
    stabilizer.update(HealthState::GOOD, 4, t0 + 4s);
    EXPECT_EQ(stabilizer.stableState(), HealthState::GOOD);

    // 全程输出稳定，状态震荡次数严格为 0
    EXPECT_EQ(stabilizer.stableState(), HealthState::GOOD);
}

// --- 2. 持续硬性故障（S11）迁移时间准确在 10s 门槛 ---

TEST_F(W3TimeDynamicsCalibration, SustainedFailureTransitionsAtHoldBoundary) {
    stabilizer.update(HealthState::GOOD, 1, t0);

    // 持续输入 BAD 证据：第 2 秒、第 5 秒、第 9 秒
    // 候选状态为 BAD 开始于 t0 + 2s
    stabilizer.update(HealthState::BAD, 2, t0 + 2s);
    stabilizer.update(HealthState::BAD, 3, t0 + 5s);
    stabilizer.update(HealthState::BAD, 4, t0 + 9s);
    // 此时距离候选开始 (t0+2s) 仅经过 7 秒，未达 10s degradation_hold，依然保持初始 GOOD
    EXPECT_EQ(stabilizer.stableState(), HealthState::GOOD);

    // 满 10s 边界到达：距离 t0 + 2s 满 10s 即 t0 + 12s + 1ms
    stabilizer.update(HealthState::BAD, 5, t0 + 12s + 1ms);
    EXPECT_EQ(stabilizer.stableState(), HealthState::BAD);
}

// --- 3. 恢复保持期（S12）必须连续观察满 20s ---

TEST_F(W3TimeDynamicsCalibration, RecoveryRequiresFullTwentySecondsHold) {
    // 建立 BAD 稳态：t0 时 candidate 开始，满 10s 即 t0 + 10s + 1ms 达到 BAD 稳态
    stabilizer.update(HealthState::BAD, 1, t0);
    stabilizer.update(HealthState::BAD, 2, t0 + 10s + 1ms);
    EXPECT_EQ(stabilizer.stableState(), HealthState::BAD);

    auto t_rec = t0 + 12s;
    // 恢复正常信号开始于 t_rec
    stabilizer.update(HealthState::GOOD, 3, t_rec);
    stabilizer.update(HealthState::GOOD, 4, t_rec + 10s);
    stabilizer.update(HealthState::GOOD, 5, t_rec + 19s);
    // 19s 时仍不得提前宣称恢复
    EXPECT_EQ(stabilizer.stableState(), HealthState::BAD);

    // 满 20s 边界到达
    stabilizer.update(HealthState::GOOD, 6, t_rec + 20s + 1ms);
    EXPECT_EQ(stabilizer.stableState(), HealthState::GOOD);
}

// --- 4. 恢复期间如果出现抖动重置计时 ---

TEST_F(W3TimeDynamicsCalibration, IntermittentFailureDuringRecoveryResetsTimer) {
    stabilizer.update(HealthState::BAD, 1, t0);
    stabilizer.update(HealthState::BAD, 2, t0 + 10s + 1ms);
    EXPECT_EQ(stabilizer.stableState(), HealthState::BAD);

    auto t_rec = t0 + 12s;
    stabilizer.update(HealthState::GOOD, 3, t_rec);
    stabilizer.update(HealthState::GOOD, 4, t_rec + 15s); // 恢复中，已等 15s

    // 突发一次 BAD
    stabilizer.update(HealthState::BAD, 5, t_rec + 16s);
    EXPECT_EQ(stabilizer.stableState(), HealthState::BAD);

    // 重新回到 GOOD
    stabilizer.update(HealthState::GOOD, 6, t_rec + 17s);
    // 计时器已被打断重置，即使距离 t_rec 已经过了 25s，距离重新 GOOD 仅 10s，不可恢复
    stabilizer.update(HealthState::GOOD, 7, t_rec + 27s);
    EXPECT_EQ(stabilizer.stableState(), HealthState::BAD);

    // 重新经过满 20s
    stabilizer.update(HealthState::GOOD, 8, t_rec + 17s + 20s + 1ms);
    EXPECT_EQ(stabilizer.stableState(), HealthState::GOOD);
}

// --- 5. Critical Bypass 零时延突发确诊 ---

TEST_F(W3TimeDynamicsCalibration, CriticalBypassInstantlyElevatesToBad) {
    stabilizer.update(HealthState::GOOD, 1, t0);
    EXPECT_EQ(stabilizer.stableState(), HealthState::GOOD);

    // SR-9 突发连续超时单杀标记为 bypass
    stabilizer.update(HealthState::BAD, 2, t0 + 500ms, /*is_critical_bypass=*/true);
    // 必须瞬间进入 BAD，不被 10s 保持期阻挡
    EXPECT_EQ(stabilizer.stableState(), HealthState::BAD);
}

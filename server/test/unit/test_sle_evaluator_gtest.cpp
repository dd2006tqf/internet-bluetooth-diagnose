#include <gtest/gtest.h>
#include "assurance/ip_reachability_evaluator.hpp"
#include "assurance/responsiveness_evaluator.hpp"
#include "assurance/reliability_evaluator.hpp"
#include "assurance/rf_health_evaluator.hpp"
#include "assurance/overall_policy.hpp"
#include "assurance/state_stabilizer.hpp"

using namespace weaknet;
using namespace std::chrono_literals;

TEST(IpReachabilityTest, HandlesSuccessRatioAndConsecutiveFailures) {
    // 连续失败 3 次 -> BAD
    std::vector<MetricSample> samples = {
        MetricSample::valid(1.0),
        MetricSample::valid(1.0),
        MetricSample::valid(0.0),
        MetricSample::valid(0.0),
        MetricSample::valid(0.0)
    };
    auto res = IpReachabilityEvaluator::evaluate(samples);
    EXPECT_EQ(res.state, HealthState::BAD);

    // 偶发失败 1 次 -> DEGRADED
    std::vector<MetricSample> samples_intermittent = {
        MetricSample::valid(1.0),
        MetricSample::valid(1.0),
        MetricSample::valid(1.0),
        MetricSample::valid(0.0)
    };
    auto res2 = IpReachabilityEvaluator::evaluate(samples_intermittent);
    EXPECT_EQ(res2.state, HealthState::DEGRADED);

    // 全成功 -> GOOD
    std::vector<MetricSample> samples_good = {
        MetricSample::valid(1.0),
        MetricSample::valid(1.0),
        MetricSample::valid(1.0)
    };
    auto res3 = IpReachabilityEvaluator::evaluate(samples_good);
    EXPECT_EQ(res3.state, HealthState::GOOD);
}

TEST(ResponsivenessTest, HandlesMedianAndBadRatio) {
    // 样本不足门禁 -> UNKNOWN
    std::vector<MetricSample> rtt_few = { MetricSample::valid(20.0) };
    auto res_few = ResponsivenessEvaluator::evaluate(rtt_few, {});
    EXPECT_EQ(res_few.state, HealthState::UNKNOWN);

    // 样本充足且极佳 -> GOOD
    std::vector<MetricSample> rtt_good = {
        MetricSample::valid(25.0),
        MetricSample::valid(28.0),
        MetricSample::valid(30.0),
        MetricSample::valid(22.0)
    };
    auto res_good = ResponsivenessEvaluator::evaluate(rtt_good, {});
    EXPECT_EQ(res_good.state, HealthState::GOOD);

    // 多个高延迟样本导致超标率过高 -> BAD
    std::vector<MetricSample> rtt_bad = {
        MetricSample::valid(30.0),
        MetricSample::valid(160.0),
        MetricSample::valid(180.0),
        MetricSample::valid(40.0)
    };
    auto res_bad = ResponsivenessEvaluator::evaluate(rtt_bad, {});
    EXPECT_EQ(res_bad.state, HealthState::BAD);
}

TEST(ReliabilityTest, AsymmetricEvidenceDecision) {
    // 1. wifi_loss 严重超标单杀 BAD，即使 tcp_loss 缺失
    std::vector<MetricSample> wifi_bad = { MetricSample::valid(5.5) };
    auto res = ReliabilityEvaluator::evaluate(wifi_bad, {}, true);
    EXPECT_EQ(res.state, HealthState::BAD);

    // 2. 无线环境下 wifi_loss 与 tcp_loss 均正常 -> GOOD 且达成 FULL_FOR_PROFILE
    std::vector<MetricSample> wifi_good = { MetricSample::valid(0.1) };
    std::vector<MetricSample> tcp_good_wl = { MetricSample::valid(0.3) };
    auto res_wireless_good = ReliabilityEvaluator::evaluate(wifi_good, tcp_good_wl, true);
    EXPECT_EQ(res_wireless_good.state, HealthState::GOOD);
    EXPECT_EQ(res_wireless_good.coverage, Coverage::FULL_FOR_PROFILE);
    ASSERT_EQ(res_wireless_good.evidence.size(), 2u);
    EXPECT_EQ(res_wireless_good.evidence[0].metric, "wifi_loss_rate");
    EXPECT_DOUBLE_EQ(res_wireless_good.evidence[0].value, 0.1);

    // 3. 有线环境无需 Wi-Fi，NOT_APPLICABLE 算作 FULL_FOR_PROFILE
    std::vector<MetricSample> tcp_good = { MetricSample::valid(0.2) };
    auto res_wired = ReliabilityEvaluator::evaluate({}, tcp_good, false);
    EXPECT_EQ(res_wired.state, HealthState::GOOD);
    EXPECT_EQ(res_wired.coverage, Coverage::FULL_FOR_PROFILE);
}

TEST(OverallPolicyTest, RFBadDoesNotKillExperience) {
    SleResult reach{HealthState::GOOD, Coverage::FULL_FOR_PROFILE, {}, ""};
    SleResult resp{HealthState::GOOD, Coverage::FULL_FOR_PROFILE, {}, ""};
    SleResult rel{HealthState::GOOD, Coverage::FULL_FOR_PROFILE, {}, ""};
    SleResult rf_bad{HealthState::BAD, Coverage::FULL_FOR_PROFILE, {}, "weak signal"};

    // RF 为 BAD，但整体网络体验仍为 GOOD，只产生 Warning
    auto exp = OverallPolicy::decide("wlan0", reach, resp, rel, rf_bad, false);
    EXPECT_EQ(exp.overall, HealthState::GOOD);
    EXPECT_FALSE(exp.warnings.empty());
}

TEST(RfHealthTest, WiredIsNotApplicable) {
    // 有线链路：state=UNKNOWN, applicability=NOT_APPLICABLE, coverage=FULL_FOR_PROFILE
    auto res = RfHealthEvaluator::evaluate({}, false);
    EXPECT_EQ(res.state, HealthState::UNKNOWN);
    EXPECT_EQ(res.applicability, Applicability::NOT_APPLICABLE);
    EXPECT_EQ(res.coverage, Coverage::FULL_FOR_PROFILE);

    // 无线且无样本：仍为 APPLICABLE（存在该维度，只是缺证据）
    auto res_wlan = RfHealthEvaluator::evaluate({}, true);
    EXPECT_EQ(res_wlan.state, HealthState::UNKNOWN);
    EXPECT_EQ(res_wlan.applicability, Applicability::APPLICABLE);
    EXPECT_EQ(res_wlan.coverage, Coverage::NONE);
}

TEST(OverallPolicyTest, WiredRfNotApplicableIgnoredEntirely) {
    SleResult reach{HealthState::GOOD, Coverage::FULL_FOR_PROFILE, {}, ""};
    SleResult resp{HealthState::GOOD, Coverage::FULL_FOR_PROFILE, {}, ""};
    SleResult rel{HealthState::GOOD, Coverage::FULL_FOR_PROFILE, {}, ""};

    // 有线 RF：NOT_APPLICABLE → 不 vote、不 warning、GOOD 按满分展示
    auto rf_na = RfHealthEvaluator::evaluate({}, false);
    auto exp = OverallPolicy::decide("eth0", reach, resp, rel, rf_na, false);
    EXPECT_EQ(exp.overall, HealthState::GOOD);
    EXPECT_TRUE(exp.warnings.empty());
    EXPECT_EQ(exp.display_score, 95);

    // 即使是 BAD 状态的 NOT_APPLICABLE，也绝不产生 warning
    SleResult rf_bad_na{HealthState::BAD, Coverage::FULL_FOR_PROFILE, {}, "n/a", Applicability::NOT_APPLICABLE};
    auto exp2 = OverallPolicy::decide("eth0", reach, resp, rel, rf_bad_na, false);
    EXPECT_EQ(exp2.overall, HealthState::GOOD);
    EXPECT_TRUE(exp2.warnings.empty());
}

TEST(OverallPolicyTest, CoverageGateTruthTable) {
    SleResult good_full{HealthState::GOOD, Coverage::FULL_FOR_PROFILE, {}, ""};
    SleResult rf_na{HealthState::UNKNOWN, Coverage::FULL_FOR_PROFILE, {}, "wired", Applicability::NOT_APPLICABLE};

    // 1. Reachability BAD 优先于一切 → BAD
    SleResult reach_bad{HealthState::BAD, Coverage::FULL_FOR_PROFILE, {}, ""};
    SleResult rel_unknown{HealthState::UNKNOWN, Coverage::NONE, {}, ""};
    SleResult resp_unknown{HealthState::UNKNOWN, Coverage::NONE, {}, ""};
    auto e1 = OverallPolicy::decide("wlan0", reach_bad, resp_unknown, rel_unknown, rf_na);
    EXPECT_EQ(e1.overall, HealthState::BAD);

    // 2. Reliability BAD（即使 Reachability 也 UNKNOWN）→ BAD
    SleResult rel_bad{HealthState::BAD, Coverage::FULL_FOR_PROFILE, {}, ""};
    SleResult reach_unknown{HealthState::UNKNOWN, Coverage::NONE, {}, ""};
    auto e2 = OverallPolicy::decide("wlan0", reach_unknown, resp_unknown, rel_bad, rf_na);
    EXPECT_EQ(e2.overall, HealthState::BAD);

    // 3. Responsiveness BAD → DEGRADED（负面证据可在 coverage 不足下成立）
    SleResult resp_bad{HealthState::BAD, Coverage::PARTIAL, {}, ""};
    auto e3 = OverallPolicy::decide("wlan0", good_full, resp_bad, rel_unknown, rf_na);
    EXPECT_EQ(e3.overall, HealthState::DEGRADED);

    // 4. 任一核心 SLE DEGRADED → DEGRADED，即使 coverage 为 PARTIAL/其他核心 UNKNOWN
    SleResult rel_degraded{HealthState::DEGRADED, Coverage::PARTIAL, {}, ""};
    auto e4 = OverallPolicy::decide("wlan0", reach_unknown, resp_unknown, rel_degraded, rf_na);
    EXPECT_EQ(e4.overall, HealthState::DEGRADED);

    // 5a. Coverage Gate：Reachability UNKNOWN 且无负面证据 → UNKNOWN（不允许 GOOD）
    auto e5a = OverallPolicy::decide("wlan0", reach_unknown, good_full, good_full, rf_na);
    EXPECT_EQ(e5a.overall, HealthState::UNKNOWN);

    // 5b. Reachability known 但 Reliability 与 Responsiveness 均 unknown → UNKNOWN
    SleResult resp_none{HealthState::UNKNOWN, Coverage::NONE, {}, ""};
    SleResult rel_none{HealthState::UNKNOWN, Coverage::NONE, {}, ""};
    auto e5b = OverallPolicy::decide("wlan0", good_full, resp_none, rel_none, rf_na);
    EXPECT_EQ(e5b.overall, HealthState::UNKNOWN);

    // 5c. Reachability known AND Responsiveness known（Reliability unknown）→ 允许 GOOD
    auto e5c = OverallPolicy::decide("wlan0", good_full, good_full, rel_none, rf_na);
    EXPECT_EQ(e5c.overall, HealthState::GOOD);

    // 6. 全部 known 且健康 → GOOD
    auto e6 = OverallPolicy::decide("wlan0", good_full, good_full, good_full, rf_na);
    EXPECT_EQ(e6.overall, HealthState::GOOD);
    EXPECT_EQ(e6.overall_coverage, Coverage::FULL_FOR_PROFILE);
}

TEST(OverallPolicyTest, NotApplicableCoreDoesNotPenalizeCoverage) {
    // NOT_APPLICABLE 的核心 SLE（理论场景，验证通用规则）不拉低 overall coverage
    SleResult reach{HealthState::GOOD, Coverage::FULL_FOR_PROFILE, {}, ""};
    SleResult resp_na{HealthState::UNKNOWN, Coverage::NONE, {}, "n/a", Applicability::NOT_APPLICABLE};
    SleResult rel{HealthState::GOOD, Coverage::FULL_FOR_PROFILE, {}, ""};
    SleResult rf{HealthState::GOOD, Coverage::FULL_FOR_PROFILE, {}, ""};

    auto exp = OverallPolicy::decide("eth0", reach, resp_na, rel, rf);
    // Reachability known + Reliability known → 满足 Minimum Core Coverage
    EXPECT_EQ(exp.overall, HealthState::GOOD);
    EXPECT_EQ(exp.overall_coverage, Coverage::FULL_FOR_PROFILE);
}

TEST(StateStabilizerTest, HR6_NoEvidenceNoHysteresisProgression) {
    StateStabilizer stabilizer;
    auto t0 = std::chrono::steady_clock::now();

    // 初始处于 GOOD，revision = 1
    stabilizer.update(HealthState::GOOD, 1, t0);
    EXPECT_EQ(stabilizer.stableState(), HealthState::GOOD);

    // 接下来多次 evaluate 返回 BAD，但 evidence_revision 仍然是 1（无新样本更新）
    auto t1 = t0 + 15s; // 超过 degradation_hold(10s)
    stabilizer.update(HealthState::BAD, 1, t1);
    // HR-6 铁律：无新证据，不得推进 Hysteresis！状态仍然必须是 GOOD！
    EXPECT_EQ(stabilizer.stableState(), HealthState::GOOD);

    // 当有新证据 revision = 2 到达时，启动候选计时
    stabilizer.update(HealthState::BAD, 2, t1);
    EXPECT_EQ(stabilizer.stableState(), HealthState::GOOD); // 刚出现，还未满 hold 期

    // 持续有新证据且时间满足 hold 期 -> 成功迁移到 BAD
    auto t2 = t1 + 11s;
    stabilizer.update(HealthState::BAD, 3, t2);
    EXPECT_EQ(stabilizer.stableState(), HealthState::BAD);

    // 严重故障直接即时 bypass
    stabilizer.reset();
    stabilizer.update(HealthState::GOOD, 10, t0);
    stabilizer.update(HealthState::BAD, 11, t0, /*is_critical_bypass=*/true);
    EXPECT_EQ(stabilizer.stableState(), HealthState::BAD);
}

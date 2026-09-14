/**
 * @file test_dns_service_evaluator_gtest.cpp
 * @brief DNS Service SLE 真值表：把所有决策分支固化为可执行断言。
 *
 * 重点覆盖三类容易出错、且后果严重的语义：
 *   1. Missingness 三态 —— 证据不足时绝不伪造 GOOD
 *   2. 分类互斥     —— TC/RCODE 不得重叠，TRUNCATED/OTHER 不稀释 failure_ratio
 *   3. Observer 三分支 —— 观测器丢证据时区分"直接坏证据"与"缺席推导证据"，
 *                        坏证据可在 PARTIAL coverage 下成立，GOOD 必须证据充足
 */

#include <gtest/gtest.h>
#include "assurance/dns_service_evaluator.hpp"

using namespace weaknet;
using namespace std::chrono_literals;

namespace {

/// 构造一个"证据充足"的窗口，便于按需覆盖单一项做对照。
DnsMetricWindow makeWindow() {
    DnsMetricWindow w;
    w.binding_epoch = 1;
    w.evaluation_cutoff = std::chrono::steady_clock::now();
    w.capture_attempts = 1000;
    w.delivered_events = 1000;
    w.response_match_attempts = 100;
    w.query_capture_attempts = 100;
    return w;
}

std::vector<DnsTransactionRecord> noTerminals() { return {}; }

}  // namespace

// ---------------------------------------------------------------------------
// 1. Missingness 三态
// ---------------------------------------------------------------------------

TEST(DnsServiceEvaluatorTruthTable, NoObservationsIsUnknownNotGood) {
    auto w = makeWindow();
    w.queries_started = 0;
    w.current_inflight = 0;

    auto res = DnsServiceEvaluator::evaluate(w, noTerminals());
    EXPECT_EQ(res.state, HealthState::UNKNOWN);
    EXPECT_EQ(res.coverage, Coverage::NONE);
    EXPECT_EQ(res.reason, "no_dns_observations");
}

TEST(DnsServiceEvaluatorTruthTable, AwaitingInflightIsPartial) {
    auto w = makeWindow();
    w.queries_started = 1;
    w.current_inflight = 1;
    // 无任何终态

    auto res = DnsServiceEvaluator::evaluate(w, noTerminals());
    EXPECT_EQ(res.state, HealthState::UNKNOWN);
    EXPECT_EQ(res.coverage, Coverage::PARTIAL);
    EXPECT_EQ(res.reason, "awaiting_inflight");
}

TEST(DnsServiceEvaluatorTruthTable, BelowTerminalGateIsInsufficient) {
    auto w = makeWindow();
    w.queries_started = 4;
    w.responses_noerror = 4;  // 少于 min_terminals_for_evaluation(5)

    auto res = DnsServiceEvaluator::evaluate(w, noTerminals());
    EXPECT_EQ(res.state, HealthState::UNKNOWN);
    EXPECT_EQ(res.coverage, Coverage::PARTIAL);
    EXPECT_EQ(res.reason, "insufficient_terminal_observations");
}

// ---------------------------------------------------------------------------
// 2. 分类互斥与 failure_ratio 语义
// ---------------------------------------------------------------------------

TEST(DnsServiceEvaluatorTruthTable, TruncatedDoesNotDiluteFailureRatio) {
    auto w = makeWindow();
    // 10 个可评估终态：8 成功 2 失败 -> 20% 失败率，判 BAD
    w.queries_started = 30;
    w.responses_noerror = 8;
    w.responses_servfail = 2;
    // TRUNCATED / OTHER 不计入 evaluable，不得稀释分母
    w.responses_truncated = 15;
    w.responses_other = 5;

    auto res = DnsServiceEvaluator::evaluate(w, noTerminals());
    EXPECT_DOUBLE_EQ(w.failureRatio(), 0.20);
    // 覆盖率分母只含可评估终态与 OTHER/TRUNCATED：10 / (10 + 15 + 5)
    EXPECT_DOUBLE_EQ(w.classificationCoverage(), 10.0 / 30.0);
    // 分类覆盖率 0.333 < 0.70 门禁 -> UNKNOWN（证据不可分类时不下结论）
    EXPECT_EQ(res.state, HealthState::UNKNOWN);
    EXPECT_EQ(res.reason, "insufficient_classified_outcomes");
}

TEST(DnsServiceEvaluatorTruthTable, NxdomainCountsAsKnownSuccess) {
    auto w = makeWindow();
    w.queries_started = 10;
    w.responses_noerror = 6;
    w.responses_nxdomain = 4;  // 域名不存在 = 事务成功

    EXPECT_EQ(w.knownSuccess(), 10u);
    EXPECT_EQ(w.knownFailure(), 0u);
    EXPECT_DOUBLE_EQ(w.failureRatio(), 0.0);

    auto res = DnsServiceEvaluator::evaluate(w, noTerminals());
    // NXDOMAIN 不是服务失败：DNS Service 应为 GOOD
    EXPECT_EQ(res.state, HealthState::GOOD);
    EXPECT_EQ(res.reason, "dns_service_healthy");
    EXPECT_EQ(res.coverage, Coverage::FULL_FOR_PROFILE);
}

TEST(DnsServiceEvaluatorTruthTable, HighFailureRateIsBad) {
    auto w = makeWindow();
    w.queries_started = 10;
    w.responses_noerror = 8;
    w.responses_servfail = 2;  // 20%

    auto res = DnsServiceEvaluator::evaluate(w, noTerminals());
    EXPECT_EQ(res.state, HealthState::BAD);
    EXPECT_EQ(res.reason, "high_failure_rate");
}

TEST(DnsServiceEvaluatorTruthTable, ElevatedFailureRateIsDegraded) {
    auto w = makeWindow();
    w.queries_started = 20;
    w.responses_noerror = 19;
    w.responses_servfail = 1;  // 5%

    auto res = DnsServiceEvaluator::evaluate(w, noTerminals());
    EXPECT_EQ(res.state, HealthState::DEGRADED);
    EXPECT_EQ(res.reason, "elevated_failure_rate");
}

TEST(DnsServiceEvaluatorTruthTable, ExcessiveMedianLatencyIsBad) {
    auto w = makeWindow();
    w.queries_started = 5;
    w.responses_noerror = 5;
    w.latencies_ms = {600.0, 610.0, 620.0, 630.0, 640.0};

    auto res = DnsServiceEvaluator::evaluate(w, noTerminals());
    EXPECT_EQ(res.state, HealthState::BAD);
    EXPECT_EQ(res.reason, "excessive_dns_latency");
}

TEST(DnsServiceEvaluatorTruthTable, ElevatedLatencyIsDegraded) {
    auto w = makeWindow();
    w.queries_started = 5;
    w.responses_noerror = 5;
    // W3 校准值：中位数 250ms >= 200ms -> DEGRADED
    w.latencies_ms = {210.0, 230.0, 250.0, 260.0, 280.0};

    auto res = DnsServiceEvaluator::evaluate(w, noTerminals());
    EXPECT_EQ(res.state, HealthState::DEGRADED);
    EXPECT_EQ(res.reason, "elevated_dns_latency");
}

TEST(DnsServiceEvaluatorTruthTable, ToleratesModerateLatencyUnderCalibratedThreshold) {
    auto w = makeWindow();
    w.queries_started = 5;
    w.responses_noerror = 5;
    // W3 校准前 150ms 会误判为 DEGRADED；校准至 200ms 后，160ms 中位数被正确判定为 GOOD
    w.latencies_ms = {120.0, 140.0, 160.0, 170.0, 180.0};

    auto res = DnsServiceEvaluator::evaluate(w, noTerminals());
    EXPECT_EQ(res.state, HealthState::GOOD);
    EXPECT_EQ(res.reason, "dns_service_healthy");
}

// ---------------------------------------------------------------------------
// 3. SR-9 突发连续超时单杀
// ---------------------------------------------------------------------------

TEST(DnsServiceEvaluatorTruthTable, BurstTimeoutsWithoutSuccessIsBad) {
    auto now = std::chrono::steady_clock::now();
    auto w = makeWindow();
    w.evaluation_cutoff = now;
    w.queries_started = 3;
    w.timeouts = 3;

    std::vector<DnsTransactionRecord> terminals;
    for (int i = 0; i < 3; ++i) {
        DnsTransactionRecord r;
        r.binding_epoch = 1;
        r.state = DnsTransactionState::TIMEOUT_EXPIRED;
        r.completed_at = now - std::chrono::milliseconds((3 - i) * 1000);
        terminals.push_back(r);
    }

    bool bypass = false;
    auto res = DnsServiceEvaluator::evaluate(w, terminals, {}, &bypass);
    EXPECT_EQ(res.state, HealthState::BAD);
    EXPECT_EQ(res.reason, "critical_burst_timeouts");
    EXPECT_TRUE(bypass);
}

TEST(DnsServiceEvaluatorTruthTable, SuccessInterleavedBreaksBurst) {
    auto now = std::chrono::steady_clock::now();
    auto w = makeWindow();
    w.evaluation_cutoff = now;
    w.queries_started = 5;

    std::vector<DnsTransactionRecord> terminals;
    for (int i = 0; i < 5; ++i) {
        DnsTransactionRecord r;
        r.binding_epoch = 1;
        r.state = (i % 2 == 0) ? DnsTransactionState::TIMEOUT_EXPIRED
                               : DnsTransactionState::NOERROR;
        r.completed_at = now - std::chrono::milliseconds((5 - i) * 1000);
        terminals.push_back(r);
    }

    bool bypass = false;
    DnsServiceEvaluator::evaluate(w, terminals, {}, &bypass);
    EXPECT_FALSE(bypass);
}

// ---------------------------------------------------------------------------
// 4. Observer 质量三分支（本增量核心不变式）
// ---------------------------------------------------------------------------

TEST(DnsServiceEvaluatorTruthTable, ObserverLossWithoutDirectEvidenceIsUnknownNotBad) {
    auto w = makeWindow();
    w.queries_started = 10;
    w.timeouts = 10;              // 全部故障依据来自"没看到 response"
    w.capture_attempts = 1000;
    w.capture_emit_failures = 100;  // 10% 输出失败，远超阈值
    w.delivered_events = 900;
    w.perf_lost_events = 100;       // 10% 投递丢失

    auto res = DnsServiceEvaluator::evaluate(w, noTerminals());
    // 观测器在丢事件，而"超时"正是会被丢失污染的缺席推导证据：
    // 不允许把这些 timeout 当作 DNS 故障，必须降为 UNKNOWN。
    EXPECT_EQ(res.state, HealthState::UNKNOWN);
    EXPECT_EQ(res.coverage, Coverage::PARTIAL);
    EXPECT_EQ(res.reason, "observer_unreliable_capture_emit_failure");
}

TEST(DnsServiceEvaluatorTruthTable, ObserverLossWithDirectEvidenceStillBad) {
    auto w = makeWindow();
    w.queries_started = 10;
    w.responses_servfail = 10;      // 直接捕获到服务端失败
    w.capture_attempts = 1000;
    w.capture_emit_failures = 100;  // 观测器同时也不可靠
    w.delivered_events = 900;
    w.perf_lost_events = 100;

    auto res = DnsServiceEvaluator::evaluate(w, noTerminals());
    // 直接负面证据不因观测丢失而失效：坏证据可在 PARTIAL coverage 下成立
    EXPECT_EQ(res.state, HealthState::BAD);
    EXPECT_EQ(res.coverage, Coverage::PARTIAL);
    EXPECT_GT(w.directNegativeEvidence(), 0u);
}

TEST(DnsServiceEvaluatorTruthTable, ObserverLossWithoutAnyNegativeEvidenceIsNeverGood) {
    auto w = makeWindow();
    w.queries_started = 10;
    w.responses_noerror = 10;       // 表面上一切正常
    w.capture_attempts = 1000;
    w.capture_emit_failures = 100;
    w.delivered_events = 900;
    w.perf_lost_events = 100;

    auto res = DnsServiceEvaluator::evaluate(w, noTerminals());
    // 观测不可靠 + 无直接坏证据 -> 绝不能 GOOD
    EXPECT_NE(res.state, HealthState::GOOD);
    EXPECT_EQ(res.state, HealthState::UNKNOWN);
}

TEST(DnsServiceEvaluatorTruthTable, AmbiguityAboveThresholdDegradesObserver) {
    auto w = makeWindow();
    w.queries_started = 10;
    w.responses_noerror = 10;
    w.response_match_attempts = 100;
    w.tracking_ambiguous = 50;      // 50% 歧义

    auto res = DnsServiceEvaluator::evaluate(w, noTerminals());
    EXPECT_EQ(res.state, HealthState::UNKNOWN);
    EXPECT_EQ(res.reason, "observer_unreliable_ambiguity");
}

TEST(DnsServiceEvaluatorTruthTable, TrackerOverflowDegradesObserver) {
    auto w = makeWindow();
    w.queries_started = 10;
    w.responses_noerror = 10;
    w.query_capture_attempts = 100;
    w.tracker_insert_failures = 50;  // 50% 容量溢出

    auto res = DnsServiceEvaluator::evaluate(w, noTerminals());
    EXPECT_EQ(res.state, HealthState::UNKNOWN);
    EXPECT_EQ(res.reason, "tracker_capacity_overflow");
}

TEST(DnsServiceEvaluatorTruthTable, HealthyObserverKeepsFullCoverage) {
    auto w = makeWindow();
    w.queries_started = 10;
    w.responses_noerror = 10;

    auto res = DnsServiceEvaluator::evaluate(w, noTerminals());
    EXPECT_EQ(res.state, HealthState::GOOD);
    EXPECT_EQ(res.coverage, Coverage::FULL_FOR_PROFILE);
}

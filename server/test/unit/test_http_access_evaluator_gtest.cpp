/**
 * @file test_http_access_evaluator_gtest.cpp
 * @brief HTTP Access SLE 真值表
 *
 * 核心不变式（与 DNS / TCP SLE 一致，并额外固化 HTTP 语义边界）：
 *   1. 4xx 是业务语义，服务端正常应答 → 属传输健康，不判 BAD
 *      （等价于 DNS 的 NXDOMAIN 属事务成功）
 *   2. 5xx 是服务端明确报告的故障 → 直接负面证据
 *   3. 无响应 / TLS 失败 → 缺席推导证据，受 Evidence Quality 约束
 *   4. 证据不足 / 观测不可靠 → UNKNOWN，绝不伪造 GOOD
 */

#include <gtest/gtest.h>
#include "assurance/http_access_evaluator.hpp"

using namespace weaknet;

namespace {

HttpAccessEvaluator::Input makeInput(const std::vector<uint16_t>& codes,
                                     double ttfb_ms = 100.0) {
    HttpAccessEvaluator::Input in;
    for (auto c : codes) {
        in.samples.push_back(HttpAccessSample{c, ttfb_ms, false});
    }
    in.capture_events = in.samples.size();
    return in;
}

}  // namespace

TEST(HttpAccessEvaluatorTruthTable, NoSamplesIsUnknownNotGood) {
    HttpAccessEvaluator::Input in;
    auto res = HttpAccessEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::UNKNOWN);
    EXPECT_EQ(res.coverage, Coverage::NONE);
    EXPECT_EQ(res.reason, "no_http_observations");
}

TEST(HttpAccessEvaluatorTruthTable, BelowSampleGateIsUnknown) {
    auto in = makeInput({200, 200, 200});
    auto res = HttpAccessEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::UNKNOWN);
    EXPECT_EQ(res.reason, "insufficient_http_samples");
}

TEST(HttpAccessEvaluatorTruthTable, AllSuccessIsGood) {
    auto in = makeInput({200, 200, 200, 200, 200, 200});
    auto res = HttpAccessEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::GOOD);
    EXPECT_EQ(res.reason, "http_access_healthy");
}

TEST(HttpAccessEvaluatorTruthTable, ClientErrorsAreNotTransportFailure) {
    // 404/403 是业务语义：服务端正常应答，链路可用，不得判 BAD
    auto in = makeInput({404, 404, 403, 200, 200, 200});
    auto res = HttpAccessEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::GOOD);
}

TEST(HttpAccessEvaluatorTruthTable, ServerErrorsAreDirectNegativeEvidence) {
    // 5xx 是服务端明确故障；1/10 = 10% 落在 DEGRADED 区间（BAD 阈值为 20%）
    auto in = makeInput({500, 200, 200, 200, 200, 200, 200, 200, 200, 200});
    auto res = HttpAccessEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::DEGRADED);
    EXPECT_EQ(res.reason, "elevated_http_server_error_rate");
}

TEST(HttpAccessEvaluatorTruthTable, HighServerErrorIsBad) {
    auto in = makeInput({500, 500, 503, 200, 200, 200, 200, 200, 200, 200});
    auto res = HttpAccessEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::BAD);
    EXPECT_EQ(res.reason, "high_http_server_error_rate");
}

TEST(HttpAccessEvaluatorTruthTable, NoResponseIsBadWhenDominant) {
    auto in = makeInput({0, 0, 0, 0, 200, 200, 200, 200, 200, 200});
    auto res = HttpAccessEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::BAD);
    EXPECT_EQ(res.reason, "high_http_no_response_rate");
}

TEST(HttpAccessEvaluatorTruthTable, TlsFailureCountsAsNoResponse) {
    HttpAccessEvaluator::Input in;
    for (int i = 0; i < 8; ++i) in.samples.push_back(HttpAccessSample{0, 0.0, true});
    for (int i = 0; i < 2; ++i) in.samples.push_back(HttpAccessSample{200, 50.0, false});
    in.capture_events = 10;
    auto res = HttpAccessEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::BAD);
    EXPECT_EQ(res.reason, "high_http_no_response_rate");
}

TEST(HttpAccessEvaluatorTruthTable, HighTtfbIsBad) {
    auto in = makeInput({200, 200, 200, 200, 200, 200}, 6000.0);
    auto res = HttpAccessEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::BAD);
    EXPECT_EQ(res.reason, "excessive_http_latency");
}

TEST(HttpAccessEvaluatorTruthTable, ElevatedTtfbIsDegraded) {
    auto in = makeInput({200, 200, 200, 200, 200, 200}, 2000.0);
    auto res = HttpAccessEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::DEGRADED);
    EXPECT_EQ(res.reason, "elevated_http_latency");
}

TEST(HttpAccessEvaluatorTruthTable, ObserverLossWithoutDirectEvidenceIsUnknown) {
    auto in = makeInput({200, 200, 200, 200, 200, 200});
    in.capture_events = 1000;
    in.capture_lost = 200;
    auto res = HttpAccessEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::UNKNOWN);
    EXPECT_EQ(res.coverage, Coverage::PARTIAL);
    EXPECT_EQ(res.reason, "observer_unreliable_event_loss");
}

TEST(HttpAccessEvaluatorTruthTable, ObserverLossWithDirectEvidenceStillBad) {
    auto in = makeInput({500, 500, 500, 200, 200, 200, 200, 200, 200, 200});
    in.capture_events = 1000;
    in.capture_lost = 200;
    auto res = HttpAccessEvaluator::evaluate(in);
    EXPECT_EQ(res.state, HealthState::BAD);
    EXPECT_EQ(res.coverage, Coverage::PARTIAL);
}

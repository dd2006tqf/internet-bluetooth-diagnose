/**
 * @file test_assurance_policy_source_gtest.cpp
 * @brief Active/Passive 语义与 OverallPolicy 权威性真值表
 *
 * 核心原则：**Profile 决定某个 SLE 是否有资格影响 Overall，
 * 而不是"它算出了 BAD 就自动影响 Overall"。**
 *
 * 本文件固化 P1「Internet Access 评价语义收口」的六条不变式：
 *   1. DNS 仅 Host-Resolver-Capability 级负面证据具有 hard-veto 权力
 *   2. 单域名 SERVFAIL/REFUSED 不得自动证明 Internet BAD
 *   3. Passive TCP 即便自身 BAD，Overall 也不得因它变 BAD/DEGRADED
 *   4. Passive cleartext HTTP 同理（且 scope 仅覆盖明文）
 *   5. Portal 无受控探测能力时无任何决策权
 *   6. Core SLE（Reachability/Reliability/Responsiveness）权力不变
 */

#include <gtest/gtest.h>
#include "assurance/overall_policy.hpp"
#include "assurance/dns_service_evaluator.hpp"

using namespace weaknet;

namespace {

SleResult coreGood() {
    SleResult r;
    r.state = HealthState::GOOD;
    r.coverage = Coverage::FULL_FOR_PROFILE;
    return r;
}

SleResult passiveBad(EvidenceScope scope) {
    SleResult r;
    r.state = HealthState::BAD;
    r.coverage = Coverage::FULL_FOR_PROFILE;
    r.source = EvidenceSource::PASSIVE_REAL_TRAFFIC;
    r.scope = scope;
    r.reason = "test_bad";
    return r;
}

SleResult dnsBad(bool capability_level) {
    SleResult r;
    r.state = HealthState::BAD;
    r.coverage = Coverage::FULL_FOR_PROFILE;
    r.source = EvidenceSource::PASSIVE_REAL_TRAFFIC;
    r.scope = EvidenceScope::HOST_RESOLVER_CAPABILITY;
    r.capability_level_negative = capability_level;
    r.reason = "test_dns_bad";
    return r;
}

SleResult portalUnknownNoCapability() {
    SleResult r;
    r.state = HealthState::UNKNOWN;
    r.coverage = Coverage::NONE;
    r.source = EvidenceSource::UNSPECIFIED;
    r.scope = EvidenceScope::NO_CAPABILITY;
    r.reason = "no_portal_probe_capability";
    return r;
}

}  // namespace

// --- 1/2. DNS 否决权只授予 capability 级证据 ---

TEST(AssurancePolicySourceTest, DnsCapabilityFailureVetoesInternetAccess) {
    // 本机解析器能力失效（连续无响应）→ 有资格否决
    auto exp = OverallPolicy::decide("wlan0", coreGood(), coreGood(), coreGood(),
                                     coreGood(), dnsBad(/*capability_level=*/true),
                                     coreGood(), coreGood(), portalUnknownNoCapability(),
                                     AssessmentProfile::INTERNET_ACCESS);
    EXPECT_EQ(exp.overall, HealthState::BAD);
    ASSERT_TRUE(exp.primary_issue.has_value());
    EXPECT_EQ(*exp.primary_issue, "DNS service resolution failure");
}

TEST(AssurancePolicySourceTest, PerDomainDnsFailureDoesNotVetoInternetAccess) {
    // 单域名 SERVFAIL/REFUSED：解析器答了，只是那个域名解析不了。
    // 可能是域名自身/权威侧/策略问题，无权把 Internet 判 BAD。
    auto exp = OverallPolicy::decide("wlan0", coreGood(), coreGood(), coreGood(),
                                     coreGood(), dnsBad(/*capability_level=*/false),
                                     coreGood(), coreGood(), portalUnknownNoCapability(),
                                     AssessmentProfile::INTERNET_ACCESS);
    EXPECT_NE(exp.overall, HealthState::BAD)
        << "单域名解析失败不得继承 host-level 否决权";
    // 但必须仍然可见：作为 warning/证据呈现
    bool warned = false;
    for (const auto& w : exp.warnings) {
        if (w.find("DNS") != std::string::npos) warned = true;
    }
    EXPECT_TRUE(warned) << "非 capability 级 DNS 失败应作为 warning 呈现";
}

// --- 3. Passive TCP 非阻塞 ---

TEST(AssurancePolicySourceTest, PassiveTcpBadDoesNotBlockInternetAccess) {
    // 被动 TCP 观测的是"连向任意业务目的地的结果"，目的地由用户决定，
    // 可能本来就该失败。无权判定 Internet 不可用。
    auto exp = OverallPolicy::decide("wlan0", coreGood(), coreGood(), coreGood(),
                                     coreGood(), coreGood(),
                                     passiveBad(EvidenceScope::PER_DESTINATION),
                                     coreGood(), portalUnknownNoCapability(),
                                     AssessmentProfile::INTERNET_ACCESS);
    EXPECT_NE(exp.overall, HealthState::BAD)
        << "被动 TCP 失败不得把 Internet 判 BAD";
    EXPECT_NE(exp.overall, HealthState::DEGRADED)
        << "被动 TCP 失败也不得把 Internet 压到 DEGRADED（语义漏洞并未消失）";
    EXPECT_EQ(exp.overall, HealthState::GOOD);
}

// --- 4. Passive cleartext HTTP 非阻塞 ---

TEST(AssurancePolicySourceTest, PassiveCleartextHttpBadDoesNotBlockInternetAccess) {
    auto exp = OverallPolicy::decide("wlan0", coreGood(), coreGood(), coreGood(),
                                     coreGood(), coreGood(), coreGood(),
                                     passiveBad(EvidenceScope::CLEARTEXT_PER_DESTINATION),
                                     portalUnknownNoCapability(),
                                     AssessmentProfile::INTERNET_ACCESS);
    EXPECT_NE(exp.overall, HealthState::BAD);
    EXPECT_NE(exp.overall, HealthState::DEGRADED);
}

TEST(AssurancePolicySourceTest, PassiveDegradedServicesDoNotDegradeOverall) {
    auto tcp = passiveBad(EvidenceScope::PER_DESTINATION);
    tcp.state = HealthState::DEGRADED;
    auto http = passiveBad(EvidenceScope::CLEARTEXT_PER_DESTINATION);
    http.state = HealthState::DEGRADED;

    auto exp = OverallPolicy::decide("wlan0", coreGood(), coreGood(), coreGood(),
                                     coreGood(), coreGood(), tcp, http,
                                     portalUnknownNoCapability(),
                                     AssessmentProfile::INTERNET_ACCESS);
    EXPECT_EQ(exp.overall, HealthState::GOOD);
}

// --- 5. Portal 无能力时无决策权 ---

TEST(AssurancePolicySourceTest, PortalWithoutCapabilityCannotDecide) {
    // 即使有人把 portal.state 误填为 BAD，只要 scope=NO_CAPABILITY，
    // 就不得参与决策（双保险：能力缺失时不允许 override）
    auto portal = portalUnknownNoCapability();
    portal.state = HealthState::BAD;

    auto exp = OverallPolicy::decide("wlan0", coreGood(), coreGood(), coreGood(),
                                     coreGood(), coreGood(), coreGood(), coreGood(),
                                     portal, AssessmentProfile::INTERNET_ACCESS);
    EXPECT_NE(exp.overall, HealthState::BAD)
        << "无受控探测能力时 portal 不得做任何决策";
}

// --- 6. 全部被动服务同时 BAD，仍不足以判定 Internet ---

TEST(AssurancePolicySourceTest, AllPassiveServicesBadStillNotInternetVerdict) {
    auto exp = OverallPolicy::decide("wlan0", coreGood(), coreGood(), coreGood(),
                                     coreGood(), coreGood(),
                                     passiveBad(EvidenceScope::PER_DESTINATION),
                                     passiveBad(EvidenceScope::CLEARTEXT_PER_DESTINATION),
                                     portalUnknownNoCapability(),
                                     AssessmentProfile::INTERNET_ACCESS);
    EXPECT_EQ(exp.overall, HealthState::GOOD)
        << "被动服务证据只能作为 observed experience，不能冒充 Internet ground truth";
}

// --- 7. Core SLE 权力不变 ---

TEST(AssurancePolicySourceTest, CoreSlePowersUnchanged) {
    auto reachBad = coreGood();
    reachBad.state = HealthState::BAD;
    auto exp = OverallPolicy::decide("wlan0", reachBad, coreGood(), coreGood(),
                                     coreGood(), coreGood(), coreGood(), coreGood(),
                                     portalUnknownNoCapability(),
                                     AssessmentProfile::INTERNET_ACCESS);
    EXPECT_EQ(exp.overall, HealthState::BAD);
    EXPECT_EQ(*exp.primary_issue, "IP path is unreachable");

    auto relBad = coreGood();
    relBad.state = HealthState::BAD;
    auto exp2 = OverallPolicy::decide("wlan0", coreGood(), coreGood(), relBad,
                                      coreGood(), coreGood(), coreGood(), coreGood(),
                                      portalUnknownNoCapability(),
                                      AssessmentProfile::INTERNET_ACCESS);
    EXPECT_EQ(exp2.overall, HealthState::BAD);

    auto respBad = coreGood();
    respBad.state = HealthState::BAD;
    auto exp3 = OverallPolicy::decide("wlan0", coreGood(), respBad, coreGood(),
                                      coreGood(), coreGood(), coreGood(), coreGood(),
                                      portalUnknownNoCapability(),
                                      AssessmentProfile::INTERNET_ACCESS);
    EXPECT_EQ(exp3.overall, HealthState::DEGRADED);
}

// --- 8. DNS evaluator 层：capability 标志按失败类型区分 ---

TEST(AssurancePolicySourceTest, DnsEvaluatorMarksCapabilityOnlyForTimeoutDominantFailures) {
    // 10 个可评估终态，其中 3 个 SERVFAIL（30%，超 BAD 阈值）但 0 个超时
    DnsMetricWindow w;
    w.binding_epoch = 1;
    w.evaluation_cutoff = std::chrono::steady_clock::now();
    w.queries_started = 10;
    w.responses_noerror = 7;
    w.responses_servfail = 3;   // 全部是"答了但失败"
    w.timeouts = 0;

    auto res = DnsServiceEvaluator::evaluate(w, {});
    EXPECT_EQ(res.state, HealthState::BAD);
    EXPECT_FALSE(res.capability_level_negative)
        << "解析器答了（SERVFAIL）不等于本机解析能力失效，不得授予否决权";

    // 同样 30% 失败率，但全为超时，且涉及多个不同 QNAME
    // （解析器整体不响应 —— 这才是本机能力级故障）
    DnsMetricWindow w2 = w;
    w2.responses_noerror = 7;
    w2.responses_servfail = 0;
    w2.timeouts = 3;
    w2.timeout_distinct_qnames = 3;

    auto res2 = DnsServiceEvaluator::evaluate(w2, {});
    EXPECT_EQ(res2.state, HealthState::BAD);
    EXPECT_TRUE(res2.capability_level_negative)
        << "多域名同时无响应属本机能力级故障，应授予否决权";
}

TEST(AssurancePolicySourceTest, DnsEvaluatorDeclaresHostResolverScope) {
    DnsMetricWindow w;
    w.binding_epoch = 1;
    w.evaluation_cutoff = std::chrono::steady_clock::now();
    w.queries_started = 10;
    w.responses_noerror = 10;

    auto res = DnsServiceEvaluator::evaluate(w, {});
    EXPECT_EQ(res.source, EvidenceSource::PASSIVE_REAL_TRAFFIC);
    EXPECT_EQ(res.scope, EvidenceScope::HOST_RESOLVER_CAPABILITY);
}

// --- 9. Portal 无能力 ≠ 缺失 required evidence ---
//
// 关键：若把 Portal 当作 INTERNET_ACCESS 的 required SLE，则主动探测上线前
// 整个 INTERNET_ACCESS 会永远 UNKNOWN。没有实现的 capability 不等于
// 缺失的 required evidence —— Portal 当前是 NOT_AVAILABLE，不是 missing。
TEST(AssurancePolicySourceTest, PortalWithoutCapabilityDoesNotForceOverallUnknown) {
    auto exp = OverallPolicy::decide("wlan0", coreGood(), coreGood(), coreGood(),
                                     coreGood(), coreGood(), coreGood(), coreGood(),
                                     portalUnknownNoCapability(),
                                     AssessmentProfile::INTERNET_ACCESS);
    EXPECT_NE(exp.overall, HealthState::UNKNOWN)
        << "Portal 无探测能力不得让 Overall 永久 UNKNOWN";
    EXPECT_EQ(exp.overall, HealthState::GOOD);
}

// --- 10. 单一 QNAME 连续 timeout 不应获得 capability-level 否决权 ---
//
// 某个域名的权威链路异常，同样会让递归解析长时间无结果。
// 只有"多个不同 QNAME 同时失败"才足以证明本机解析能力整体失效。
TEST(AssurancePolicySourceTest, SingleQnameTimeoutsDoNotGrantCapabilityVeto) {
    DnsMetricWindow w;
    w.binding_epoch = 1;
    w.evaluation_cutoff = std::chrono::steady_clock::now();
    w.queries_started = 6;
    w.timeouts = 6;                  // 全部超时
    w.timeout_distinct_qnames = 1;   // 但只涉及同一个 QNAME

    auto res = DnsServiceEvaluator::evaluate(w, {});
    EXPECT_EQ(res.state, HealthState::BAD);
    EXPECT_FALSE(res.capability_level_negative)
        << "单一域名的权威链路异常不得升级为本机解析能力故障";

    // 对照：多个不同 QNAME 同时超时 → 才认定能力级
    DnsMetricWindow w2 = w;
    w2.timeout_distinct_qnames = 4;
    auto res2 = DnsServiceEvaluator::evaluate(w2, {});
    EXPECT_EQ(res2.state, HealthState::BAD);
    EXPECT_TRUE(res2.capability_level_negative);
}

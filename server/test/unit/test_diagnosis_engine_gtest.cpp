#include <gtest/gtest.h>
#include "assurance/evidence_id_generator.hpp"
#include "assurance/predicate_engine.hpp"
#include "assurance/action_registry.hpp"
#include "assurance/diagnosis_rules.hpp"
#include "assurance/causal_resolver.hpp"
#include "assurance/diagnosis_engine.hpp"

using namespace weaknet;

// 1. EvidenceIdGenerator 单测：跨重启稳定性与格式校验
TEST(EvidenceIdGeneratorTest, FormatAndUniqueness) {
    EvidenceIdGenerator gen("board_a7a", 12, true);
    std::string id1 = gen.generate("dns", 1042, 1);
    std::string id2 = gen.generate("reach", 1042, 2);

    EXPECT_EQ(id1, "ev_board_a7a_e12_dns_s1042_01");
    EXPECT_EQ(id2, "ev_board_a7a_e12_reach_s1042_02");

    // 容灾模式：降级使用 boot_uuid
    EvidenceIdGenerator fallback_gen("board_a7a", 0, false, "boot_uuid_test");
    std::string fb_id = fallback_gen.generate("dns", 1042, 1);
    EXPECT_EQ(fb_id, "ev_board_a7a_boot_uuid_test_dns_s1042_01");
}

// 2. PredicateEngine 单测：严格三态求值与 Missing != false 守卫
TEST(PredicateEngineTest, MissingIsNotFalse) {
    auto resolver = [](const std::string& field) -> std::optional<PredicateValue> {
        if (field == "existing_num") return 10.0;
        if (field == "existing_str") return std::string("GOOD");
        return std::nullopt; // 字段不存在
    };

    Predicate p_gt{"missing_field", PredicateOp::Gt, 5.0};
    EXPECT_EQ(PredicateEngine::evaluate(p_gt, resolver), EvalResult::Unknown);

    Predicate p_lte{"missing_field", PredicateOp::Lte, 5.0};
    EXPECT_EQ(PredicateEngine::evaluate(p_lte, resolver), EvalResult::Unknown);

    Predicate p_null{"missing_field", PredicateOp::IsNull, std::string("")};
    EXPECT_EQ(PredicateEngine::evaluate(p_null, resolver), EvalResult::True);

    // 正常数值比较
    Predicate p_num{"existing_num", PredicateOp::Gt, 5.0};
    EXPECT_EQ(PredicateEngine::evaluate(p_num, resolver), EvalResult::True);

    Predicate p_num_fail{"existing_num", PredicateOp::Lt, 5.0};
    EXPECT_EQ(PredicateEngine::evaluate(p_num_fail, resolver), EvalResult::False);
}

// 3. ActionRegistry 单测：强类型白名单参数与安全 posix_spawn exec 数组
TEST(ActionRegistryTest, StrongTypingAndNoShell) {
    ActionRegistry reg;

    // 参数合法
    auto res_ok = reg.validate("PROBE_PUBLIC_RESOLVER", {{"resolver", "223.5.5.5"}});
    EXPECT_TRUE(res_ok.ok);

    // 参数不在白名单
    auto res_bad_ip = reg.validate("PROBE_PUBLIC_RESOLVER", {{"resolver", "1.1.1.1"}});
    EXPECT_FALSE(res_bad_ip.ok);

    // 尝试注入恶意 shell 命令
    auto res_inject = reg.validate("PROBE_PUBLIC_RESOLVER", {{"resolver", "223.5.5.5; rm -rf /"}});
    EXPECT_FALSE(res_inject.ok);

    // 生成安全 ExecSpec
    auto spec_opt = reg.buildExecSpec("PROBE_PUBLIC_RESOLVER", {{"resolver", "223.5.5.5"}});
    ASSERT_TRUE(spec_opt.has_value());
    EXPECT_EQ(spec_opt->executable, "/usr/bin/dig");
    ASSERT_EQ(spec_opt->argv.size(), 5u);
    EXPECT_EQ(spec_opt->argv[0], "/usr/bin/dig");
    EXPECT_EQ(spec_opt->argv[1], "223.5.5.5");

    // 验证 SafeExec 真实调用能力（例如执行 /bin/cat 查看 resolv.conf）
    auto cat_spec = reg.buildExecSpec("CHECK_RESOLVER_CONFIG", {});
    ASSERT_TRUE(cat_spec.has_value());
    auto exec_res = ActionRegistry::safeExec(*cat_spec);
    EXPECT_EQ(exec_res.exit_code, 0);
    EXPECT_FALSE(exec_res.stdout_output.empty());
}

// 4. RuleLoader 环形抑制检测
TEST(RuleLoaderTest, DetectSuppressionCycle) {
    DiagnosisRule r1, r2;
    r1.id = "RULE_A";
    r1.domain = "TEST";
    r1.suppresses = {"RULE_B"};

    r2.id = "RULE_B";
    r2.domain = "TEST";
    r2.suppresses = {"RULE_A"};

    EXPECT_THROW(RuleLoader::validateRuleSet({r1, r2}), std::runtime_error);
}

// 5. Golden Case 01: DNS 突发连续超时但网关正常
TEST(GoldenCasesTest, Case01_DnsBurstTimeoutWithGoodGateway) {
    AssessmentSnapshot snap;
    snap.sequence_id = 101;
    snap.experience.iface = "wlan0";
    snap.experience.overall = HealthState::BAD;

    // 网关正常
    snap.experience.ip_reachability.state = HealthState::GOOD;
    snap.experience.ip_reachability.reason = "reachability_healthy";
    snap.experience.ip_reachability.evidence.push_back({"success_ratio", 1.0, "100% gateway reachable"});

    // Wi-Fi 信号极佳
    snap.experience.rf_health.state = HealthState::GOOD;
    snap.experience.rf_health.evidence.push_back({"rssi_dbm", -45.0, "Strong Wi-Fi"});

    // DNS 连续超时
    snap.experience.dns_service.state = HealthState::BAD;
    snap.experience.dns_service.reason = "critical_burst_timeouts";
    snap.experience.dns_service.evidence.push_back({"burst_timeouts", 3.0, "3 consecutive timeouts"});

    DiagnosisEngine engine;
    EvidenceIdGenerator id_gen("radxa_cubie_a7a", 1, true);
    auto facts = engine.diagnose(snap, id_gen);

    EXPECT_EQ(facts.fault_domain, "DNS_SERVICE");
    EXPECT_EQ(facts.primary_issue, "critical_burst_timeouts");
    EXPECT_EQ(facts.confidence, DiagnosisConfidence::HIGH);
    ASSERT_FALSE(facts.evidence_refs.empty());
    EXPECT_EQ(facts.evidence_refs[0], "ev_radxa_cubie_a7a_e1_dns_service_s101_01");

    // 排除项校验：由于 RSSI > -65 且 IP 正常，应成功排除 Wi-Fi 和 Gateway 故障
    bool excluded_wifi = false;
    for (const auto& ec : facts.excluded_causes) {
        if (ec.cause == "WEAK_WIFI_SIGNAL") excluded_wifi = true;
    }
    EXPECT_TRUE(excluded_wifi);

    // 动作列表与参数校验
    ASSERT_GE(facts.actions.size(), 2u);
    EXPECT_EQ(facts.actions[0].action_id, "CHECK_RESOLVER_CONFIG");
    EXPECT_EQ(facts.actions[1].action_id, "PROBE_PUBLIC_RESOLVER");
    EXPECT_EQ(facts.actions[1].params["resolver"], "223.5.5.5");
}

// 6. Golden Case 02: 网关不可达强制抑制 DNS 故障（因果优先级）
TEST(GoldenCasesTest, Case02_GatewayUnreachableSuppressesDns) {
    AssessmentSnapshot snap;
    snap.sequence_id = 102;
    snap.experience.iface = "wlan0";
    snap.experience.overall = HealthState::BAD;

    // 网关失败
    snap.experience.ip_reachability.state = HealthState::BAD;
    snap.experience.ip_reachability.reason = "frequent_or_consecutive_probe_failures";
    snap.experience.ip_reachability.evidence.push_back({"consecutive_fails", 4.0, "Gateway down"});

    // DNS 同样表现为超时
    snap.experience.dns_service.state = HealthState::BAD;
    snap.experience.dns_service.reason = "critical_burst_timeouts";
    snap.experience.dns_service.evidence.push_back({"burst_timeouts", 3.0, "Timeouts"});

    DiagnosisEngine engine;
    EvidenceIdGenerator id_gen("radxa_cubie_a7a", 1, true);
    auto facts = engine.diagnose(snap, id_gen);

    // 因果抑制：网关失败抑制上层 DNS 故障
    EXPECT_EQ(facts.fault_domain, "IP_NETWORK");
    EXPECT_EQ(facts.primary_issue, "frequent_or_consecutive_probe_failures");
    EXPECT_EQ(facts.confidence, DiagnosisConfidence::HIGH);
    EXPECT_EQ(facts.actions[0].action_id, "INSPECT_DEFAULT_GATEWAY");
}

// 7. Golden Case 04 (标志性用例): Wi-Fi 弱信号与独立 DNS 故障并发共存（不以协议层级吞并）
TEST(GoldenCasesTest, Case04_WifiWeakAndIndependentDnsFailureCoexist) {
    AssessmentSnapshot snap;
    snap.sequence_id = 104;
    snap.experience.iface = "wlan0";
    snap.experience.overall = HealthState::BAD;

    // 网关正常
    snap.experience.ip_reachability.state = HealthState::GOOD;
    snap.experience.ip_reachability.reason = "reachability_healthy";
    snap.experience.ip_reachability.evidence.push_back({"success_ratio", 1.0, "OK"});

    // Wi-Fi 信号极弱 (RSSI = -85)
    snap.experience.rf_health.state = HealthState::BAD;
    snap.experience.rf_health.reason = "weak_signal_coverage";
    snap.experience.rf_health.evidence.push_back({"rssi_dbm", -85.0, "Weak RSSI"});

    // 上游 DNS 突发超时
    snap.experience.dns_service.state = HealthState::BAD;
    snap.experience.dns_service.reason = "critical_burst_timeouts";
    snap.experience.dns_service.evidence.push_back({"burst_timeouts", 3.0, "Timeouts"});

    DiagnosisEngine engine;
    EvidenceIdGenerator id_gen("radxa_cubie_a7a", 1, true);
    auto facts = engine.diagnose(snap, id_gen);

    // 标志性断言：Protocol-layer precedence != causal precedence
    // Wi-Fi 虽更底层，但不能吞掉高优先级的 DNS 致命单杀；两者并存，DNS 为 Primary，Wi-Fi 为 Secondary
    EXPECT_EQ(facts.fault_domain, "DNS_SERVICE");
    EXPECT_EQ(facts.primary_issue, "critical_burst_timeouts");
    ASSERT_EQ(facts.secondary_issues.size(), 1u);
    EXPECT_EQ(facts.secondary_issues[0], "weak_signal_coverage");
}

// 8. 确定性稳定性校验：相同快照运行 10,000 次结果 byte-for-byte 等价
TEST(DiagnosisEngineTest, Determinism10000Runs) {
    AssessmentSnapshot snap;
    snap.sequence_id = 200;
    snap.experience.iface = "wlan0";
    snap.experience.overall = HealthState::BAD;
    snap.experience.ip_reachability.state = HealthState::GOOD;
    snap.experience.dns_service.state = HealthState::BAD;
    snap.experience.dns_service.reason = "critical_burst_timeouts";
    snap.experience.dns_service.evidence.push_back({"burst_timeouts", 3.0, "3 timeouts"});

    DiagnosisEngine engine;
    EvidenceIdGenerator id_gen("device_deterministic", 1, true);

    auto baseline = engine.diagnose(snap, id_gen);

    for (int i = 0; i < 10000; ++i) {
        auto run = engine.diagnose(snap, id_gen);
        ASSERT_EQ(run.fault_domain, baseline.fault_domain);
        ASSERT_EQ(run.primary_issue, baseline.primary_issue);
        ASSERT_EQ(run.confidence, baseline.confidence);
        ASSERT_EQ(run.evidence_refs, baseline.evidence_refs);
        ASSERT_EQ(run.actions.size(), baseline.actions.size());
    }
}

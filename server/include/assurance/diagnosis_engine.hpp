#pragma once

#include "assurance/network_experience.hpp"
#include "assessment_snapshot.hpp"
#include "assurance/evidence_id_generator.hpp"
#include "assurance/predicate_engine.hpp"
#include "assurance/action_registry.hpp"
#include "assurance/diagnosis_rules.hpp"
#include "assurance/causal_resolver.hpp"
#include <string>
#include <vector>
#include <map>
#include <memory>

namespace weaknet {

struct ExcludedCauseFact {
    std::string cause;
    std::string reason;
};

struct ActionRequest {
    int priority{1};
    std::string action_id;
    std::map<std::string, std::string> params;
};

/**
 * @brief 纯机器确定性诊断事实（DiagnosisFacts）
 * 严格无任何大模型依赖，百分之百确定可测。
 */
struct DiagnosisFacts {
    std::string fault_domain{"UNKNOWN"};
    std::string primary_issue{"no_issue_detected"};
    std::vector<std::string> secondary_issues;
    DiagnosisConfidence confidence{DiagnosisConfidence::LOW};

    std::vector<std::string> evidence_refs;
    std::vector<ExcludedCauseFact> excluded_causes;
    std::vector<ActionRequest> actions;
    std::string default_summary_template;

    std::string toJson() const {
        std::ostringstream oss;
        oss << "{\"fault_domain\":\"" << fault_domain << "\","
            << "\"primary_issue\":\"" << primary_issue << "\","
            << "\"confidence\":\"" << (confidence == DiagnosisConfidence::HIGH ? "HIGH" : (confidence == DiagnosisConfidence::MEDIUM ? "MEDIUM" : "LOW")) << "\",";

        oss << "\"secondary_issues\":[";
        for (size_t i = 0; i < secondary_issues.size(); ++i) {
            oss << "\"" << secondary_issues[i] << "\"" << (i + 1 < secondary_issues.size() ? "," : "");
        }
        oss << "],";

        oss << "\"evidence_refs\":[";
        for (size_t i = 0; i < evidence_refs.size(); ++i) {
            oss << "\"" << evidence_refs[i] << "\"" << (i + 1 < evidence_refs.size() ? "," : "");
        }
        oss << "],";

        oss << "\"excluded_causes\":[";
        for (size_t i = 0; i < excluded_causes.size(); ++i) {
            oss << "{\"cause\":\"" << excluded_causes[i].cause << "\",\"reason\":\"" << excluded_causes[i].reason << "\"}"
                << (i + 1 < excluded_causes.size() ? "," : "");
        }
        oss << "],";

        oss << "\"actions\":[";
        for (size_t i = 0; i < actions.size(); ++i) {
            oss << "{\"priority\":" << actions[i].priority
                << ",\"action_id\":\"" << actions[i].action_id << "\",\"params\":{";
            size_t p_idx = 0;
            for (const auto& [k, v] : actions[i].params) {
                oss << "\"" << k << "\":\"" << v << "\"" << (++p_idx < actions[i].params.size() ? "," : "");
            }
            oss << "}}" << (i + 1 < actions.size() ? "," : "");
        }
        oss << "],";

        oss << "\"default_summary_template\":\"" << default_summary_template << "\"}";
        return oss.str();
    }
};

class DiagnosisEngine {
public:
    explicit DiagnosisEngine(std::vector<DiagnosisRule> rules = RuleLoader::loadDefaultRules(),
                             std::shared_ptr<ActionRegistry> action_registry = std::make_shared<ActionRegistry>())
        : rules_(std::move(rules)), action_registry_(std::move(action_registry)) {}

    DiagnosisFacts diagnose(const AssessmentSnapshot& snapshot, const EvidenceIdGenerator& id_gen) const {
        DiagnosisFacts facts;

        // 1. 构建 FieldResolver
        auto resolver = buildResolver(snapshot);

        // 2. 匹配规则与收集证据引用
        std::vector<MatchedRule> matches;
        uint32_t evidence_counter = 0;

        for (const auto& r : rules_) {
            // trigger 检查
            const SleResult* sle = getSleByDomain(snapshot.experience, r.trigger_sle);
            if (!sle) continue;
            if (sle->reason != r.trigger_reason) continue;

            // preconditions 检查 (Missing != false 原则)
            if (PredicateEngine::evaluateGroup(r.preconditions, resolver) != EvalResult::True) {
                continue;
            }

            MatchedRule mr;
            mr.rule = &r;
            mr.confidence = r.base_confidence;

            // 置信度升级算法
            if (PredicateEngine::evaluateGroup(r.upgrade_to_high_if, resolver) == EvalResult::True) {
                mr.confidence = DiagnosisConfidence::HIGH;
            }

            // 绑定真实 Evidence ID
            for (size_t i = 0; i < sle->evidence.size(); ++i) {
                mr.evidence_refs.push_back(id_gen.generate(r.trigger_sle, snapshot.sequence_id, ++evidence_counter));
            }
            matches.push_back(std::move(mr));
        }

        if (matches.empty()) {
            // 无规则命中时的默认事实
            if (snapshot.experience.overall == HealthState::GOOD) {
                facts.fault_domain = "NONE";
                facts.primary_issue = "all_metrics_healthy";
                facts.confidence = DiagnosisConfidence::HIGH;
                facts.default_summary_template = "当前网络各项体验指标均在健康阈值内，无需排查。";
            } else {
                facts.fault_domain = "UNKNOWN";
                facts.primary_issue = snapshot.experience.primary_issue.value_or("unspecified_issue");
                facts.confidence = DiagnosisConfidence::LOW;
                facts.default_summary_template = "当前网络指标存在异常但缺乏明确的判定规则，建议人工关注。";
            }
            return facts;
        }

        // 3. 因果剪枝与冲突解决
        ResolvedDiagnosis resolved = CausalResolver::resolve(matches);
        const auto* primary_match = resolved.primary;

        facts.fault_domain = primary_match->rule->domain;
        facts.primary_issue = primary_match->rule->trigger_reason;
        facts.confidence = primary_match->confidence;
        facts.default_summary_template = primary_match->rule->default_summary_template;

        // 汇总证据
        for (const auto& ref : primary_match->evidence_refs) {
            facts.evidence_refs.push_back(ref);
        }

        // 机器判定排除项：严格只在 when 谓词真值成立时写入
        for (const auto& ec : primary_match->rule->excluded_candidates) {
            if (PredicateEngine::evaluateGroup(ec.when, resolver) == EvalResult::True) {
                facts.excluded_causes.push_back({ec.cause, ec.explanation});
            }
        }

        // 次要问题汇总
        for (const auto* sec : resolved.secondary) {
            facts.secondary_issues.push_back(sec->rule->trigger_reason);
            for (const auto& ref : sec->evidence_refs) {
                facts.evidence_refs.push_back(ref);
            }
        }

        // 动作生成与 ActionRegistry 验证
        for (const auto& act_ref : primary_match->rule->actions) {
            auto val_res = action_registry_->validate(act_ref.action_id, act_ref.params);
            if (val_res.ok) {
                facts.actions.push_back({act_ref.priority, act_ref.action_id, act_ref.params});
            }
        }

        return facts;
    }

private:
    static const SleResult* getSleByDomain(const NetworkExperience& exp, const std::string& sle_name) {
        if (sle_name == "ip_reachability") return &exp.ip_reachability;
        if (sle_name == "responsiveness") return &exp.responsiveness;
        if (sle_name == "reliability") return &exp.reliability;
        if (sle_name == "rf_health") return &exp.rf_health;
        if (sle_name == "dns_service") return &exp.dns_service;
        if (sle_name == "tcp_connect") return &exp.tcp_connect;
        if (sle_name == "http_access") return &exp.http_access;
        if (sle_name == "captive_portal") return &exp.captive_portal;
        if (sle_name == "active_dns") return &exp.active_dns;
        if (sle_name == "active_tcp") return &exp.active_tcp;
        if (sle_name == "active_https") return &exp.active_https;
        if (sle_name == "active_portal") return &exp.active_portal;
        return nullptr;
    }

    PredicateEngine::FieldResolver buildResolver(const AssessmentSnapshot& snapshot) const {
        return [&snapshot](const std::string& field) -> std::optional<PredicateValue> {
            const auto& exp = snapshot.experience;

            if (field == "overall_state") return healthStateToString(exp.overall);
            if (field == "link_type") {
                return (exp.iface.rfind("wl", 0) == 0) ? std::string("WIFI") : std::string("ETHERNET");
            }
            if (field == "ip_reachability.state") return healthStateToString(exp.ip_reachability.state);
            if (field == "ip_reachability.reason") return exp.ip_reachability.reason;
            if (field == "dns_service.state") return healthStateToString(exp.dns_service.state);
            if (field == "dns_service.reason") return exp.dns_service.reason;
            if (field == "rf_health.state") return healthStateToString(exp.rf_health.state);
            if (field == "rf_health.reason") return exp.rf_health.reason;

            // 提取具体 EvidenceItem metric
            for (const auto* sle : {&exp.ip_reachability, &exp.responsiveness, &exp.reliability,
                                    &exp.rf_health, &exp.dns_service, &exp.tcp_connect,
                                    &exp.http_access, &exp.captive_portal}) {
                for (const auto& item : sle->evidence) {
                    if (field == item.metric) {
                        return item.value;
                    }
                    if (field == "rf_health.rssi_dbm" && (item.metric == "rssi_dbm" || item.metric == "median_rssi_dbm")) {
                        return item.value;
                    }
                    if (field == "dns.consecutive_timeouts" && item.metric == "burst_timeouts") {
                        return item.value;
                    }
                }
            }

            return std::nullopt; // 字段缺失返回 nullopt，保证 Missing != false
        };
    }

    std::vector<DiagnosisRule> rules_;
    std::shared_ptr<ActionRegistry> action_registry_;
};

} // namespace weaknet

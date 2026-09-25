#pragma once

#include "assurance/predicate_engine.hpp"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <cctype>

namespace weaknet {

enum class DiagnosisConfidence {
    LOW,
    MEDIUM,
    HIGH
};

struct ExcludedCandidateRule {
    std::string cause;
    PredicateGroup when;
    std::string explanation;
};

struct RuleActionRef {
    std::string action_id;
    int priority{1};
    std::map<std::string, std::string> params;
};

struct DiagnosisRule {
    std::string id;
    std::string domain;
    int priority{50};
    std::vector<std::string> suppresses;
    std::vector<std::string> coexists_with;

    // 触发与前置条件
    std::string trigger_sle;
    std::string trigger_reason;
    PredicateGroup preconditions;

    // 置信度算法
    DiagnosisConfidence base_confidence{DiagnosisConfidence::MEDIUM};
    PredicateGroup upgrade_to_high_if;

    std::string hypothesis;
    std::vector<ExcludedCandidateRule> excluded_candidates;
    std::vector<RuleActionRef> actions;
    std::string default_summary_template;
};

class RuleLoader {
public:
    static DiagnosisRule createRuleDnsBurstTimeout() {
        DiagnosisRule r;
        r.id = "RULE_DNS_CRITICAL_BURST_TIMEOUTS";
        r.domain = "DNS_SERVICE";
        r.priority = 80;
        r.suppresses = {"RULE_DNS_SLOW_LATENCY"};
        r.coexists_with = {"RULE_WIFI_WEAK_SIGNAL"};
        r.trigger_sle = "dns_service";
        r.trigger_reason = "critical_burst_timeouts";

        // 前置条件: ip_reachability.state == "GOOD"
        r.preconditions.logic = PredicateGroup::Logic::All;
        r.preconditions.predicates.push_back({"ip_reachability.state", PredicateOp::Eq, std::string("GOOD")});

        // 置信度升级
        r.base_confidence = DiagnosisConfidence::MEDIUM;
        r.upgrade_to_high_if.logic = PredicateGroup::Logic::All;
        r.upgrade_to_high_if.predicates.push_back({"ip_reachability.state", PredicateOp::Eq, std::string("GOOD")});
        r.upgrade_to_high_if.predicates.push_back({"dns.consecutive_timeouts", PredicateOp::Gte, 3.0});

        r.hypothesis = "本地网关及物理链路正常，但配置的上游 DNS 解析器连续超时，无任何成功应答";

        // 机器判定排除项
        ExcludedCandidateRule ec1;
        ec1.cause = "WEAK_WIFI_SIGNAL";
        ec1.when.logic = PredicateGroup::Logic::All;
        ec1.when.predicates.push_back({"link_type", PredicateOp::Eq, std::string("WIFI")});
        ec1.when.predicates.push_back({"rf_health.rssi_dbm", PredicateOp::Gt, -65.0});
        ec1.explanation = "空口 Wi-Fi 信号极佳 (RSSI > -65dBm)";
        r.excluded_candidates.push_back(std::move(ec1));

        ExcludedCandidateRule ec2;
        ec2.cause = "GATEWAY_UNREACHABLE";
        ec2.when.logic = PredicateGroup::Logic::All;
        ec2.when.predicates.push_back({"ip_reachability.state", PredicateOp::Eq, std::string("GOOD")});
        ec2.explanation = "网关 ping 连通性正常且丢包率为 0";
        r.excluded_candidates.push_back(std::move(ec2));

        r.actions.push_back({"CHECK_RESOLVER_CONFIG", 1, {}});
        r.actions.push_back({"PROBE_PUBLIC_RESOLVER", 2, {{"resolver", "223.5.5.5"}}});
        r.default_summary_template = "检测到 DNS 域名解析服务发生突发连续超时，局域网链路可达。请依次执行建议的排查动作。";
        return r;
    }

    static DiagnosisRule createRuleGatewayUnreachable() {
        DiagnosisRule r;
        r.id = "RULE_GATEWAY_UNREACHABLE";
        r.domain = "IP_NETWORK";
        r.priority = 100;
        // 网关不可达强制因果抑制高层症状（DNS / HTTP）
        r.suppresses = {"RULE_DNS_CRITICAL_BURST_TIMEOUTS", "RULE_DNS_SLOW_LATENCY", "RULE_HTTP_SLOW"};
        r.trigger_sle = "ip_reachability";
        r.trigger_reason = "frequent_or_consecutive_probe_failures";

        r.base_confidence = DiagnosisConfidence::HIGH;
        r.hypothesis = "默认网关连续探测失败或丢包严重，本地到局域网出口链路断开";

        ExcludedCandidateRule ec1;
        ec1.cause = "DNS_SERVER_CRASH";
        ec1.when.logic = PredicateGroup::Logic::All;
        ec1.when.predicates.push_back({"ip_reachability.state", PredicateOp::Eq, std::string("BAD")});
        ec1.explanation = "IP 层连通性已完全丧失，上层 DNS/HTTP 故障均为派生症状";
        r.excluded_candidates.push_back(std::move(ec1));

        r.actions.push_back({"INSPECT_DEFAULT_GATEWAY", 1, {}});
        r.actions.push_back({"RESTART_NETWORK_INTERFACE", 2, {{"interface", "wlan0"}}});
        r.default_summary_template = "局域网网关不可达，底层 IP 链路已中断。建议检查本地网络硬件或网关。";
        return r;
    }

    static DiagnosisRule createRuleWifiWeakSignal() {
        DiagnosisRule r;
        r.id = "RULE_WIFI_WEAK_SIGNAL";
        r.domain = "PHYSICAL_LINK";
        r.priority = 40;
        // 核心原则：Protocol-layer precedence != causal precedence
        // Wi-Fi 弱信号与上游 DNS 故障独立共存，绝不轻易抑制 DNS 故障
        r.coexists_with = {"RULE_DNS_CRITICAL_BURST_TIMEOUTS"};
        r.trigger_sle = "rf_health";
        r.trigger_reason = "weak_signal_coverage";

        r.base_confidence = DiagnosisConfidence::MEDIUM;
        r.hypothesis = "Wi-Fi 空口信号强度极低 (RSSI <= -80dBm)，易产生丢包或抖动";

        r.default_summary_template = "Wi-Fi 信号强度衰减严重，可能导致连接质量劣化。建议缩减与 AP 距离。";
        return r;
    }

    static std::vector<DiagnosisRule> loadDefaultRules() {
        std::vector<DiagnosisRule> rules;
        rules.push_back(createRuleGatewayUnreachable());
        rules.push_back(createRuleDnsBurstTimeout());
        rules.push_back(createRuleWifiWeakSignal());
        validateRuleSet(rules);
        return rules;
    }

    // 说明：此前这里有一个 loadFromFile(path) —— 它打开文件后不做任何解析，
    // 无条件返回 loadDefaultRules()，即"看起来支持外部规则文件、实际永远是
    // 硬编码规则"的伪装路径。它无生产调用者（server.cpp 直接用
    // loadDefaultRules()），也无测试覆盖，属于孤儿接口，已删除。
    // 若将来确需从文件加载规则，必须连同 rules/*.yaml 的真实解析、
    // 未知语法处理与失败语义一起设计，而不是留一个静默回落默认值的空壳。

    static void validateRuleSet(const std::vector<DiagnosisRule>& rules) {
        std::set<std::string> rule_ids;
        for (const auto& r : rules) {
            if (r.id.empty()) {
                throw std::runtime_error("Rule ID cannot be empty");
            }
            if (!rule_ids.insert(r.id).second) {
                throw std::runtime_error("Duplicate rule ID: " + r.id);
            }
            if (r.domain.empty()) {
                throw std::runtime_error("Rule domain cannot be empty: " + r.id);
            }
        }

        // 检测抑制环 (Cycle Detection)
        for (const auto& r : rules) {
            for (const auto& target_id : r.suppresses) {
                if (target_id == r.id) {
                    throw std::runtime_error("Self-suppression cycle detected: " + r.id);
                }
                // 检查双向直接循环 A suppresses B and B suppresses A
                for (const auto& other : rules) {
                    if (other.id == target_id) {
                        for (const auto& other_suppress : other.suppresses) {
                            if (other_suppress == r.id) {
                                throw std::runtime_error("Direct suppression cycle between " + r.id + " and " + other.id);
                            }
                        }
                    }
                }
            }
        }
    }
};

} // namespace weaknet

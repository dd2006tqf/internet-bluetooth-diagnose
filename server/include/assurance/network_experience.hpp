#pragma once

#include "assurance/health_state.hpp"
#include "assurance/dns_types.hpp"
#include <string>
#include <vector>
#include <optional>

namespace weaknet {

struct NetworkExperience {
    std::string iface;
    HealthState overall{HealthState::UNKNOWN};
    Coverage overall_coverage{Coverage::NONE};
    AssessmentProfile assessment_profile{AssessmentProfile::INTERNET_ACCESS}; // IR-3

    SleResult ip_reachability;
    SleResult responsiveness;
    SleResult reliability;
    SleResult rf_health;   // Advisory
    SleResult dns_service;   // Phase 2A: DNS Service SLE
    SleResult tcp_connect;    // Stage 2: TCP Connect SLE
    SleResult http_access;    // Stage 2: HTTP/HTTPS Access SLE
    SleResult captive_portal; // Stage 2: Captive Portal SLE

    // 受控主动探测（host-level Internet capability）——唯一有资格
    // 判定 INTERNET_ACCESS 的服务层证据。与被动 observed experience 分开表达。
    SleResult active_dns;
    SleResult active_tcp;
    SleResult active_https;
    SleResult active_portal;

    std::vector<std::string> warnings;
    std::optional<std::string> primary_issue;
    int display_score{50}; // 仅供展示与兼容映射
};

} // namespace weaknet

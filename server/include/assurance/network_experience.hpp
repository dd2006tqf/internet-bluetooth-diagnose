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
    SleResult dns_service; // Phase 2A: Service Health SLE (HOST / RESOLVER)

    std::vector<std::string> warnings;
    std::optional<std::string> primary_issue;
    int display_score{50}; // 仅供展示与兼容映射
};

} // namespace weaknet

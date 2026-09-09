#pragma once

#include "assurance/health_state.hpp"
#include <string>
#include <vector>
#include <optional>

namespace weaknet {

struct NetworkExperience {
    std::string iface;
    HealthState overall{HealthState::UNKNOWN};
    Coverage overall_coverage{Coverage::NONE};

    SleResult ip_reachability;
    SleResult responsiveness;
    SleResult reliability;
    SleResult rf_health; // Advisory

    std::vector<std::string> warnings;
    std::optional<std::string> primary_issue;
    int display_score{50}; // 仅供展示与兼容映射
};

} // namespace weaknet

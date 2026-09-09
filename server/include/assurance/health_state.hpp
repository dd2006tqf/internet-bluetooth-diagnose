#pragma once

#include <string>
#include <vector>

namespace weaknet {

enum class HealthState {
    GOOD,
    DEGRADED,
    BAD,
    UNKNOWN
};

inline const char* healthStateToString(HealthState s) {
    switch (s) {
        case HealthState::GOOD: return "GOOD";
        case HealthState::DEGRADED: return "DEGRADED";
        case HealthState::BAD: return "BAD";
        case HealthState::UNKNOWN: return "UNKNOWN";
        default: return "UNKNOWN";
    }
}

enum class Coverage {
    FULL_FOR_PROFILE,
    PARTIAL,
    NONE
};

inline const char* coverageToString(Coverage c) {
    switch (c) {
        case Coverage::FULL_FOR_PROFILE: return "FULL_FOR_PROFILE";
        case Coverage::PARTIAL: return "PARTIAL";
        case Coverage::NONE: return "NONE";
        default: return "NONE";
    }
}

/// SLE 对当前链路形态/profile 的适用性。NOT_APPLICABLE 表示该维度在此环境下不存在
/// （如有线链路的 RF Health），不参与 OverallPolicy vote、coverage 惩罚、warning 与 primary_issue。
enum class Applicability {
    APPLICABLE,
    NOT_APPLICABLE
};

inline const char* applicabilityToString(Applicability a) {
    switch (a) {
        case Applicability::APPLICABLE: return "APPLICABLE";
        case Applicability::NOT_APPLICABLE: return "NOT_APPLICABLE";
        default: return "APPLICABLE";
    }
}

struct EvidenceItem {
    std::string metric;
    double value{0.0};
    std::string detail;
};

struct SleResult {
    HealthState state{HealthState::UNKNOWN};
    Coverage coverage{Coverage::NONE};
    std::vector<EvidenceItem> evidence;
    std::string reason;
    // 默认 APPLICABLE：未显式声明不适用的 SLE 一律按参与决策处理（安全默认）。
    Applicability applicability{Applicability::APPLICABLE};
};

} // namespace weaknet

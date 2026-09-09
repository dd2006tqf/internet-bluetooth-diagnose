#pragma once

#include "assurance/health_state.hpp"
#include <chrono>

namespace weaknet {

struct StateStabilizerConfig {
    std::chrono::milliseconds degradation_hold{10000}; // 10s 恶化保持期
    std::chrono::milliseconds recovery_hold{20000};    // 20s 恢复保持期
};

class StateStabilizer {
public:
    using Config = StateStabilizerConfig;

    explicit StateStabilizer(Config cfg = Config()) : cfg_(cfg) {}

    HealthState update(HealthState raw_state,
                       uint64_t evidence_revision,
                       std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now(),
                       bool is_critical_bypass = false) {
        if (!initialized_) {
            stable_state_ = raw_state;
            candidate_state_ = raw_state;
            candidate_since_ = now;
            last_revision_ = evidence_revision;
            initialized_ = true;
            return stable_state_;
        }

        // HR-6 铁律：没有新 evidence 到达时，不得推进 hysteresis 状态或计数
        if (evidence_revision == last_revision_ && !is_critical_bypass) {
            return stable_state_;
        }
        last_revision_ = evidence_revision;

        // 严重故障直接即时 bypass 滞后
        if (is_critical_bypass && raw_state == HealthState::BAD) {
            stable_state_ = HealthState::BAD;
            candidate_state_ = HealthState::BAD;
            candidate_since_ = now;
            return stable_state_;
        }

        if (raw_state == stable_state_) {
            candidate_state_ = raw_state;
            candidate_since_ = now;
            return stable_state_;
        }

        if (raw_state != candidate_state_) {
            candidate_state_ = raw_state;
            candidate_since_ = now;
            return stable_state_;
        }

        auto hold_duration = (isWorse(candidate_state_, stable_state_)) ? cfg_.degradation_hold : cfg_.recovery_hold;
        if (now >= candidate_since_ && (now - candidate_since_) >= hold_duration) {
            stable_state_ = candidate_state_;
        }

        return stable_state_;
    }

    HealthState stableState() const { return stable_state_; }
    void reset() { initialized_ = false; }

private:
    static bool isWorse(HealthState a, HealthState b) {
        auto score = [](HealthState s) {
            switch (s) {
                case HealthState::GOOD: return 3;
                case HealthState::DEGRADED: return 2;
                case HealthState::BAD: return 1;
                case HealthState::UNKNOWN: return 0;
            }
            return 0;
        };
        return score(a) < score(b);
    }

    Config cfg_;
    bool initialized_{false};
    HealthState stable_state_{HealthState::UNKNOWN};
    HealthState candidate_state_{HealthState::UNKNOWN};
    std::chrono::steady_clock::time_point candidate_since_{std::chrono::steady_clock::now()};
    uint64_t last_revision_{0};
};

} // namespace weaknet

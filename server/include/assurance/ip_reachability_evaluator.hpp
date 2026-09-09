#pragma once

#include "assurance/health_state.hpp"
#include "metrics/metric_sample.hpp"
#include <vector>

namespace weaknet {

struct IpReachabilityConfig {
    size_t bad_consecutive_failures{3};
    size_t degraded_consecutive_failures{1};
    double bad_ratio_threshold{0.50};     // 成功率低于 50% 判 BAD
    double degraded_ratio_threshold{0.90};  // 成功率低于 90% 判 DEGRADED
};

class IpReachabilityEvaluator {
public:
    using Config = IpReachabilityConfig;

    static SleResult evaluate(const std::vector<MetricSample>& reachability_samples,
                             const Config& cfg = Config()) {
        SleResult res;
        if (reachability_samples.empty()) {
            res.state = HealthState::UNKNOWN;
            res.coverage = Coverage::NONE;
            res.reason = "no_probe_samples";
            return res;
        }

        size_t valid_count = 0;
        size_t success_count = 0;
        size_t current_consecutive_fails = 0;
        size_t max_consecutive_fails = 0;

        for (const auto& s : reachability_samples) {
            if (s.state != MetricState::VALID) continue;
            valid_count++;
            bool ok = (s.value > 0.5);
            if (ok) {
                success_count++;
                current_consecutive_fails = 0;
            } else {
                current_consecutive_fails++;
                if (current_consecutive_fails > max_consecutive_fails) {
                    max_consecutive_fails = current_consecutive_fails;
                }
            }
        }

        if (valid_count == 0) {
            res.state = HealthState::UNKNOWN;
            res.coverage = Coverage::NONE;
            res.reason = "no_valid_probe_samples";
            return res;
        }

        double success_ratio = static_cast<double>(success_count) / static_cast<double>(valid_count);

        res.evidence.push_back({"success_ratio", success_ratio * 100.0,
                                "Probe success ratio: " + std::to_string(static_cast<int>(success_ratio * 100)) + "%"});
        res.evidence.push_back({"recent_consecutive_fails", static_cast<double>(current_consecutive_fails),
                                "Recent consecutive failed probes"});

        res.coverage = Coverage::FULL_FOR_PROFILE;

        if (current_consecutive_fails >= cfg.bad_consecutive_failures || success_ratio < cfg.bad_ratio_threshold) {
            res.state = HealthState::BAD;
            res.reason = "frequent_or_consecutive_probe_failures";
        } else if (current_consecutive_fails >= cfg.degraded_consecutive_failures || success_ratio < cfg.degraded_ratio_threshold) {
            res.state = HealthState::DEGRADED;
            res.reason = "intermittent_probe_packet_loss";
        } else {
            res.state = HealthState::GOOD;
            res.reason = "ip_path_fully_reachable";
        }

        return res;
    }
};

} // namespace weaknet

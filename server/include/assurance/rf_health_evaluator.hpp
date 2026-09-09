#pragma once

#include "assurance/health_state.hpp"
#include "metrics/metric_sample.hpp"
#include <vector>
#include <algorithm>

namespace weaknet {

struct RfHealthConfig {
    double rssi_good_dbm{-65.0};
    double rssi_bad_dbm{-80.0};
    double low_rssi_ratio_threshold{0.40};
};

class RfHealthEvaluator {
public:
    using Config = RfHealthConfig;

    static SleResult evaluate(const std::vector<MetricSample>& rssi_samples,
                             bool is_wireless,
                             const Config& cfg = Config()) {
        SleResult res;
        if (!is_wireless) {
            // 有线链路不存在 RF 维度：显式声明 NOT_APPLICABLE，OverallPolicy 将忽略该维度
            // （不 vote、不罚 coverage、不产生 warning），前端对应显示 N/A。
            res.state = HealthState::UNKNOWN;
            res.coverage = Coverage::FULL_FOR_PROFILE;
            res.reason = "not_applicable_for_wired";
            res.applicability = Applicability::NOT_APPLICABLE;
            return res;
        }

        std::vector<double> valid_rssis;
        size_t low_count = 0;
        for (const auto& s : rssi_samples) {
            if (s.state == MetricState::VALID && s.value > -999.0) {
                valid_rssis.push_back(s.value);
                if (s.value < cfg.rssi_bad_dbm) {
                    low_count++;
                }
            }
        }

        if (valid_rssis.empty()) {
            res.state = HealthState::UNKNOWN;
            res.coverage = Coverage::NONE;
            res.reason = "no_valid_rssi_samples";
            return res;
        }

        std::sort(valid_rssis.begin(), valid_rssis.end());
        double median_rssi = valid_rssis[valid_rssis.size() / 2];
        double low_ratio = static_cast<double>(low_count) / static_cast<double>(valid_rssis.size());

        res.evidence.push_back({"median_rssi_dbm", median_rssi, "Median Wi-Fi RSSI"});
        res.evidence.push_back({"low_rssi_ratio", low_ratio * 100.0, "Low signal sample ratio %"});
        res.coverage = Coverage::FULL_FOR_PROFILE;

        if (median_rssi < cfg.rssi_bad_dbm || low_ratio >= cfg.low_rssi_ratio_threshold) {
            res.state = HealthState::BAD;
            res.reason = "weak_signal_coverage";
        } else if (median_rssi <= cfg.rssi_good_dbm) {
            res.state = HealthState::DEGRADED;
            res.reason = "moderate_signal_attenuation";
        } else {
            res.state = HealthState::GOOD;
            res.reason = "strong_rf_signal";
        }

        return res;
    }
};

} // namespace weaknet

#pragma once

#include "assurance/health_state.hpp"
#include "metrics/metric_sample.hpp"
#include <vector>
#include <algorithm>

namespace weaknet {

struct ResponsivenessConfig {
    double rtt_good_ms{60.0};
    double rtt_bad_ms{150.0};
    double jitter_good_ms{15.0};
    double jitter_bad_ms{40.0};
    double bad_rtt_ratio_threshold{0.30};
    size_t min_valid_samples{4};
};

class ResponsivenessEvaluator {
public:
    using Config = ResponsivenessConfig;

    static SleResult evaluate(const std::vector<MetricSample>& rtt_samples,
                             const std::vector<MetricSample>& jitter_samples,
                             const Config& cfg = Config()) {
        SleResult res;
        std::vector<double> valid_rtts;
        valid_rtts.reserve(rtt_samples.size());

        size_t bad_rtt_count = 0;
        for (const auto& s : rtt_samples) {
            if (s.state == MetricState::VALID && s.value >= 0.0) {
                valid_rtts.push_back(s.value);
                if (s.value > cfg.rtt_bad_ms) {
                    bad_rtt_count++;
                }
            }
        }

        if (valid_rtts.size() < cfg.min_valid_samples) {
            res.state = HealthState::UNKNOWN;
            res.coverage = Coverage::PARTIAL;
            res.reason = "insufficient_rtt_samples";
            return res;
        }

        std::sort(valid_rtts.begin(), valid_rtts.end());
        double median_rtt = valid_rtts[valid_rtts.size() / 2];
        double bad_ratio = static_cast<double>(bad_rtt_count) / static_cast<double>(valid_rtts.size());

        res.evidence.push_back({"median_rtt_ms", median_rtt, "Median RTT in window"});
        res.evidence.push_back({"bad_rtt_ratio", bad_ratio * 100.0, "Ratio of RTT samples > " + std::to_string(static_cast<int>(cfg.rtt_bad_ms)) + "ms"});

        std::vector<double> valid_jitters;
        for (const auto& s : jitter_samples) {
            if (s.state == MetricState::VALID && s.value >= 0.0) {
                valid_jitters.push_back(s.value);
            }
        }
        double median_jitter = 0.0;
        bool has_jitter = false;
        if (!valid_jitters.empty()) {
            std::sort(valid_jitters.begin(), valid_jitters.end());
            median_jitter = valid_jitters[valid_jitters.size() / 2];
            has_jitter = true;
            res.evidence.push_back({"median_jitter_ms", median_jitter, "Median jitter in window"});
        }

        res.coverage = has_jitter ? Coverage::FULL_FOR_PROFILE : Coverage::PARTIAL;

        bool rtt_bad = (median_rtt > cfg.rtt_bad_ms || bad_ratio >= cfg.bad_rtt_ratio_threshold);
        bool jitter_bad = (has_jitter && median_jitter > cfg.jitter_bad_ms);

        bool rtt_good = (median_rtt <= cfg.rtt_good_ms && bad_ratio < 0.10);
        bool jitter_good = (!has_jitter || median_jitter <= cfg.jitter_good_ms);

        if (rtt_bad || jitter_bad) {
            res.state = HealthState::BAD;
            res.reason = rtt_bad ? "rtt_severely_high_or_volatile" : "jitter_excessive";
        } else if (rtt_good && jitter_good) {
            res.state = HealthState::GOOD;
            res.reason = "rtt_and_jitter_stable_and_low";
        } else {
            res.state = HealthState::DEGRADED;
            res.reason = "latency_or_jitter_suboptimal";
        }

        return res;
    }
};

} // namespace weaknet

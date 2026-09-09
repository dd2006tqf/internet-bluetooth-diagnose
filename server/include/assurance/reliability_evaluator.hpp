#pragma once

#include "assurance/health_state.hpp"
#include "metrics/metric_sample.hpp"
#include <vector>
#include <algorithm>

namespace weaknet {

struct ReliabilityConfig {
    double wifi_loss_good_max{0.5}; // %
    double wifi_loss_bad_min{3.0};   // %
    double tcp_loss_good_max{1.0};  // %
    double tcp_loss_bad_min{4.0};   // %
};

class ReliabilityEvaluator {
public:
    using Config = ReliabilityConfig;

    static SleResult evaluate(const std::vector<MetricSample>& wifi_loss_samples,
                             const std::vector<MetricSample>& tcp_loss_samples,
                             bool is_wireless,
                             const Config& cfg = Config()) {
        SleResult res;

        auto extract_median = [](const std::vector<MetricSample>& samples, double& median_out) -> MetricState {
            std::vector<double> vals;
            MetricState dominant_state = MetricState::UNAVAILABLE;
            for (const auto& s : samples) {
                if (s.state == MetricState::NOT_APPLICABLE) return MetricState::NOT_APPLICABLE;
                if (s.state == MetricState::VALID) {
                    vals.push_back(s.value);
                } else {
                    dominant_state = s.state;
                }
            }
            if (vals.empty()) return dominant_state;
            std::sort(vals.begin(), vals.end());
            median_out = vals[vals.size() / 2];
            return MetricState::VALID;
        };

        double wifi_loss_val = 0.0;
        MetricState wifi_state = is_wireless ? extract_median(wifi_loss_samples, wifi_loss_val) : MetricState::NOT_APPLICABLE;

        double tcp_loss_val = 0.0;
        MetricState tcp_state = extract_median(tcp_loss_samples, tcp_loss_val);

        if (wifi_state == MetricState::VALID) {
            res.evidence.push_back({"wifi_loss_rate", wifi_loss_val, "Wi-Fi link loss rate %"});
        }
        if (tcp_state == MetricState::VALID) {
            res.evidence.push_back({"tcp_loss_rate", tcp_loss_val, "TCP kernel retransmission loss rate %"});
        }

        bool bad_evidence = (wifi_state == MetricState::VALID && wifi_loss_val >= cfg.wifi_loss_bad_min) ||
                            (tcp_state == MetricState::VALID && tcp_loss_val >= cfg.tcp_loss_bad_min);
        if (bad_evidence) {
            res.state = HealthState::BAD;
            res.coverage = (wifi_state == MetricState::VALID && tcp_state == MetricState::VALID) ? Coverage::FULL_FOR_PROFILE : Coverage::PARTIAL;
            res.reason = "high_packet_loss_or_retransmission";
            return res;
        }

        bool degraded_evidence = (wifi_state == MetricState::VALID && wifi_loss_val > cfg.wifi_loss_good_max) ||
                                 (tcp_state == MetricState::VALID && tcp_loss_val > cfg.tcp_loss_good_max);
        if (degraded_evidence) {
            res.state = HealthState::DEGRADED;
            res.coverage = (wifi_state == MetricState::VALID && tcp_state == MetricState::VALID) ? Coverage::FULL_FOR_PROFILE : Coverage::PARTIAL;
            res.reason = "moderate_packet_loss_detected";
            return res;
        }

        bool wifi_good = (wifi_state == MetricState::NOT_APPLICABLE) || (wifi_state == MetricState::VALID && wifi_loss_val <= cfg.wifi_loss_good_max);
        bool tcp_good = (tcp_state == MetricState::VALID && tcp_loss_val <= cfg.tcp_loss_good_max);

        if (tcp_good && wifi_good) {
            res.state = HealthState::GOOD;
            res.coverage = (wifi_state == MetricState::VALID || !is_wireless) ? Coverage::FULL_FOR_PROFILE : Coverage::PARTIAL;
            res.reason = "minimal_packet_loss";
            return res;
        }

        if (tcp_state != MetricState::VALID && (wifi_state != MetricState::VALID && is_wireless)) {
            res.state = HealthState::UNKNOWN;
            res.coverage = Coverage::NONE;
            res.reason = "no_loss_metrics_available";
            return res;
        }

        res.state = HealthState::GOOD;
        res.coverage = Coverage::PARTIAL;
        res.reason = "partial_metrics_healthy";
        return res;
    }
};

} // namespace weaknet

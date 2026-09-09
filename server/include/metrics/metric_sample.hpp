#pragma once

#include "metrics/metric_types.hpp"
#include <chrono>

namespace weaknet {

struct MetricSample {
    double value{0.0};
    std::chrono::steady_clock::time_point observed_at{std::chrono::steady_clock::now()};
    MetricState state{MetricState::VALID};
    uint64_t sequence{0};

    static MetricSample valid(double val, uint64_t seq = 0) {
        MetricSample s;
        s.value = val;
        s.observed_at = std::chrono::steady_clock::now();
        s.state = MetricState::VALID;
        s.sequence = seq;
        return s;
    }

    static MetricSample unavailable(uint64_t seq = 0) {
        MetricSample s;
        s.value = 0.0;
        s.observed_at = std::chrono::steady_clock::now();
        s.state = MetricState::UNAVAILABLE;
        s.sequence = seq;
        return s;
    }

    static MetricSample notApplicable(uint64_t seq = 0) {
        MetricSample s;
        s.value = 0.0;
        s.observed_at = std::chrono::steady_clock::now();
        s.state = MetricState::NOT_APPLICABLE;
        s.sequence = seq;
        return s;
    }

    bool isValid(std::chrono::milliseconds freshness,
                 std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const {
        if (state != MetricState::VALID) return false;
        if (now < observed_at) return false;
        return (now - observed_at) <= freshness;
    }
};

} // namespace weaknet

#include "ebpf_monitor_metrics.hpp"
#include <chrono>

namespace weaknet_dbus {

namespace {
uint64_t monotonicNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
}

const char* ebpfMonitorStateName(EbpfMonitorState state) {
    switch (state) {
        case EbpfMonitorState::Uninitialized: return "uninitialized";
        case EbpfMonitorState::Initializing: return "initializing";
        case EbpfMonitorState::Attached: return "attached";
        case EbpfMonitorState::Fallback: return "fallback";
        case EbpfMonitorState::Error: return "error";
        case EbpfMonitorState::Stopped: return "stopped";
    }
    return "unknown";
}

void EbpfMonitorMetricsTracker::recordProbeAttached() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++metrics_.attachedProbes;
}

void EbpfMonitorMetricsTracker::recordReadSuccess(uint64_t elapsedUs, bool sample) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++metrics_.mapReads;
    if (sample) ++metrics_.samples;
    metrics_.totalReadTimeUs += elapsedUs;
    metrics_.averageReadTimeUs = metrics_.mapReads == 0 ? 0 : metrics_.totalReadTimeUs / metrics_.mapReads;
    lastSuccessfulSampleNs_ = monotonicNs();
    consecutiveErrors_ = 0;
}

void EbpfMonitorMetricsTracker::recordReadFailure(const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++metrics_.mapReadErrors;
    metrics_.lastError = error;
    ++consecutiveErrors_;
}

EbpfMonitorMetrics EbpfMonitorMetricsTracker::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metrics_;
}

uint64_t EbpfMonitorMetricsTracker::consecutiveErrors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return consecutiveErrors_;
}

uint64_t EbpfMonitorMetricsTracker::lastSuccessfulSampleNs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastSuccessfulSampleNs_;
}

void EbpfMonitorMetricsTracker::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_ = {};
    consecutiveErrors_ = 0;
    lastSuccessfulSampleNs_ = 0;
}

void EbpfMonitorStateSupport::setState(EbpfMonitorState state, bool available, const std::string& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = state;
    available_ = available;
    status_ = status;
}

void EbpfMonitorStateSupport::setStateFromRead(bool success, const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (success) {
        tracker_.recordReadSuccess(0);
        status_.clear();
    } else {
        tracker_.recordReadFailure(error);
        status_ = error;
    }
}

EbpfMonitorHealth EbpfMonitorStateSupport::health() const {
    std::lock_guard<std::mutex> lock(mutex_);
    EbpfMonitorHealth result;
    result.name = name_;
    result.state = state_;
    result.available = available_;
    auto metrics = tracker_.snapshot();
    result.lastSuccessfulSampleNs = tracker_.lastSuccessfulSampleNs();
    result.consecutiveErrors = tracker_.consecutiveErrors();
    result.totalErrors = metrics.mapReadErrors;
    result.status = status_;
    result.healthy = available_ && (state_ == EbpfMonitorState::Attached);
    return result;
}

EbpfMonitorState EbpfMonitorStateSupport::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool EbpfMonitorStateSupport::isAvailable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return available_;
}

void EbpfMonitorStateSupport::resetMetrics() {
    tracker_.reset();
}

}  // namespace weaknet_dbus

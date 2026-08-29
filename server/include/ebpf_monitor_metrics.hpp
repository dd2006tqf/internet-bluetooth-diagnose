#pragma once

#include "ebpf_monitor_interface.hpp"
#include <chrono>
#include <mutex>

namespace weaknet_dbus {

class EbpfMonitorMetricsTracker {
public:
    void recordProbeAttached();
    void recordReadSuccess(uint64_t elapsedUs, bool sample = true);
    void recordReadFailure(const std::string& error);
    uint64_t consecutiveErrors() const;
    uint64_t lastSuccessfulSampleNs() const;

    EbpfMonitorMetrics snapshot() const;
    void reset();

private:
    mutable std::mutex mutex_;
    EbpfMonitorMetrics metrics_;
    uint64_t consecutiveErrors_ = 0;
    uint64_t lastSuccessfulSampleNs_ = 0;

    friend class EbpfMonitorStateSupport;
};

class EbpfMonitorStateSupport {
public:
    explicit EbpfMonitorStateSupport(const char* name) : name_(name) {}

    void setState(EbpfMonitorState state, bool available, const std::string& status = "");
    void setStateFromRead(bool success, const std::string& error = "");
    EbpfMonitorHealth health() const;
    EbpfMonitorState state() const;
    bool isAvailable() const;
    void resetMetrics();
    EbpfMonitorMetrics metrics() const { return tracker_.snapshot(); }
    void recordProbeAttached() { tracker_.recordProbeAttached(); }
    void recordReadSuccess(uint64_t elapsedUs, bool sample = true) const { tracker_.recordReadSuccess(elapsedUs, sample); }
    void recordReadFailure(const std::string& error) const { tracker_.recordReadFailure(error); }

private:
    const char* name_;
    mutable std::mutex mutex_;
    EbpfMonitorState state_ = EbpfMonitorState::Uninitialized;
    bool available_ = false;
    std::string status_;
    mutable EbpfMonitorMetricsTracker tracker_;
};

}  // namespace weaknet_dbus

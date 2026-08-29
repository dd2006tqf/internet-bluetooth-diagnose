#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace weaknet_dbus {

enum class EbpfMonitorState {
    Uninitialized,
    Initializing,
    Attached,
    Fallback,
    Error,
    Stopped
};

const char* ebpfMonitorStateName(EbpfMonitorState state);

struct EbpfMonitorHealth {
    std::string name;
    EbpfMonitorState state = EbpfMonitorState::Uninitialized;
    bool available = false;
    bool healthy = false;
    uint64_t lastSuccessfulSampleNs = 0;
    uint64_t consecutiveErrors = 0;
    uint64_t totalErrors = 0;
    std::string status;
};

struct EbpfMonitorMetrics {
    uint64_t attachedProbes = 0;
    uint64_t mapReads = 0;
    uint64_t mapReadErrors = 0;
    uint64_t samples = 0;
    uint64_t totalReadTimeUs = 0;
    uint64_t averageReadTimeUs = 0;
    std::string lastError;
};

class IEbpfMonitor {
public:
    virtual ~IEbpfMonitor() = default;
    virtual const char* monitorName() const = 0;
    virtual EbpfMonitorState commonState() const = 0;
    virtual bool isAvailable() const = 0;
    virtual EbpfMonitorHealth health() const = 0;
    virtual EbpfMonitorMetrics metrics() const = 0;
    virtual void resetMetrics() = 0;
};

}  // namespace weaknet_dbus

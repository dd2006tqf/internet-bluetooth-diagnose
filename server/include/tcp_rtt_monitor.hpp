/**
 * @file tcp_rtt_monitor.hpp
 * @brief 被动 TCP 往返时延（RTT）监控器 — 用户态接口
 *
 * 通过挂载内核 tracepoint/tcp/tcp_probe，被动统计真实业务连接的内核平滑 RTT（srtt）与拥塞窗口。
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include "ebpf_monitor_interface.hpp"
#include "ebpf_monitor_metrics.hpp"
#include "utils/bpf_map_sizing.hpp"

namespace weaknet_dbus {

/// 单条连接的被动 RTT 快照
struct TcpRttConnItem {
    std::string saddr;
    std::string daddr;
    uint16_t sport = 0;
    uint16_t dport = 0;
    uint32_t srttUs = 0;
    uint32_t minRttUs = 0;
    uint32_t maxRttUs = 0;
    uint32_t sndCwnd = 0;
    uint32_t sndWnd = 0;
    uint32_t rcvWnd = 0;
    uint64_t sampleCount = 0;
    uint64_t lastTimestampNs = 0;
};

/// 全局被动 RTT 统计汇总
struct TcpRttSummary {
    uint64_t totalSamples = 0;
    uint64_t activeConns = 0;
    uint32_t avgRttUs = 0;
    uint32_t p50RttUs = 0;
    uint32_t p90RttUs = 0;
    uint32_t p99RttUs = 0;
    uint32_t minRttUs = 0;
    uint32_t maxRttUs = 0;
    std::vector<TcpRttConnItem> topConns;
};

class TcpRttMonitor : public IEbpfMonitor {
public:
    TcpRttMonitor();
    ~TcpRttMonitor() override;

    bool init(const std::string& bpfObjPath = "build/tcp_rtt.bpf.o",
              const weaknet::MapSizingPlan& plan = {});

    void stop();

    TcpRttSummary getStats();

    // ---- IEbpfMonitor 接口实现 ----
    const char* monitorName() const override { return "TcpRttMonitor"; }
    EbpfMonitorState commonState() const override;
    bool isAvailable() const override;
    EbpfMonitorHealth health() const override;
    EbpfMonitorMetrics metrics() const override;
    void resetMetrics() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace weaknet_dbus

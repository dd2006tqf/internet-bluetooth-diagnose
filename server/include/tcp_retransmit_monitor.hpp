// tcp_retransmit_monitor.hpp
// TCP 重传追踪监控器 - 用户态接口
// 从 BPF Map 读取重传事件和统计，替代 net_tcp.cpp 的 netlink dump

#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include "ebpf_monitor_interface.hpp"
#include "ebpf_monitor_metrics.hpp"

namespace weaknet_dbus {

// TCP 连接标识
struct TcpConnKey {
    uint32_t saddr;
    uint32_t daddr;
    uint16_t sport;
    uint16_t dport;

    bool operator<(const TcpConnKey& other) const;
    bool operator==(const TcpConnKey& other) const;
};

// 重传事件
struct TcpRetransEvent {
    TcpConnKey connKey;
    uint32_t pid;
    uint32_t tgid;
    uint64_t timestampNs;
    uint32_t segsOut;
    uint32_t segsRetrans;
    uint32_t sstate;
};

// 连接级重传统计
struct TcpRetransStats {
    uint64_t totalRetrans;
    uint64_t totalSegs;
    uint32_t lastState;

    double lossRate() const {
        if (totalSegs == 0) return 0.0;
        return (totalRetrans * 100.0) / totalSegs;
    }
};

// TCP 重传监控器
class TcpRetransMonitor : public IEbpfMonitor {
public:
    TcpRetransMonitor();
    ~TcpRetransMonitor();

    // 初始化（加载 BPF 对象）
    bool init(const std::string& bpfObjPath);

    // 停止并清理
    void stop();

    // 是否已初始化
    bool isInitialized() const { return initialized_; }

    // 是否可用（BPF 加载成功）
    bool isAvailable() const override { return available_; }

    const char* monitorName() const override { return "TcpRetransMonitor"; }
    EbpfMonitorState commonState() const override { return stateSupport_.state(); }
    EbpfMonitorHealth health() const override { return stateSupport_.health(); }
    EbpfMonitorMetrics metrics() const override { return stateSupport_.metrics(); }
    void resetMetrics() override { stateSupport_.resetMetrics(); }

    // 获取所有连接的重传统计
    std::map<TcpConnKey, TcpRetransStats> getStats();

    // 计算全局丢包率（替代 TcpLossMonitor::compute）
    double computeLossRate();

    // 获取重传次数最多的 N 个连接
    std::vector<std::pair<TcpConnKey, TcpRetransStats>> getTopRetransConnections(size_t topN);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    bool initialized_ = false;
    bool available_ = false;
    EbpfMonitorStateSupport stateSupport_{"TcpRetransMonitor"};
};

}  // namespace weaknet_dbus
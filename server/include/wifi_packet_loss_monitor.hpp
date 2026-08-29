// wifi_packet_loss_monitor.hpp
// Wi-Fi/网卡收发丢包归因监控器 - 用户态接口
// 从 BPF Map 读取收发/丢弃/重传统计，区分发送/接收丢包

#pragma once

#include <string>
#include <map>
#include <memory>
#include "ebpf_monitor_interface.hpp"
#include "ebpf_monitor_metrics.hpp"

namespace weaknet_dbus {

// 接口收发统计
struct IfacePacketStats {
    uint32_t ifindex;
    uint64_t rxPkts;
    uint64_t rxBytes;
    uint64_t txPkts;
    uint64_t txBytes;
    uint64_t txDrops;
    uint64_t txRetries;

    // 发送丢包率 = txDrops / txPkts
    double txLossRate() const {
        if (txPkts == 0) return 0.0;
        return (txDrops * 100.0) / txPkts;
    }
};

// Wi-Fi/网卡丢包归因监控器
class WifiPacketLossMonitor : public IEbpfMonitor {
public:
    WifiPacketLossMonitor();
    ~WifiPacketLossMonitor();

    // 初始化（加载 BPF 对象）
    bool init(const std::string& bpfObjPath);

    // 停止并清理
    void stop();

    // 是否已初始化
    bool isInitialized() const { return initialized_; }

    // 是否可用（BPF 加载成功）
    bool isAvailable() const override { return available_; }

    const char* monitorName() const override { return "WifiPacketLossMonitor"; }
    EbpfMonitorState commonState() const override { return stateSupport_.state(); }
    EbpfMonitorHealth health() const override { return stateSupport_.health(); }
    EbpfMonitorMetrics metrics() const override { return stateSupport_.metrics(); }
    void resetMetrics() override { stateSupport_.resetMetrics(); }

    // 获取所有接口的收发统计
    std::map<uint32_t, IfacePacketStats> getStats();

    // 获取指定接口的统计
    bool getStats(uint32_t ifindex, IfacePacketStats* out);

    // 丢包归因分析
    struct LossAttribution {
        std::string ifaceName;
        double rxLossRate;
        double txLossRate;
        uint64_t txRetries;
        std::string analysis;
    };
    LossAttribution analyze(uint32_t ifindex, const std::string& ifaceName);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    bool initialized_ = false;
    bool available_ = false;
    EbpfMonitorStateSupport stateSupport_{"WifiPacketLossMonitor"};
};

}  // namespace weaknet_dbus
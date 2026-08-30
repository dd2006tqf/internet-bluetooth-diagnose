/**
 * @file wifi_packet_loss_monitor.hpp
 * @brief Wi-Fi/网卡收发丢包归因监控器 — 用户态接口
 *
 * 从 eBPF Map（wifi_packet_loss.bpf.c）读取接口级收发/丢弃/重传统计，
 * 区分发送丢包 vs 接收丢包，帮助定位"是 Wi-Fi 信号差还是对端不响应"。
 *
 * 发送丢包（txDrops）：内核 qdisc 队列溢出 / 网卡驱动主动丢弃
 * 接收丢包（rxPkts 不足）：信号干扰 / 缓冲区溢出 / 对端没发
 *
 * 挂点：kprobe/dev_queue_xmit（发送）、tracepoint/net/netif_receive（接收）
 * Maps：iface_stats（ifindex → 收发统计）
 *
 * 降级策略：eBPF 不可用时 init() 返回 false，isAvailable() 为 false。
 */

#pragma once

#include <string>
#include <map>
#include <memory>
#include "ebpf_monitor_interface.hpp"
#include "ebpf_monitor_metrics.hpp"

namespace weaknet_dbus {

/// 接口级收发统计（从内核态 iface_stats map 取出）
struct IfacePacketStats {
    uint32_t ifindex;          ///< 接口索引号（net_iface 系统编号）
    uint64_t rxPkts;           ///< 接收包数
    uint64_t rxBytes;          ///< 接收字节数
    uint64_t txPkts;           ///< 发送包数
    uint64_t txBytes;          ///< 发送字节数
    uint64_t txDrops;          ///< 发送丢弃数（qdisc + driver）
    uint64_t txRetries;        ///< 发送重传次数

    /**
     * @brief 计算发送丢包率
     * @return 丢包率百分比（txDrops / txPkts * 100）；无发送时返回 0.0
     */
    double txLossRate() const {
        if (txPkts == 0) return 0.0;
        return (txDrops * 100.0) / txPkts;
    }
};

/**
 * @brief Wi-Fi/网卡丢包归因监控器
 *
 * 继承 IEbpfMonitor，统一健康/指标查询。
 * 使用 Pimpl 模式隐藏 libbpf 细节。
 */
class WifiPacketLossMonitor : public IEbpfMonitor {
public:
    WifiPacketLossMonitor();
    ~WifiPacketLossMonitor();

    bool init(const std::string& bpfObjPath);
    void stop();

    bool isInitialized() const { return initialized_; }
    bool isAvailable() const override { return available_; }

    const char* monitorName() const override { return "WifiPacketLossMonitor"; }
    EbpfMonitorState commonState() const override { return stateSupport_.state(); }
    EbpfMonitorHealth health() const override { return stateSupport_.health(); }
    EbpfMonitorMetrics metrics() const override { return stateSupport_.metrics(); }
    void resetMetrics() override { stateSupport_.resetMetrics(); }

    /// 获取所有接口的收发统计（ifindex → 统计）
    std::map<uint32_t, IfacePacketStats> getStats();

    /**
     * @brief 获取指定接口的统计
     * @param ifindex 接口索引号
     * @param out     输出参数
     * @return true 找到；false 接口无记录
     */
    bool getStats(uint32_t ifindex, IfacePacketStats* out);

    /**
     * @brief 丢包归因分析
     *
     * 综合 txLossRate / txRetries / txPkts，给出归因结论：
     *   - 发送端拥塞（txDrops 高）
     *   - 接收端干扰（txRetries 高但 txDrops 正常）
     *   - 无明显问题（两者都低）
     *
     * @param ifindex  接口索引号
     * @param ifaceName 接口名（用于日志和诊断）
     */
    struct LossAttribution {
        std::string ifaceName;
        double rxLossRate;     ///< 接收丢包率（估算值）
        double txLossRate;     ///< 发送丢包率
        uint64_t txRetries;    ///< 发送重传次数
        std::string analysis;  ///< "发送端拥塞" / "接收端干扰" / "正常"
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

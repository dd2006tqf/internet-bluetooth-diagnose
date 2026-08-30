/**
 * @file tcp_retransmit_monitor.hpp
 * @brief TCP 重传追踪监控器 — 用户态接口
 *
 * 从 eBPF Map（tcp_retransmit.bpf.c）读取重传事件，
 * 替代 TcpLossMonitor（Phase 1，/proc/net/snmp 全局统计），
 * 提供连接粒度的重传追踪。
 *
 * 优势对比：
 *   - Phase 1（/proc/net/snmp）：只有全局计数器，无法区分"哪个连接丢包"
 *   - Phase 2（eBPF）：按 (src,dst,sport,dport) 四元组追踪，可定位问题连接
 *
 * 挂点：kprobe/tcp_retransmit_skb
 * Maps：tcp_retrans_stats（conn_key → 累计重传统计）
 *
 * 注意：TcpConnKey 使用 IPv4 网络字节序存储地址，
 *       调用方在比较/打印时需转换为主机字节序。
 */

#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include "ebpf_monitor_interface.hpp"
#include "ebpf_monitor_metrics.hpp"

namespace weaknet_dbus {

/// TCP 连接四元组 key（用于 map 查找，网络字节序）
struct TcpConnKey {
    uint32_t saddr;     ///< 源 IP（网络字节序）
    uint32_t daddr;     ///< 目标 IP（网络字节序）
    uint16_t sport;     ///< 源端口（网络字节序）
    uint16_t dport;     ///< 目标端口（网络字节序）

    bool operator<(const TcpConnKey& other) const;
    bool operator==(const TcpConnKey& other) const;
};

/// 单次重传事件（可选，用于事件流上报）
struct TcpRetransEvent {
    TcpConnKey connKey;      ///< 发生重传的连接
    uint32_t pid;            ///< 关联进程 ID
    uint32_t tgid;           ///< 线程组 ID
    uint64_t timestampNs;   ///< 事件时间戳（CLOCK_MONOTONIC ns）
    uint32_t segsOut;       ///< 发送出去的总段数（采样时快照）
    uint32_t segsRetrans;   ///< 重传段数（采样时快照）
    uint32_t sstate;         ///< TCP socket state（用于诊断连接状态）
};

/// 连接级重传统计
struct TcpRetransStats {
    uint64_t totalRetrans;       ///< 累计重传段数
    uint64_t totalSegs;         ///< 累计发送段数
    uint32_t lastState;          ///< 上次采样时的 TCP socket state

    /**
     * @brief 计算该连接的丢包率
     * @return 丢包率百分比；无发送段时返回 0.0 避免除零
     */
    double lossRate() const {
        if (totalSegs == 0) return 0.0;
        return (totalRetrans * 100.0) / totalSegs;
    }
};

/**
 * @brief TCP 重传监控器
 *
 * 继承 IEbpfMonitor，统一健康/指标查询。
 * 使用 Pimpl 模式隐藏 libbpf 细节。
 */
class TcpRetransMonitor : public IEbpfMonitor {
public:
    TcpRetransMonitor();
    ~TcpRetransMonitor();

    bool init(const std::string& bpfObjPath);
    void stop();

    bool isInitialized() const { return initialized_; }
    bool isAvailable() const override { return available_; }

    const char* monitorName() const override { return "TcpRetransMonitor"; }
    EbpfMonitorState commonState() const override { return stateSupport_.state(); }
    EbpfMonitorHealth health() const override { return stateSupport_.health(); }
    EbpfMonitorMetrics metrics() const override { return stateSupport_.metrics(); }
    void resetMetrics() override { stateSupport_.resetMetrics(); }

    /// 获取所有连接的重传统计（conn_key → 统计）
    std::map<TcpConnKey, TcpRetransStats> getStats();

    /**
     * @brief 计算全局丢包率
     *
     * 替代 TcpLossMonitor::compute()（Phase 1 全局版本）。
     * 对所有连接求和后计算：sum(totalRetrans) / sum(totalSegs) * 100。
     */
    double computeLossRate();

    /**
     * @brief 获取重传次数最多的 N 个连接
     * @param topN 返回前 N 个（无连接时返回空）
     */
    std::vector<std::pair<TcpConnKey, TcpRetransStats>> getTopRetransConnections(size_t topN);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    bool initialized_ = false;
    bool available_ = false;
    EbpfMonitorStateSupport stateSupport_{"TcpRetransMonitor"};
};

}  // namespace weaknet_dbus

/**
 * @file dns_monitor.hpp
 * @brief DNS 监控器 — 用户态接口
 *
 * 从 eBPF Map（dns_monitor.bpf.c）读取 DNS 查询/响应记录，
 * 计算解析延迟（从查询发出到收到响应的时间差）。
 *
 * 挂点：tracepoint/net/netif_receive（或 kprobe/udp_recvmsg）
 * Maps：dns_events（最近 N 条记录环形缓冲区）、dns_stats（聚合统计）
 *
 * 降级策略：eBPF 加载失败时 init() 返回 false，可用率为 false。
 *           调用方需检查 isAvailable() 后再调用 getStats()。
 */

#pragma once

#include <string>
#include <vector>
#include <memory>
#include "ebpf_monitor_interface.hpp"
#include "ebpf_monitor_metrics.hpp"

namespace weaknet_dbus {

/// 单条 DNS 查询记录（从内核态 events map 取出）
struct DnsRecord {
    std::string queryIp;   ///< DNS 服务器 IP（目标地址）
    std::string domain;    ///< 查询域名（简化提取，完整解析需 DNS 协议解析器）
    uint64_t latencyMs;    ///< 解析延迟（毫秒）
    bool     isTimeout;    ///< 是否超时（未收到响应，由内核态标记）
    uint8_t  rcode;        ///< DNS 响应码（0=无错误，3=NXDOMAIN 等）
    uint64_t timestamp;    ///< 查询时间戳（CLOCK_MONOTONIC ns）
};

/// DNS 聚合统计（定期从内核态 stats map 累加）
struct DnsAggStats {
    uint64_t totalQueries;     ///< 累计查询数
    uint64_t totalResponses;   ///< 累计响应数
    uint64_t totalTimeouts;    ///< 累计超时数
    uint64_t totalErrors;      ///< 累计错误响应（rcode != 0）
    uint64_t avgLatencyMs;     ///< 平均解析延迟（毫秒）
    uint64_t maxLatencyMs;     ///< 最大解析延迟（毫秒）

    /**
     * @brief 计算 DNS 超时率
     * @return 超时率百分比（totalTimeouts / totalQueries * 100）；
     *         无查询时返回 0.0 避免除零
     */
    double   timeoutRate() const {
        if (totalQueries == 0) return 0.0;
        return (totalTimeouts * 100.0) / totalQueries;
    }
};

/**
 * @brief DNS 监控器
 *
 * 继承 IEbpfMonitor，统一健康/指标查询。
 * 使用 Pimpl 模式隐藏 libbpf 细节，减少头文件依赖。
 */
class DnsMonitor : public IEbpfMonitor {
public:
    DnsMonitor();
    ~DnsMonitor();

    /**
     * @brief 初始化（加载 BPF 对象并挂载 tracepoint）
     * @param bpfObjPath BPF 对象文件路径
     * @return true 加载成功；false 加载失败（降级为离线模式）
     */
    bool init(const std::string& bpfObjPath);

    /// 停止并清理（卸载 eBPF 程序，关闭 BPF 对象）
    void stop();

    /// 是否已初始化（init 被调用过，无论成功与否）
    bool isInitialized() const { return initialized_; }

    /// eBPF 是否可用（挂载成功）
    bool isAvailable() const override { return available_; }

    // ---- IEbpfMonitor 实现 ----
    const char* monitorName() const override { return "DnsMonitor"; }
    EbpfMonitorState commonState() const override { return stateSupport_.state(); }
    EbpfMonitorHealth health() const override { return stateSupport_.health(); }
    EbpfMonitorMetrics metrics() const override { return stateSupport_.metrics(); }
    void resetMetrics() override { stateSupport_.resetMetrics(); }

    /// 获取当前 DNS 聚合统计
    DnsAggStats getStats();

    /// 获取平均 DNS 延迟（毫秒）
    double getAvgLatencyMs();

    /// 获取 DNS 超时率（百分比）
    double getTimeoutRate();

private:
    struct Impl;                    ///< Pimpl：隐藏 libbpf 实现细节
    std::unique_ptr<Impl> impl_;

    bool initialized_ = false;
    bool available_ = false;
    EbpfMonitorStateSupport stateSupport_{"DnsMonitor"};
};

}  // namespace weaknet_dbus

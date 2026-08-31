/**
 * @file tcp_conn_monitor.hpp
 * @brief TCP 连接生命周期监控器 — 用户态接口
 *
 * 从 eBPF Map（tcp_conn_stats.bpf.c）读取 TCP 入向连接的生命周期统计：
 * accept/close 计数、当前活跃入向连接数、连接时长分布、每端口接受/关闭数。
 *
 * 挂点：kretprobe/inet_csk_accept（入向连接建立）
 *       kprobe/tcp_close（被跟踪连接关闭）
 * Maps：conn_start（连接起始时间）、conn_stats（全局聚合）、conn_ports（每端口计数）
 *
 * 与现有监控的边界：flow_rate / tcp_retransmit 统计流量与重传维度，
 * 本监控器统计连接生命周期维度，互不重叠。
 *
 * 降级策略：eBPF 加载失败时 init() 返回 false 并置 Fallback/Error 状态，
 *           调用方需检查 isAvailable() 后再调用 getStats()。
 */

#pragma once

#include <string>
#include <vector>
#include <memory>
#include "ebpf_monitor_interface.hpp"
#include "ebpf_monitor_metrics.hpp"

namespace weaknet_dbus {

/// 连接时长分桶数量（与 tcp_conn_stats.bpf.c 的 HIST_BUCKET_COUNT 一致）
constexpr int kTcpConnHistBuckets = 7;

/// 连接时长分桶标签（下标与内核态 duration_bucket() 一致）
inline const char* tcpConnHistBucketLabel(int idx)
{
    static const char* kLabels[kTcpConnHistBuckets] = {
        "<100ms", "<1s", "<10s", "<1min", "<5min", "<1h", ">=1h",
    };
    return (idx >= 0 && idx < kTcpConnHistBuckets) ? kLabels[idx] : "?";
}

/// 单端口 accept/close 计数（从 conn_ports map 读取）
struct TcpConnPortStats {
    uint16_t port;      ///< 本地监听端口（主机序）
    uint64_t accepts;   ///< 该端口累计 accept 数
    uint64_t closes;    ///< 该端口累计被跟踪连接关闭数
};

/// TCP 连接生命周期聚合统计（定期从内核态 map 读取）
struct TcpConnAggStats {
    uint64_t totalAccepts = 0;        ///< 累计入向连接 accept 数
    uint64_t totalAcceptFailures = 0; ///< accept 返回错误指针次数（客户端中止等）
    uint64_t acceptsV4 = 0;           ///< IPv4 入向连接数
    uint64_t acceptsV6 = 0;           ///< IPv6 入向连接数
    uint64_t totalCloses = 0;         ///< 累计被跟踪入向连接关闭数
    int64_t activeInbound = 0;        ///< 当前活跃入向连接数（accept - tracked close）
    uint64_t avgDurationMs = 0;       ///< 已关闭连接平均时长（毫秒）
    uint64_t maxDurationMs = 0;       ///< 已关闭连接最大时长（毫秒）
    uint64_t durationCount = 0;       ///< 已完成时长统计的连接数
    uint64_t hist[kTcpConnHistBuckets] = {};  ///< 连接时长分桶计数
    std::vector<TcpConnPortStats> topPorts;   ///< 每端口计数（按 accept 数降序，最多前 8）
};

/**
 * @brief TCP 连接生命周期监控器
 *
 * 继承 IEbpfMonitor，统一健康/指标查询。
 * 使用 Pimpl 模式隐藏 libbpf 细节，减少头文件依赖。
 */
class TcpConnMonitor : public IEbpfMonitor {
public:
    TcpConnMonitor();
    ~TcpConnMonitor();

    /**
     * @brief 初始化（加载 BPF 对象并挂载 kprobe/kretprobe）
     * @param bpfObjPath BPF 对象文件路径（通常为 "build/tcp_conn_stats.bpf.o"）
     * @return true 加载成功；false 加载失败（状态置 Error/Fallback）
     */
    bool init(const std::string& bpfObjPath);

    /// 停止并清理（卸载 eBPF 程序，关闭 BPF 对象）
    void stop();

    /// 是否已初始化（init 被调用过，无论成功与否）
    bool isInitialized() const { return initialized_; }

    /// eBPF 是否可用（挂载成功）
    bool isAvailable() const override { return available_; }

    // ---- IEbpfMonitor 实现 ----
    const char* monitorName() const override { return "TcpConnMonitor"; }
    EbpfMonitorState commonState() const override { return stateSupport_.state(); }
    EbpfMonitorHealth health() const override { return stateSupport_.health(); }
    EbpfMonitorMetrics metrics() const override { return stateSupport_.metrics(); }
    void resetMetrics() override { stateSupport_.resetMetrics(); }

    /// 读取当前连接生命周期聚合统计（同时更新监控指标）
    TcpConnAggStats getStats();

private:
    struct Impl;                    ///< Pimpl：隐藏 libbpf 实现细节
    std::unique_ptr<Impl> impl_;

    bool initialized_ = false;
    bool available_ = false;
    EbpfMonitorStateSupport stateSupport_{"TcpConnMonitor"};
};

}  // namespace weaknet_dbus

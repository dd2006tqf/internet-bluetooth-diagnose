/**
 * @file http_latency_monitor.hpp
 * @brief HTTP 请求级延迟监控器 — 用户态接口
 *
 * 从 eBPF Map（http_latency.bpf.c）读取 HTTP 事务记录，
 * 计算 TTFB（首字节延迟），并区分"网络慢" vs "应用慢"。
 *
 * 判定逻辑：
 *   TTFB 低但 Total Time 高 → 应用慢（服务端处理/渲染耗时）
 *   TTFB 高                 → 网络慢（连接建立 + RTT 叠加）
 *   TTFB 和 Total 都低       → 正常
 *
 * 挂点：kprobe/tcp_sendmsg（请求发出）、kprobe/tcp_recvmsg（响应接收）
 * Maps：http_events（最近 N 条事务环形缓冲区）
 *
 * 注意：只能监控明文 HTTP（端口 80），HTTPS（443）由于 TLS 加密无法解析。
 */

#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include "ebpf_monitor_interface.hpp"
#include "ebpf_monitor_metrics.hpp"

namespace weaknet_dbus {

/// 单次 HTTP 事务（从请求发出到响应完成）
struct HttpTxnInfo {
    std::string srcIp;        ///< 源 IP（客户端）
    std::string dstIp;        ///< 目标 IP（服务器）
    uint16_t srcPort;         ///< 源端口
    uint16_t dstPort;         ///< 目标端口（通常 80）
    uint64_t ttfbNs;          ///< TTFB（首字节延迟，纳秒）
    uint32_t reqBytes;        ///< 请求体大小
    uint32_t respBytes;       ///< 响应体大小
    uint16_t statusCode;      ///< HTTP 状态码（200=OK, 301=重定向, 404=未找到...）
};

/// HTTP 延迟聚合统计（分位数 + 诊断分析）
struct HttpLatencyStats {
    uint64_t p50Ns;           ///< 50 分位数延迟（纳秒，中位数）
    uint64_t p95Ns;           ///< 95 分位数延迟
    uint64_t p99Ns;           ///< 99 分位数延迟
    uint64_t maxNs;           ///< 最大延迟
    uint64_t totalTxns;       ///< 聚合窗口内事务总数
    std::string analysis;     ///< "主要网络慢" / "主要应用慢" / "正常" / "数据不足"
};

/**
 * @brief HTTP 请求级延迟监控器
 *
 * 继承 IEbpfMonitor，统一健康/指标查询。
 * 使用 Pimpl 模式隐藏 libbpf 细节。
 */
class HttpLatencyMonitor : public IEbpfMonitor {
public:
    HttpLatencyMonitor();
    ~HttpLatencyMonitor();

    bool init(const std::string& bpfObjPath);
    void stop();

    bool isInitialized() const { return initialized_; }
    bool isAvailable() const override { return available_; }

    const char* monitorName() const override { return "HttpLatencyMonitor"; }
    EbpfMonitorState commonState() const override { return stateSupport_.state(); }
    EbpfMonitorHealth health() const override { return stateSupport_.health(); }
    EbpfMonitorMetrics metrics() const override { return stateSupport_.metrics(); }
    void resetMetrics() override { stateSupport_.resetMetrics(); }

    /**
     * @brief 获取最近完成的 HTTP 事务
     * @param limit 最大返回条数（防止 UI 卡死）
     */
    std::vector<HttpTxnInfo> getRecentTxns(size_t limit);

    /// 按目标 IP 聚合 TTFB 统计（定位"哪个服务器慢"）
    std::map<std::string, HttpLatencyStats> getByDstHost();

    /// 全局 TTFB 分位数
    HttpLatencyStats getGlobalStats();

private:
    /// 从排序后的样本列表中计算分位数（线性插值）
    uint64_t percentile(const std::vector<uint64_t>& values, double p);

    struct Impl;
    std::unique_ptr<Impl> impl_;

    bool initialized_ = false;
    bool available_ = false;
    EbpfMonitorStateSupport stateSupport_{"HttpLatencyMonitor"};
};

}  // namespace weaknet_dbus

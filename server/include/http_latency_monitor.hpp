// http_latency_monitor.hpp
// HTTP 请求级延迟监控器 - 用户态接口
// 从 BPF Map 读取 HTTP 事务记录，计算 TTFB（首字节延迟），区分应用慢 vs 网络慢

#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>

namespace weaknet_dbus {

// 单次 HTTP 事务
struct HttpTxnInfo {
    std::string srcIp;
    std::string dstIp;
    uint16_t srcPort;
    uint16_t dstPort;
    uint64_t ttfbNs;        // TTFB（首字节延迟）
    uint32_t reqBytes;
    uint32_t respBytes;
    uint16_t statusCode;
};

// 聚合统计
struct HttpLatencyStats {
    uint64_t p50Ns;
    uint64_t p95Ns;
    uint64_t p99Ns;
    uint64_t maxNs;
    uint64_t totalTxns;
    std::string analysis;   // "主要网络慢" / "主要应用慢" / "正常"
};

// HTTP 请求级延迟监控器
class HttpLatencyMonitor {
public:
    HttpLatencyMonitor();
    ~HttpLatencyMonitor();

    // 初始化（加载 BPF 对象）
    bool init(const std::string& bpfObjPath);

    // 停止并清理
    void stop();

    // 是否已初始化
    bool isInitialized() const { return initialized_; }

    // 是否可用（BPF 加载成功）
    bool isAvailable() const { return available_; }

    // 获取最近完成的 HTTP 事务
    std::vector<HttpTxnInfo> getRecentTxns(size_t limit);

    // 按目标 IP 聚合 TTFB 统计
    std::map<std::string, HttpLatencyStats> getByDstHost();

    // 全局 TTFB 分位数
    HttpLatencyStats getGlobalStats();

private:
    // 计算分位数
    uint64_t percentile(const std::vector<uint64_t>& values, double p);

    struct Impl;
    std::unique_ptr<Impl> impl_;

    bool initialized_ = false;
    bool available_ = false;
};

}  // namespace weaknet_dbus
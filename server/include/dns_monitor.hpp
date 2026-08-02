// dns_monitor.hpp
// DNS 监控器 - 用户态接口
// 从 BPF Map 读取 DNS 查询记录，计算解析延迟

#pragma once

#include <string>
#include <vector>
#include <memory>

namespace weaknet_dbus {

// DNS 查询记录
struct DnsRecord {
    std::string queryIp;   // DNS 服务器 IP
    std::string domain;    // 查询域名（简化，需要从 payload 提取）
    uint64_t latencyMs;    // 延迟（毫秒）
    bool     isTimeout;    // 是否超时
    uint8_t  rcode;        // DNS 响应码（0=无错误）
    uint64_t timestamp;    // 查询时间戳
};

// DNS 聚合统计
struct DnsAggStats {
    uint64_t totalQueries;
    uint64_t totalResponses;
    uint64_t totalTimeouts;
    uint64_t totalErrors;
    uint64_t avgLatencyMs;
    uint64_t maxLatencyMs;
    double   timeoutRate() const {
        if (totalQueries == 0) return 0.0;
        return (totalTimeouts * 100.0) / totalQueries;
    }
};

// DNS 监控器
class DnsMonitor {
public:
    DnsMonitor();
    ~DnsMonitor();

    // 初始化（加载 BPF 对象）
    bool init(const std::string& bpfObjPath);

    // 停止并清理
    void stop();

    // 是否已初始化
    bool isInitialized() const { return initialized_; }

    // 是否可用
    bool isAvailable() const { return available_; }

    // 获取聚合统计
    DnsAggStats getStats();

    // 获取平均 DNS 延迟（毫秒）
    double getAvgLatencyMs();

    // 获取 DNS 超时率
    double getTimeoutRate();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    bool initialized_ = false;
    bool available_ = false;
};

}  // namespace weaknet_dbus
/**
 * @file traffic_anomaly_detector.h
 * @brief 高级流量异常检测器（统计分析 + 模式识别）
 *
 * 在 NetTrafficAnalyzer 基础上提供更深入的异常检测能力：
 *
 *   - 流量模式分析：按流维护时间序列，计算均值/标准差，识别统计学异常
 *   - 偷跑流量检测：识别低频突发 + 大数据量 + 可疑目标 IP 的组合模式
 *   - 异常连接模式：高频短连接 / 并发连接数爆炸
 *   - 时间模式异常：深夜异常流量（非用户正常活动时间）
 *
 * 算法核心：Z-Score + 百分位 + 领域规则组合判定。
 *
 * 线程安全：patternsMutex_ 保护 trafficPatterns_；
 *           所有 analyze*/detect* 方法内部加锁，可多线程并发调用。
 */

#pragma once

#include "net_traffic.h"
#include <vector>
#include <map>
#include <chrono>
#include <mutex>
#include <algorithm>
#include <cmath>

/// 流量模式历史（用于统计异常检测）
struct TrafficPattern {
    std::string flowKey;                    ///< 流唯一标识（src:sport→dst:dport proto）
    std::vector<uint64_t> bpsHistory;       ///< 历史字节速率序列
    std::vector<uint64_t> ppsHistory;       ///< 历史包速率序列
    std::chrono::system_clock::time_point firstSeen;    ///< 首次出现时间
    std::chrono::system_clock::time_point lastSeen;     ///< 最近出现时间
    uint64_t totalBytes = 0;                ///< 累计字节数
    uint64_t totalPackets = 0;              ///< 累计包数
    double avgBps = 0.0;                    ///< 平均字节速率（定期更新）
    double stdDevBps = 0.0;                 ///< 速率标准差（用于 Z-Score 计算）
    bool isSuspicious = false;              ///< 上次检测是否标记为可疑
};

/// 高级异常检测结果（比 TrafficAnomaly 更详细）
struct AdvancedAnomaly {
    std::string flowKey;
    std::string anomalyType;              ///< "burst" / "data_exfil" / "suspicious_conn" / "temporal"
    double confidence;                    ///< 置信度 0.0-1.0（算法确信程度）
    double severity;                      ///< 严重程度 0.0-1.0（业务影响）
    std::string description;              ///< 人类可读描述
    std::map<std::string, double> metrics; ///< 各种诊断指标（Z-Score、stdDev 倍数等）
    std::chrono::system_clock::time_point timestamp;
};

/**
 * @brief 高级流量异常检测器
 *
 * 与 NetTrafficAnalyzer 的关系：
 *   NetTrafficAnalyzer 负责底层 eBPF 采样，
 *   TrafficAnomalyDetector 负责上层模式识别和异常判定。
 *   两者可独立使用，也可组合。
 */
class TrafficAnomalyDetector {
public:
    TrafficAnomalyDetector();
    ~TrafficAnomalyDetector() = default;

    /**
     * @brief 分析流量模式并检测异常（主入口）
     * @param flows  当前采样的流列表（来自 NetTrafficAnalyzer::sampleTopFlows）
     * @return 检测到的异常列表（可能为空）
     */
    std::vector<AdvancedAnomaly> analyzeTrafficPatterns(const std::vector<FlowRate>& flows);

    /**
     * @brief 检测偷跑流量行为（数据外泄模式）
     *
     * 特征：低活跃周期 + 突发性大流量 + 目标 IP 不在常用服务范围内。
     */
    std::vector<AdvancedAnomaly> detectDataExfiltration(const std::vector<FlowRate>& flows);

    /**
     * @brief 检测异常连接模式
     *
     * 特征：短时间内大量新建连接（端口扫描）、高频并发连接（DDoS 嫌疑）。
     */
    std::vector<AdvancedAnomaly> detectSuspiciousConnections(const std::vector<FlowRate>& flows);

    /**
     * @brief 检测时间模式异常
     *
     * 特征：正常静默时段（如凌晨 2-5 点）出现非零流量。
     */
    std::vector<AdvancedAnomaly> detectTemporalAnomalies(const std::vector<FlowRate>& flows);

    /// 获取当前流量模式统计快照（供 UI 展示）
    std::map<std::string, TrafficPattern> getTrafficPatterns();

    /**
     * @brief 设置检测参数
     * @param burstThreshold    突发阈值倍数（相对于均值的 stdDev 倍数）
     * @param volumeThreshold   流量阈值倍数
     * @param timeThreshold     时间异常阈值（分钟）
     */
    void setDetectionParams(double burstThreshold, double volumeThreshold, double timeThreshold);

    /// 清空所有历史数据（适配器重启、周期性归零）
    void clearHistory();

private:
    mutable std::mutex patternsMutex_;    ///< 保护 trafficPatterns_
    std::map<std::string, TrafficPattern> trafficPatterns_;

    // ---- 可配置检测参数 ----
    double burstThreshold_;      ///< 突发阈值倍数（默认 3.0σ）
    double volumeThreshold_;    ///< 流量阈值倍数
    double timeThreshold_;      ///< 时间异常阈值

    // ---- 内部检测方法 ----

    /// 将新采样追加到历史模式（维护 bpsHistory/ppsHistory/avgBps/stdDevBps）
    void updateTrafficPattern(const FlowRate& flow);

    /// 计算标准差（用于 Z-Score）
    double calculateStandardDeviation(const std::vector<uint64_t>& values);

    /// 判断数据外泄模式（低频突发 + 大数据量）
    bool isDataExfiltrationPattern(const TrafficPattern& pattern);

    /// 判断异常连接模式（高频短连接）
    bool isSuspiciousConnectionPattern(const TrafficPattern& pattern);

    /// 判断时间异常（静默时段流量）
    bool isTemporalAnomaly(const TrafficPattern& pattern);

    /// 综合计算异常置信度（0.0-1.0）
    double calculateAnomalyConfidence(const TrafficPattern& pattern, const std::string& anomalyType);

    // ---- 统计辅助 ----

    /// Z-Score：(value - mean) / stdDev，衡量偏离程度
    double calculateZScore(uint64_t value, double mean, double stdDev);

    /// 从排序后的序列中取百分位值
    double calculatePercentile(const std::vector<uint64_t>& values, double percentile);

    /// 判断值是否为离群点（值 > mean + 3×stdDev）
    bool isOutlier(uint64_t value, const std::vector<uint64_t>& values);
};

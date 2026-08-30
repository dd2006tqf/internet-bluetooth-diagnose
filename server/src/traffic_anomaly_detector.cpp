/**
 * @file traffic_anomaly_detector.cpp
 * @brief 流量异常检测器实现 - 统计分析 + 启发式规则
 *
 * 检测能力（三类异常）：
 *   1. 数据泄露（data_exfiltration）：持续高流量 + 稳定传输模式
 *      - 判定条件：>50% 时间 bps > 5MB/s，且稳定性 > 0.3，平均 bps > 5MB/s
 *   2. 可疑连接（suspicious_connection）：流量波动剧烈
 *      - 判定条件：变异系数 > 1.0 或存在 3-sigma 异常值
 *   3. 时间异常（temporal_anomaly）：非工作时间的高流量活动
 *      - 判定条件：工作时间外（8:00~18:00 之外），且流量 > 2MB/s
 *
 * 数据来源：
 *   - 由 NetTrafficAnalyzer（eBPF）或外部调用者提供 FlowRate 列表
 *   - 每个 FlowRate 包含：协议/源 IP/目的 IP/bps/pps/pid
 *
 * 线程模型：
 *   - 本类本身不创建线程
 *   - updateTrafficPattern / analyzeTrafficPatterns / clearHistory 持有 patternsMutex_
 *     保护 trafficPatterns_ 的并发访问
 *
 * 统计方法说明：
 *   - 标准差（calculateStandardDeviation）：总体标准差 σ = √(Σ(xᵢ - μ)² / N)
 *   - Z-Score（calculateZScore）：(值 - 均值) / 标准差，用于 3-sigma 异常值检测
 *   - 百分位数（calculatePercentile）：对排序后的列表取位置 (p/100) × (n-1)
 *   - 变异系数：σ / μ，衡量相对波动程度（不受量纲影响）
 */

#include "traffic_anomaly_detector.h"
#include "logger.hpp"
#include <iostream>
#include <numeric>
#include <algorithm>
#include <cmath>
#include <sstream>

using namespace weaknet_dbus;

/**
 * @brief 构造函数：初始化检测参数
 *
 * 默认参数：
 *   - burstThreshold = 2.5 倍（流量突变倍数阈值）
 *   - volumeThreshold = 3.0 倍（流量突增倍数阈值）
 *   - timeThreshold = 2.0 倍（时间模式异常倍数阈值）
 */
TrafficAnomalyDetector::TrafficAnomalyDetector() 
    : burstThreshold_(2.5), volumeThreshold_(3.0), timeThreshold_(2.0) {
}

/**
 * @brief 动态调整异常检测参数
 * @param burstThreshold  突发倍数阈值（流量 > 历史平均 × burstThreshold 判定突发）
 * @param volumeThreshold 体积倍数阈值（流量 > 历史平均 × volumeThreshold 判定可疑）
 * @param timeThreshold   时间倍数阈值
 */
void TrafficAnomalyDetector::setDetectionParams(double burstThreshold, double volumeThreshold, double timeThreshold) {
    std::lock_guard<std::mutex> lock(patternsMutex_);
    burstThreshold_ = burstThreshold;
    volumeThreshold_ = volumeThreshold;
    timeThreshold_ = timeThreshold;
}

/**
 * @brief 更新单个流的历史模式（bps/pps 时间序列）
 *
 * 每次收到 NetTrafficAnalyzer 的采样数据时调用，
 * 将新的 bps/pps 追加到该流的历史窗口中，
 * 并计算/更新统计指标（avgBps/stdDevBps）。
 *
 * 历史窗口上限 MAX_HISTORY=100，超过时丢弃最旧值。
 *
 * @param flow 新的流量采样数据
 */
void TrafficAnomalyDetector::updateTrafficPattern(const FlowRate& flow) {
    // 构造流唯一键：src_port-dst_port/protocol
    std::string flowKey = flow.src + ":" + std::to_string(flow.sport) + "-" + 
                         flow.dst + ":" + std::to_string(flow.dport) + "/" + flow.proto;
    
    auto& pattern = trafficPatterns_[flowKey];
    pattern.flowKey = flowKey;
    
    auto now = std::chrono::system_clock::now();
    
    // 首次记录：标记 firstSeen
    if (pattern.bpsHistory.empty()) {
        pattern.firstSeen = now;
    }
    pattern.lastSeen = now;
    
    // 追加新采样值到历史窗口
    pattern.bpsHistory.push_back(flow.bps);
    pattern.ppsHistory.push_back(flow.pps);
    pattern.totalBytes += flow.bps;
    pattern.totalPackets += flow.pps;
    
    // 限制历史记录大小（滑动窗口最大 100 个采样点）
    const size_t MAX_HISTORY = 100;
    if (pattern.bpsHistory.size() > MAX_HISTORY) {
        pattern.bpsHistory.erase(pattern.bpsHistory.begin());
        pattern.ppsHistory.erase(pattern.ppsHistory.begin());
    }
    
    // 更新统计指标（至少 2 个样本才能计算标准差）
    if (pattern.bpsHistory.size() > 1) {
        // 均值 = Σbps / N
        pattern.avgBps = std::accumulate(pattern.bpsHistory.begin(), pattern.bpsHistory.end(), 0.0) / pattern.bpsHistory.size();
        // 标准差 = √(Σ(bps - avgBps)² / N)
        pattern.stdDevBps = calculateStandardDeviation(pattern.bpsHistory);
    }
}

/**
 * @brief 计算总体标准差
 *
 * 公式：σ = √(Σ(xᵢ - μ)² / N)
 * 样本数 < 2 时返回 0.0（无法计算）
 *
 * @param values 原始数值列表
 * @return 标准差 σ（与输入同单位）
 */
double TrafficAnomalyDetector::calculateStandardDeviation(const std::vector<uint64_t>& values) {
    if (values.size() < 2) return 0.0;
    
    double mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
    double variance = 0.0;
    
    for (uint64_t value : values) {
        variance += std::pow(value - mean, 2);  // 累计偏差平方
    }
    variance /= values.size();  // 总体方差
    
    return std::sqrt(variance);  // 标准差 = √方差
}

/**
 * @brief 计算 Z-Score（标准分数）
 *
 * Z = (值 - 均值) / 标准差，衡量该值偏离均值多少个标准差。
 * stdDev = 0 时返回 0（避免除以零）。
 *
 * 用途：|Z| > 3 表示该值是 3-sigma 异常值（正态分布下概率 < 0.3%）
 *
 * @param value 待测值
 * @param mean  总体均值
 * @param stdDev 总体标准差
 * @return Z-Score
 */
double TrafficAnomalyDetector::calculateZScore(uint64_t value, double mean, double stdDev) {
    if (stdDev == 0.0) return 0.0;
    return (value - mean) / stdDev;
}

/**
 * @brief 计算百分位数（简化线性插值）
 *
 * 索引 = (p/100) × (n-1)，直接取排序后对应位置的值，
 * 不做加权插值，保证确定性。
 *
 * @param values 原始数值列表（内部会排序，不修改原列表）
 * @param percentile 百分位（0~100）
 * @return 对应百分位的值
 */
double TrafficAnomalyDetector::calculatePercentile(const std::vector<uint64_t>& values, double percentile) {
    if (values.empty()) return 0.0;
    
    std::vector<uint64_t> sortedValues = values;
    std::sort(sortedValues.begin(), sortedValues.end());
    
    size_t index = static_cast<size_t>((percentile / 100.0) * (sortedValues.size() - 1));
    return sortedValues[index];
}

/**
 * @brief 检测单个值是否为 3-sigma 异常值
 *
 * 前提：至少 3 个历史样本才能计算有意义的 Z-Score。
 *
 * @param value 待测值
 * @param values 历史样本
 * @return true 该值偏离均值超过 3 个标准差
 */
bool TrafficAnomalyDetector::isOutlier(uint64_t value, const std::vector<uint64_t>& values) {
    if (values.size() < 3) return false;
    
    double mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
    double stdDev = calculateStandardDeviation(values);
    double zScore = calculateZScore(value, mean, stdDev);
    
    // 使用 3-sigma 规则检测异常值（正态分布下 |Z| > 3 概率 < 0.3%）
    return std::abs(zScore) > 3.0;
}

/**
 * @brief 主分析入口：检测三类异常
 *
 * 1. 首先将所有新 flows 更新到 trafficPatterns_（updateTrafficPattern）
 * 2. 对每个已注册的 pattern 依次检测：数据泄露 → 可疑连接 → 时间异常
 * 3. 对命中的异常，计算 confidence（置信度）和 severity（严重度，0~1）
 *
 * @param flows 最近的流量采样列表（来自 NetTrafficAnalyzer 或 eBPF）
 * @return 检测到的异常列表
 */
std::vector<AdvancedAnomaly> TrafficAnomalyDetector::analyzeTrafficPatterns(const std::vector<FlowRate>& flows) {
    LOG_INFO(LogModule::WEAK_MGR, "analyzeTrafficPatterns: analyzing " << flows.size() << " flows");
    std::vector<AdvancedAnomaly> anomalies;
    
    std::lock_guard<std::mutex> lock(patternsMutex_);
    
    // 更新所有流量模式（先更新后检测，保证最新数据参与判断）
    for (const auto& flow : flows) {
        updateTrafficPattern(flow);
    }
    
    // 对每个已注册的流模式依次检测三类异常
    for (auto& [flowKey, pattern] : trafficPatterns_) {
        // ---- 1. 数据泄露检测 ----
        if (isDataExfiltrationPattern(pattern)) {
            LOG_WARNING(LogModule::WEAK_MGR, "analyzeTrafficPatterns: data exfiltration detected for flow " << flowKey);
            AdvancedAnomaly anomaly;
            anomaly.flowKey = flowKey;
            anomaly.anomalyType = "data_exfiltration";
            anomaly.confidence = calculateAnomalyConfidence(pattern, "data_exfiltration");
            // 严重度：相对于 10MB/s 的比例，上限 1.0
            anomaly.severity = std::min(1.0, pattern.avgBps / (10 * 1024 * 1024));
            anomaly.description = "检测到可能的数据泄露行为";
            anomaly.timestamp = std::chrono::system_clock::now();
            anomaly.metrics["avg_bps"] = pattern.avgBps;
            anomaly.metrics["total_bytes"] = pattern.totalBytes;
            anomaly.metrics["duration_minutes"] = std::chrono::duration_cast<std::chrono::minutes>(
                pattern.lastSeen - pattern.firstSeen).count();
            anomalies.push_back(anomaly);
        }
        
        // ---- 2. 可疑连接模式检测 ----
        if (isSuspiciousConnectionPattern(pattern)) {
            LOG_WARNING(LogModule::WEAK_MGR, "analyzeTrafficPatterns: suspicious connection detected for flow " << flowKey);
            AdvancedAnomaly anomaly;
            anomaly.flowKey = flowKey;
            anomaly.anomalyType = "suspicious_connection";
            anomaly.confidence = calculateAnomalyConfidence(pattern, "suspicious_connection");
            // 严重度：变异系数（σ/μ），衡量波动剧烈程度
            anomaly.severity = std::min(1.0, pattern.stdDevBps / pattern.avgBps);
            anomaly.description = "检测到可疑连接模式";
            anomaly.timestamp = std::chrono::system_clock::now();
            anomaly.metrics["std_dev"] = pattern.stdDevBps;
            anomaly.metrics["coefficient_variation"] = pattern.stdDevBps / pattern.avgBps;
            anomalies.push_back(anomaly);
        }
        
        // ---- 3. 时间模式异常检测 ----
        if (isTemporalAnomaly(pattern)) {
            LOG_WARNING(LogModule::WEAK_MGR, "analyzeTrafficPatterns: temporal anomaly detected for flow " << flowKey);
            AdvancedAnomaly anomaly;
            anomaly.flowKey = flowKey;
            anomaly.anomalyType = "temporal_anomaly";
            anomaly.confidence = calculateAnomalyConfidence(pattern, "temporal_anomaly");
            anomaly.severity = 0.7; // 时间异常通常中等严重程度
            anomaly.description = "检测到时间模式异常";
            anomaly.timestamp = std::chrono::system_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::minutes>(pattern.lastSeen - pattern.firstSeen);
            anomaly.metrics["duration_minutes"] = duration.count();
            anomalies.push_back(anomaly);
        }
    }
    
    return anomalies;
}

/**
 * @brief 判断是否符合数据泄露模式
 *
 * 判定逻辑：持续高流量 + 流量稳定（不像正常的突发）
 *   1. 历史样本数 ≥ 5
 *   2. 超过 50% 的时间 bps > 5MB/s
 *   3. 稳定性 = 1 - (σ/μ) > 0.3（流量相对平稳）
 *   4. 平均 bps > 5MB/s
 *
 * @param pattern 已积累的流量模式
 * @return true 符合数据泄露特征
 */
bool TrafficAnomalyDetector::isDataExfiltrationPattern(const TrafficPattern& pattern) {
    if (pattern.bpsHistory.size() < 5) return false;
    
    // 检查持续高流量
    uint64_t highBpsCount = 0;
    uint64_t threshold = 5 * 1024 * 1024; // 5MB/s
    
    for (uint64_t bps : pattern.bpsHistory) {
        if (bps > threshold) {
            highBpsCount++;
        }
    }
    
    // 超过 50% 的时间都是高流量，可能是数据泄露
    double highBpsRatio = static_cast<double>(highBpsCount) / pattern.bpsHistory.size();
    
    // 检查流量稳定性（数据泄露通常比较稳定，波动不大）
    // 稳定性 = 1 - (变异系数)，值越大越稳定
    double stability = 1.0 - (pattern.stdDevBps / pattern.avgBps);
    
    return highBpsRatio > 0.5 && stability > 0.3 && pattern.avgBps > threshold;
}

/**
 * @brief 判断是否为可疑连接模式
 *
 * 判定逻辑：流量波动异常剧烈
 *   1. 历史样本数 ≥ 3
 *   2. 变异系数 > 1.0（σ > μ，波动幅度超过均值）
 *      或存在 3-sigma 异常值（尖峰流量）
 *
 * @param pattern 已积累的流量模式
 * @return true 符合可疑连接特征
 */
bool TrafficAnomalyDetector::isSuspiciousConnectionPattern(const TrafficPattern& pattern) {
    if (pattern.bpsHistory.size() < 3) return false;
    
    // 变异系数 = σ / μ，衡量相对波动
    double coefficientOfVariation = pattern.stdDevBps / pattern.avgBps;
    
    // 检查是否有异常峰值（3-sigma outlier）
    bool hasOutliers = false;
    for (uint64_t bps : pattern.bpsHistory) {
        if (isOutlier(bps, pattern.bpsHistory)) {
            hasOutliers = true;
            break;
        }
    }
    
    return coefficientOfVariation > 1.0 || hasOutliers;
}

/**
 * @brief 判断是否为时间模式异常
 *
 * 判定逻辑：在非工作时间有大量流量
 *   1. 当前时间不在 8:00~18:00 工作时间窗口内
 *   2. 平均流量 > 2MB/s
 *
 * @param pattern 已积累的流量模式
 * @return true 符合时间异常特征
 */
bool TrafficAnomalyDetector::isTemporalAnomaly(const TrafficPattern& pattern) {
    auto now = std::chrono::system_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::minutes>(now - pattern.lastSeen);
    
    // 获取当前小时数
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto tm = *std::localtime(&time_t);
    int hour = tm.tm_hour;
    
    bool isOffHours = (hour < 8 || hour > 18);  // 工作时间外
    bool hasHighVolume = pattern.avgBps > (2 * 1024 * 1024); // 2MB/s
    
    return isOffHours && hasHighVolume;
}

/**
 * @brief 计算异常置信度（0~1）
 *
 * 不同异常类型采用不同的打分模型：
 *   - data_exfiltration：volumeScore（相对 20MB/s）和 stabilityScore 的平均
 *   - suspicious_connection：变异系数，值越大越可疑
 *   - temporal_anomaly：基于当前时间的偏离程度（深夜 > 早晚 > 工作时间）
 *
 * @param pattern 流量模式
 * @param anomalyType 异常类型字符串
 * @return 置信度（0.0~1.0）
 */
double TrafficAnomalyDetector::calculateAnomalyConfidence(const TrafficPattern& pattern, const std::string& anomalyType) {
    double confidence = 0.0;
    
    if (anomalyType == "data_exfiltration") {
        // 基于流量大小和稳定性计算置信度
        double volumeScore = std::min(1.0, pattern.avgBps / (20 * 1024 * 1024)); // 20MB/s 为满分
        double stabilityScore = 1.0 - std::min(1.0, pattern.stdDevBps / pattern.avgBps);
        confidence = (volumeScore + stabilityScore) / 2.0;
    }
    else if (anomalyType == "suspicious_connection") {
        // 基于流量波动性计算置信度
        double variationScore = std::min(1.0, pattern.stdDevBps / pattern.avgBps);
        confidence = variationScore;
    }
    else if (anomalyType == "temporal_anomaly") {
        // 基于时间模式计算置信度
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto tm = *std::localtime(&time_t);
        int hour = tm.tm_hour;
        
        // 深夜（<6 或 >22）置信度 1.0，早晚（<8 或 >20）0.7，工作时间 0.3
        double timeScore = 0.0;
        if (hour < 6 || hour > 22) timeScore = 1.0;   // 深夜
        else if (hour < 8 || hour > 20) timeScore = 0.7; // 早晚
        else timeScore = 0.3;                            // 工作时间
        
        confidence = timeScore;
    }
    
    return std::min(1.0, std::max(0.0, confidence));  // 夹取到 [0, 1]
}

// ---- 以下三个方法为直接检测接口，不依赖 analyzeTrafficPatterns 的完整模式更新 ----

/**
 * @brief 直接检测数据泄露（简化版）
 *
 * 单 pass 扫描所有 flow，bps > 10MB/s 即标记为潜在数据泄露。
 * 比 analyzeTrafficPatterns 更灵敏但误报率更高。
 *
 * @param flows 流量采样列表
 * @return 异常列表
 */
std::vector<AdvancedAnomaly> TrafficAnomalyDetector::detectDataExfiltration(const std::vector<FlowRate>& flows) {
    std::vector<AdvancedAnomaly> anomalies;
    
    for (const auto& flow : flows) {
            // 检测大文件传输（单连接 bps > 10MB/s）
        if (flow.bps > 10 * 1024 * 1024) { // 10MB/s
            LOG_WARNING(LogModule::WEAK_MGR, "detectDataExfiltration: high流量 " << flow.bps << " bytes/sec from " << flow.src << " to " << flow.dst);
            AdvancedAnomaly anomaly;
            anomaly.flowKey = flow.src + ":" + std::to_string(flow.sport) + "-" + 
                             flow.dst + ":" + std::to_string(flow.dport) + "/" + flow.proto;
            anomaly.anomalyType = "data_exfiltration";
            anomaly.confidence = std::min(1.0, flow.bps / (50.0 * 1024 * 1024));
            anomaly.severity = std::min(1.0, flow.bps / (100.0 * 1024 * 1024));
            anomaly.description = "检测到高流量数据传输，可能存在数据泄露";
            anomaly.timestamp = std::chrono::system_clock::now();
            anomaly.metrics["current_bps"] = flow.bps;
            anomaly.metrics["pid"] = flow.pid;
            anomalies.push_back(anomaly);
        }
    }
    
    return anomalies;
}

/**
 * @brief 直接检测可疑连接（进程级连接数异常）
 *
 * 统计每个 pid 的连接数，超过 50 个标记为可疑（可能是 C&C 回连或僵尸网络）。
 *
 * @param flows 流量采样列表
 * @return 异常列表
 */
std::vector<AdvancedAnomaly> TrafficAnomalyDetector::detectSuspiciousConnections(const std::vector<FlowRate>& flows) {
    std::vector<AdvancedAnomaly> anomalies;
    
    // 统计每个进程的连接数
    std::map<uint32_t, int> pidConnectionCount;
    for (const auto& flow : flows) {
        if (flow.pid > 0) {
            pidConnectionCount[flow.pid]++;
        }
    }
    
    // 检测异常多连接的进程
    for (const auto& [pid, count] : pidConnectionCount) {
        if (count > 50) { // 超过 50 个连接（正常应用一般 < 20）
            LOG_WARNING(LogModule::WEAK_MGR, "detectSuspiciousConnections: PID " << pid << " has " << count << " connections");
            AdvancedAnomaly anomaly;
            anomaly.flowKey = "PID:" + std::to_string(pid);
            anomaly.anomalyType = "suspicious_connection";
            anomaly.confidence = std::min(1.0, count / 200.0);
            anomaly.severity = std::min(1.0, count / 100.0);
            anomaly.description = "进程 " + std::to_string(pid) + " 有异常多的连接数: " + std::to_string(count);
            anomaly.timestamp = std::chrono::system_clock::now();
            anomaly.metrics["connection_count"] = count;
            anomaly.metrics["pid"] = pid;
            anomalies.push_back(anomaly);
        }
    }
    
    return anomalies;
}

/**
 * @brief 直接检测时间异常（简化版）
 *
 * 当前时间为非工作时间时，检查总流量是否超过 5MB/s。
 *
 * @param flows 流量采样列表
 * @return 异常列表
 */
std::vector<AdvancedAnomaly> TrafficAnomalyDetector::detectTemporalAnomalies(const std::vector<FlowRate>& flows) {
    std::vector<AdvancedAnomaly> anomalies;
    
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto tm = *std::localtime(&time_t);
    
    // 检查是否在非工作时间（8:00~18:00 之外）
    bool isOffHours = (tm.tm_hour < 8 || tm.tm_hour > 18);
    
    if (isOffHours) {
        // 累计当前所有流的 bps
        uint64_t totalBps = 0;
        for (const auto& flow : flows) {
            totalBps += flow.bps;
        }

        if (totalBps > 5 * 1024 * 1024) { // 5MB/s
            LOG_WARNING(LogModule::WEAK_MGR, "detectTemporalAnomalies: 非工作时间高流量活动，总流量 " << totalBps << " bytes/sec，小时 " << (int)tm.tm_hour);
            AdvancedAnomaly anomaly;
            anomaly.flowKey = "temporal_anomaly";
            anomaly.anomalyType = "temporal_anomaly";
            anomaly.confidence = 0.8;
            anomaly.severity = std::min(1.0, totalBps / (20.0 * 1024 * 1024));
            anomaly.description = "在非工作时间检测到高流量活动";
            anomaly.timestamp = now;
            anomaly.metrics["total_bps"] = totalBps;
            anomaly.metrics["hour"] = tm.tm_hour;
            anomalies.push_back(anomaly);
        }
    }
    
    return anomalies;
}

/**
 * @brief 获取所有已记录的流量模式快照
 * @return 以流唯一键为键的 TrafficPattern Map
 */
std::map<std::string, TrafficPattern> TrafficAnomalyDetector::getTrafficPatterns() {
    std::lock_guard<std::mutex> lock(patternsMutex_);
    return trafficPatterns_;
}

/**
 * @brief 清除所有历史流量模式（在分析周期切换时调用）
 */
void TrafficAnomalyDetector::clearHistory() {
    std::lock_guard<std::mutex> lock(patternsMutex_);
    trafficPatterns_.clear();
}

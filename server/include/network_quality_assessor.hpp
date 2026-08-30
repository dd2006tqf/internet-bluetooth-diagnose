/**
 * @file network_quality_assessor.hpp
 * @brief 网络质量评估器：基于多指标综合评分
 *
 * 评估维度：
 *   - RTT 延迟（ICMP 探测）
 *   - TCP 丢包率（/proc/net/snmp 差分）
 *   - Wi-Fi RSSI（wpa_supplicant SIGNAL_POLL）
 *   - 流量异常（可选，由 NetTrafficAnalyzer 提供）
 *
 * 评分公式（每维度 0-100 分，按权重加权平均）：
 *   score = w_rtt * RTT_score + w_loss * Loss_score + w_rssi * RSSI_score + w_traffic * Traffic_score
 *   各权重默认：w_rtt=0.3, w_loss=0.3, w_rssi=0.3, w_traffic=0.1
 *
 * 阈值配置：
 *   所有阈值可通过环境变量覆盖，便于现场调优（无需重新编译）。
 *   环境变量格式：WEAKNET_<METRIC>_<LEVEL>，如 WEAKNET_RTT_EXCELLENT=50。
 *
 * 线程安全：assessQuality() / assessInterfaceQuality() 内部无状态修改，
 *           可安全在多线程中调用；updateThresholds() 需外部加锁。
 */

#ifndef NETWORK_QUALITY_ASSESSOR_HPP
#define NETWORK_QUALITY_ASSESSOR_HPP

#include <string>
#include <map>
#include <vector>
#include <cstdlib>
#include "net_info.hpp"

namespace weaknet_dbus {

/// 网络质量等级（可与 LinkQuality 枚举对比，但粒度不同）
enum class NetworkQualityLevel {
    EXCELLENT = 4,  ///< 优秀（总分 >= 80）
    GOOD = 3,       ///< 良好（60-79）
    FAIR = 2,       ///< 一般（40-59）
    POOR = 1,       ///< 差（< 40）
    UNKNOWN = 0     ///< 未知（数据不足）
};

/// 网络质量评估结果（供 D-Bus GetNetworkQuality 方法返回）
struct NetworkQualityResult {
    NetworkQualityLevel level;       ///< 质量等级枚举
    std::string levelName;           ///< 等级名称字符串（"excellent" / "good" / ...）
    std::string details;             ///< JSON 格式的详细指标（用于 UI 展示）
    double score;                    ///< 0-100 的综合质量分数
    std::vector<std::string> issues; ///< 发现的问题列表（如 "RTT 过高"、"丢包率异常"）
};

/**
 * @brief 网络质量评估器
 *
 * 使用：
 *   NetworkQualityAssessor assessor;
 *   auto result = assessor.assessQuality(interfaces);
 *   // 或指定单个接口
 *   auto result = assessor.assessInterfaceQuality(oneInterface);
 */
class NetworkQualityAssessor {
private:
    /**
     * @brief 质量评估阈值配置
     *
     * 所有阈值可通过环境变量覆盖（见各字段注释）。
     * 使用 lambda 初始化列表在构造函数执行时读取环境变量，
     * 避免运行时反复调用 std::getenv。
     */
    struct QualityThresholds {
        // ---- RTT 阈值（毫秒） ----
        // WEAKNET_RTT_EXCELLENT / WEAKNET_RTT_GOOD / WEAKNET_RTT_FAIR
        int rtt_excellent = []() {
            const char* env = std::getenv("WEAKNET_RTT_EXCELLENT");
            return env ? std::atoi(env) : 50;
        }();
        int rtt_good = []() {
            const char* env = std::getenv("WEAKNET_RTT_GOOD");
            return env ? std::atoi(env) : 100;
        }();
        int rtt_fair = []() {
            const char* env = std::getenv("WEAKNET_RTT_FAIR");
            return env ? std::atoi(env) : 200;
        }();

        // ---- TCP 丢包率阈值（百分比） ----
        // WEAKNET_TCP_LOSS_EXCELLENT / WEAKNET_TCP_LOSS_GOOD / WEAKNET_TCP_LOSS_FAIR
        double tcp_loss_excellent = []() {
            const char* env = std::getenv("WEAKNET_TCP_LOSS_EXCELLENT");
            return env ? std::atof(env) : 0.1;
        }();
        double tcp_loss_good = []() {
            const char* env = std::getenv("WEAKNET_TCP_LOSS_GOOD");
            return env ? std::atof(env) : 0.5;
        }();
        double tcp_loss_fair = []() {
            const char* env = std::getenv("WEAKNET_TCP_LOSS_FAIR");
            return env ? std::atof(env) : 2.0;
        }();

        // ---- RSSI 阈值（dBm，越大越好）----
        // WEAKNET_RSSI_EXCELLENT / WEAKNET_RSSI_GOOD / WEAKNET_RSSI_FAIR
        int rssi_excellent = []() {
            const char* env = std::getenv("WEAKNET_RSSI_EXCELLENT");
            return env ? std::atoi(env) : -50;
        }();
        int rssi_good = []() {
            const char* env = std::getenv("WEAKNET_RSSI_GOOD");
            return env ? std::atoi(env) : -60;
        }();
        int rssi_fair = []() {
            const char* env = std::getenv("WEAKNET_RSSI_FAIR");
            return env ? std::atoi(env) : -70;
        }();

        // ---- 流量异常检测阈值 ----
        double traffic_anomaly_threshold = 0.8;  ///< 异常流量比例阈值
        int min_flows_for_analysis = 5;           ///< 最小流数量用于分析
    };

    QualityThresholds thresholds_;   ///< 当前生效的阈值配置
    NetworkQualityResult lastResult_;  ///< 上次评估结果（供 D-Bus 缓存查询）
    int32_t qualityChangeCounter_;    ///< 质量等级变化计数器（每次等级变化 +1）

public:
    NetworkQualityAssessor();
    
    /**
     * @brief 评估多个接口的综合网络质量
     *
     * 对所有接口分别评分，取最高分作为最终结果（活跃接口优先）。
     *
     * @param interfaces 所有网络接口的 NetInfo 列表
     * @return 综合评估结果
     */
    NetworkQualityResult assessQuality(const std::vector<NetInfo>& interfaces);
    
    /**
     * @brief 评估单个接口的质量
     * @param interface  单个接口的 NetInfo
     * @return 该接口的质量评估结果
     */
    NetworkQualityResult assessInterfaceQuality(const NetInfo& interface);
    
    /// 获取质量等级的英文名称字符串（供 D-Bus 上报）
    static std::string getQualityLevelName(NetworkQualityLevel level);
    
    /**
     * @brief 生成详细质量信息（JSON 格式）
     *
     * 包含各维度原始指标、单项分数、问题列表。
     * 用于 D-Bus Details 属性和日志调试。
     */
    std::string generateQualityDetails(const NetInfo& interface, double score, const std::vector<std::string>& issues);
    
    /// 比较当前结果与上次是否有等级变化
    bool hasQualityChanged(const NetworkQualityResult& current);
    
    /// 自上次 reset 以来质量等级变化次数
    int32_t getQualityChangeCounter() const { return qualityChangeCounter_; }
    
    /**
     * @brief 更新阈值配置
     *
     * 用于运行时动态调整（如根据设备类型切换阈值）。
     * @note 非线程安全；若多线程使用需外部加锁。
     */
    void updateThresholds(const QualityThresholds& newThresholds);
    
private:
    /// 计算 RTT 质量分数（0-100，越低越好）
    double calculateRttScore(int rttMs);
    
    /// 计算 TCP 丢包质量分数（0-100，越低越好）
    double calculateTcpLossScore(double lossRate);
    
    /// 计算 RSSI 质量分数（0-100，越大越好）
    double calculateRssiScore(int rssiDbm);
    
    /// 计算流量质量分数（异常流量占比越高分数越低）
    double calculateTrafficScore(const NetInfo& interface);
    
    /// 综合所有维度，生成诊断问题列表
    std::vector<std::string> detectNetworkIssues(const NetInfo& interface, double score);
    
    /// 生成 JSON 格式的详细指标字符串（供 generateQualityDetails 内部调用）
    std::string generateMetricsJson(const NetInfo& interface, double score, const std::vector<std::string>& issues);
};

} // namespace weaknet_dbus

#endif // NETWORK_QUALITY_ASSESSOR_HPP

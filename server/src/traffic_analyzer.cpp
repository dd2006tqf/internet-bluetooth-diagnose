/**
 * @file traffic_analyzer.cpp
 * @brief 流量周期分析线程实现
 *
 * 监控指标：
 *   - 总带宽（MB/s）：累计所有活动流的 bitrate
 *   - 活动流数量（activeFlows）
 *   - 总包速率（PPS）
 *   - 顶 N 流量连接（Top 5 flows）：协议/源/目的 IP 端口/带宽/包速
 *   - 异常流量检测：突发流量 / 可疑连接模式（委托 TrafficAnomalyDetector）
 *
 * 数据源：
 *   - eBPF：通过 NetTrafficAnalyzer（封装 flow_rate.bpf.o）从 BPF Map 读取流量统计
 *     - BPF 对象文件名：flow_rate.bpf.o（与 ProcessNetProfiler 共享）
 *     - 探针类型：kprobe（与 ProcessNetProfiler 相同的 3 个 kprobe）
 *   - 降级模式：eBPF 初始化失败时，degraded_mode_=true，analyzeLoop 跳过所有 eBPF 调用
 *
 * 线程模型：
 *   - 独立 std::thread（TrafficAnalyzer::analyzeLoop），可通过 start()/stop() 控制生命周期
 *   - analyzeLoop 内部周期性（interval_seconds，默认 10s）调用：
 *     1. analyzer_->getRealTimeStats() 获取实时统计
 *     2. analyzer_->detectAnomalies() 检测异常
 *     3. analyzer_->sampleTopFlows() 获取顶 N 流量连接
 *   - 线程安全：cached_stats_ 通过 stats_mutex_ 保护，对外暴露 getCurrentStats()
 */

#include "traffic_analyzer.hpp"
#include "logger.hpp"
#include <chrono>
#include <thread>

namespace weaknet_dbus {

TrafficAnalyzer::TrafficAnalyzer()
    : running_(false), degraded_mode_(false), interval_seconds_(10) {
    analyzer_ = NetTrafficAnalyzer::getInstance();
}

TrafficAnalyzer::~TrafficAnalyzer() {
    stop();
}

/**
 * @brief 启动流量分析线程
 *
 * 执行顺序：
 *   1. 设置 eBPF 对象路径和异常检测参数（突发阈值 5MB/s，可疑阈值 20MB/s，2.5 倍突发倍数）
 *   2. 初始化 NetTrafficAnalyzer（可能失败，失败后进入降级模式）
 *   3. 创建 analyzeLoop 后台线程
 *
 * @param interface      网络接口名（如 "wlan0"），空字符串表示自动选择
 * @param interval_seconds 分析周期（秒），默认 10s
 */
void TrafficAnalyzer::start(const std::string& interface, int interval_seconds) {
    if (running_.load()) {
        LOG_INFO(LogModule::WEAK_MGR, "Traffic analyzer already running");
        return;
    }
    
    interface_ = interface;
    interval_seconds_ = interval_seconds;
    
    // 设置 eBPF 对象路径
    analyzer_->setBpfObjectPath("build/flow_rate.bpf.o");
    
    // 设置异常检测参数
    analyzer_->setAnomalyDetectionParams(
        5 * 1024 * 1024,    // 突发阈值: 5MB/s
        20 * 1024 * 1024,   // 可疑阈值: 20MB/s
        2.5                 // 突发倍数: 2.5倍（超过历史平均 2.5 倍算突发）
    );
    
    // 初始化网络接口
    // degraded_mode_: initForInterface 失败时置 true，analyzeLoop 检查此标志跳过 eBPF 调用
    if (!analyzer_->initForInterface(interface)) {
        LOG_ERROR(LogModule::WEAK_MGR, "Failed to initialize traffic analyzer for interface: " << interface);
        LOG_INFO(LogModule::WEAK_MGR, "Traffic analyzer will run in degraded mode (no eBPF monitoring)");
        degraded_mode_.store(true);
        // 不返回，继续运行但跳过 eBPF 功能
    } else {
        degraded_mode_.store(false);
    }
    
    running_.store(true);
    thread_ = std::make_unique<std::thread>(&TrafficAnalyzer::analyzeLoop, this);
    
    LOG_INFO(LogModule::WEAK_MGR, "Traffic analyzer started for interface: " << interface << " (interval=" << interval_seconds << "s)");
}

/**
 * @brief 停止流量分析线程
 *
 * 设置 running_=false，等待 analyzeLoop 退出（最多 interval_seconds 秒），
 * 然后清理 NetTrafficAnalyzer 的历史数据。
 */
void TrafficAnalyzer::stop() {
    if (!running_.load()) {
        return;
    }
    
    running_.store(false);
    
    if (thread_ && thread_->joinable()) {
        thread_->join();
    }
    
    // 清理历史数据
    analyzer_->clearHistory();
    
    LOG_INFO(LogModule::WEAK_MGR, "Traffic analyzer stopped");
}

/**
 * @brief 流量分析主循环（后台线程入口）
 *
 * 每个周期执行：
 *   1. 降级模式检测：degraded_mode_=true 时跳过所有 eBPF 调用，仅打印降级日志
 *   2. 获取实时统计（总带宽/活动流/PPS）并缓存到 cached_stats_
 *   3. 调用 detectAnomalies(5) 检测近 5 秒的异常流量
 *   4. 调用 sampleTopFlows(5, 5) 采样顶 5 流量连接，记录日志
 *
 * 异常处理：每个 try-catch 块独立，单个步骤失败不影响后续步骤。
 * 睡眠策略：以 1 秒为单位分段睡眠，保证 stop() 能在 1s 内响应。
 */
void TrafficAnalyzer::analyzeLoop() {
    LOG_INFO(LogModule::WEAK_MGR, "Traffic analysis loop started");
    
    while (running_.load()) {
        try {
            // 降级模式下跳过 eBPF 调用
            if (degraded_mode_.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(interval_seconds_));
                continue;
            }

            // 获取实时统计（如果eBPF可用）
            NetTrafficAnalyzer::RealTimeStats stats;
            bool hasStats = false;

            try {
                stats = analyzer_->getRealTimeStats();
                hasStats = true;

                // 更新缓存（供外部线程调用 getCurrentStats() 读取）
                {
                    std::lock_guard<std::mutex> lock(stats_mutex_);
                    cached_stats_ = stats;
                }
            } catch (const std::exception& e) {
                LOG_INFO(LogModule::WEAK_MGR, "Traffic stats unavailable (eBPF not working): " << e.what());
            }
            
            // 检测异常流量（如果eBPF可用）
            if (hasStats) {
                try {
                    auto anomalies = analyzer_->detectAnomalies(5);  // 检测近 5 秒的异常
                    if (!anomalies.empty()) {
                        LOG_INFO(LogModule::WEAK_MGR, "Detected " << anomalies.size() << " traffic anomalies");
                        for (const auto& anomaly : anomalies) {
                            LOG_INFO(LogModule::WEAK_MGR, "Anomaly: " << anomaly.anomalyType 
                                << " on " << anomaly.flowKey 
                                << " (severity: " << (anomaly.severity * 100) << "%)");
                        }
                    }
                } catch (const std::exception& e) {
                    LOG_INFO(LogModule::WEAK_MGR, "Anomaly detection unavailable: " << e.what());
                }
            }
            
            // 记录详细流量统计
            if (hasStats) {
                LOG_INFO(LogModule::WEAK_MGR, "TRAFFIC_MONITOR: Total=" << (stats.totalBps / (1024*1024)) 
                    << "MB/s, Flows=" << stats.activeFlows 
                    << ", PPS=" << stats.totalPps
                    << ", Interface=" << interface_);
                    
                // 获取Top流量连接并记录（采样 5 秒，取前 5 个）
                try {
                    auto topFlows = analyzer_->sampleTopFlows(5, 5);
                    if (!topFlows.empty()) {
                        LOG_INFO(LogModule::WEAK_MGR, "TOP_FLOWS: ");
                        for (size_t i = 0; i < std::min(topFlows.size(), size_t(3)); ++i) {
                            const auto& flow = topFlows[i];
                            LOG_INFO(LogModule::WEAK_MGR, "  " << (i+1) << ". " << flow.proto 
                                << " " << flow.src << ":" << flow.sport 
                                << " -> " << flow.dst << ":" << flow.dport 
                                << " | " << (flow.bps / 1024) << "KB/s, " << flow.pps << "pps");
                        }
                    }
                } catch (const std::exception& e) {
                    LOG_INFO(LogModule::WEAK_MGR, "Top flows unavailable: " << e.what());
                }
            } else {
                LOG_INFO(LogModule::WEAK_MGR, "TRAFFIC_MONITOR: Running in degraded mode (no eBPF data available)");
            }
            
        } catch (const std::exception& e) {
            LOG_ERROR(LogModule::WEAK_MGR, "Traffic analysis error: " << e.what());
        }
        
        // 等待下一个分析周期（以 1s 分段睡眠，保证 stop() 能快速响应）
        for (int i = 0; i < interval_seconds_ && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    
    LOG_INFO(LogModule::WEAK_MGR, "Traffic analysis loop stopped");
}

/**
 * @brief 获取最近缓存的实时流量统计（线程安全）
 * @return RealTimeStats 缓存副本
 */
NetTrafficAnalyzer::RealTimeStats TrafficAnalyzer::getCurrentStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return cached_stats_;
}

/**
 * @brief 采样顶 N 流量连接（委托 NetTrafficAnalyzer）
 * @param sample_seconds 采样时间窗口（秒）
 * @param top_count 返回的最大连接数
 * @return 按带宽降序的流量连接列表
 */
std::vector<FlowRate> TrafficAnalyzer::getTopFlows(int sample_seconds, int top_count) const {
    return analyzer_->sampleTopFlows(sample_seconds, top_count);
}

/**
 * @brief 检测异常流量（委托 NetTrafficAnalyzer）
 * @param detection_seconds 异常检测时间窗口（秒）
 * @return 异常列表
 */
std::vector<TrafficAnomaly> TrafficAnalyzer::detectAnomalies(int detection_seconds) const {
    return analyzer_->detectAnomalies(detection_seconds);
}

/**
 * @brief 获取流量历史记录
 * @return 以接口名为键的 TrafficHistory Map
 */
std::map<std::string, TrafficHistory> TrafficAnalyzer::getTrafficHistory() const {
    return analyzer_->getTrafficHistory();
}

} // namespace weaknet_dbus

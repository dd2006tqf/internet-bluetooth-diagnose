/**
 * @file traffic_analyzer.hpp
 * @brief 流量分析线程管理器
 *
 * 封装 NetTrafficAnalyzer 的后台线程，提供 start/stop 生命周期管理和缓存状态查询。
 * 典型用法：server.cpp 在启动时调用 start()，DbusService 通过 getter 查询实时状态。
 *
 * 线程模型：
 *   - 内部启动一个 std::thread，循环调用 NetTrafficAnalyzer::sampleTopFlows()
 *   - stats_mutex_ 保护 cached_stats_，工作线程写、查询线程读
 *   - running_（std::atomic）控制循环退出
 *   - degraded_mode_（std::atomic）标记 eBPF 是否可用
 *
 * 降级策略：eBPF 初始化失败时，analyzeLoop() 内部检测并设置 degraded_mode_=true，
 *           后续调用返回空数据，不崩溃。
 */

#pragma once

#include <thread>
#include <atomic>
#include <string>
#include <memory>
#include "net_traffic.h"

namespace weaknet_dbus {

/**
 * @brief 流量分析线程管理器
 *
 * 持有 NetTrafficAnalyzer 实例（共享所有权），
 * 在独立线程中定期采样并缓存结果，供高频查询使用。
 */
class TrafficAnalyzer {
public:
    TrafficAnalyzer();
    ~TrafficAnalyzer();

    /**
     * @brief 启动流量分析线程
     * @param interface       绑定的网卡名（传递给 NetTrafficAnalyzer::initForInterface）
     * @param interval_seconds 采样间隔（秒），默认 10 秒
     */
    void start(const std::string& interface, int interval_seconds = 10);
    
    /// 停止流量分析线程（join 等待退出）
    void stop();
    
    /// 线程是否在运行（start 后 stop 前）
    bool isRunning() const { return running_.load(); }

    /// 是否处于降级模式（eBPF 初始化失败，但线程仍在跑，返回空数据）
    bool isDegradedMode() const { return degraded_mode_.load(); }
    
    /**
     * @brief 获取缓存的实时流量统计
     * @return 最近一次采样的统计快照（stats_mutex_ 保护拷贝）
     */
    NetTrafficAnalyzer::RealTimeStats getCurrentStats() const;
    
    /**
     * @brief 获取 Top 流量连接（按需触发一次采样）
     * @param sample_seconds  采样窗口（秒）
     * @param top_count       返回前 N 个
     */
    std::vector<FlowRate> getTopFlows(int sample_seconds = 5, int top_count = 10) const;
    
    /**
     * @brief 触发一次流量异常检测
     * @param detection_seconds  采样窗口（秒）
     */
    std::vector<TrafficAnomaly> detectAnomalies(int detection_seconds = 5) const;
    
    /// 获取流量历史记录（内部转发给 NetTrafficAnalyzer）
    std::map<std::string, TrafficHistory> getTrafficHistory() const;

private:
    /// 线程主循环：采样 → 缓存 → 等 interval
    void analyzeLoop();
    
    std::unique_ptr<std::thread> thread_;   ///< 后台采样线程
    std::atomic<bool> running_;              ///< 线程运行标志
    std::atomic<bool> degraded_mode_;       ///< 是否降级模式
    std::string interface_;                 ///< 绑定网卡名
    int interval_seconds_;                  ///< 采样间隔

    std::shared_ptr<NetTrafficAnalyzer> analyzer_;  ///< 底层 eBPF 分析器

    mutable std::mutex stats_mutex_;                             ///< 保护 cached_stats_
    NetTrafficAnalyzer::RealTimeStats cached_stats_;             ///< 最近一次采样结果缓存
};

} // namespace weaknet_dbus

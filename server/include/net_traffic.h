/**
 * @file net_traffic.h
 * @brief 流量分析器（eBPF 驱动，单例）
 *
 * 通过加载 flow_rate.bpf.o 并挂接到 TCP/UDP 发送/接收路径，
 * 实现内核态的流速率统计和异常检测。
 *
 * 架构：
 *   eBPF 程序在内核态按 (src, dst, sport, dport, proto) 五元组
 *   累加 bytes 和 packets，用户态定时从 map 读取做差分计算速率。
 *
 * 线程安全：getInstance() 使用 std::once_flag。
 *           mapMutex_ 保护 map fd 访问；historyMutex_ 保护历史数据。
 */

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <cstdint>
#include <chrono>
#include <deque>
#include <map>

/// 单条流的速率快照（从内核态 map 读取后差分计算）
struct FlowRate {
    std::string src;       ///< 源 IP
    std::string dst;       ///< 目标 IP
    int sport = 0;         ///< 源端口
    int dport = 0;         ///< 目标端口
    std::string proto;     ///< "TCP" 或 "UDP"
    uint64_t bps = 0;      ///< 字节速率（bytes per second，采样窗口内）
    uint64_t pps = 0;      ///< 包速率（packets per second）
    uint32_t pid = 0;      ///< 关联进程 ID（eBPF 捕获）
};

/// 流量异常检测结果（突发流量 / 高流量 / 可疑流量）
struct TrafficAnomaly {
    std::string flowKey;        ///< 流唯一标识（src:sport → dst:dport proto）
    std::string anomalyType;    ///< 异常类型: "burst" / "suspicious" / "high_volume"
    uint64_t currentBps;       ///< 当前速率（字节/秒）
    uint64_t thresholdBps;     ///< 触发阈值
    double severity;            ///< 严重程度 0.0-1.0
    std::chrono::system_clock::time_point timestamp;
    std::string description;    ///< 人类可读描述
};

/// 流量历史记录（用于突发/异常检测的基线对比）
struct TrafficHistory {
    std::deque<uint64_t> bpsHistory;    ///< 历史速率记录（最近 N 个采样）
    std::deque<uint64_t> ppsHistory;    ///< 历史包速率记录
    std::chrono::system_clock::time_point lastUpdate;
    uint64_t totalBytes = 0;
    uint64_t totalPackets = 0;
};

/**
 * @brief eBPF 驱动的流量分析器
 *
 * 单例模式，通过 getInstance() 获取。
 * 必须先调用 initForInterface() 绑定网卡，否则所有方法返回空/零值。
 *
 * 降级策略：eBPF 加载失败时 initForInterface() 返回 false，
 *           后续所有方法返回空数据（不崩溃）。
 */
class NetTrafficAnalyzer {
public:
    /// 线程安全懒汉单例
    static std::shared_ptr<NetTrafficAnalyzer> getInstance();

    /**
     * @brief 设置 eBPF 对象文件路径
     * @param path  通常为 "build/flow_rate.bpf.o"
     */
    void setBpfObjectPath(const std::string& path);

    /**
     * @brief 初始化并附加到内核（按接口过滤）
     *
     * 创建 XDP hook 或 tc hook，过滤指定网卡的流量。
     * @param ifaceName  绑定的网卡名（如 "wlan0"）
     * @return true  eBPF 成功挂载
     * @return false 加载失败（降级为离线模式）
     */
    bool initForInterface(const std::string& ifaceName);

    /// 获取 process_stats map 的 fd（供 ProcessNetProfiler 共享）
    int getProcessStatsFd() const { return mapProcessStatsFd_; }

    /**
     * @brief 采样 TopN 流的速率
     * @param intervalSec  采样窗口（秒）；内部会 sleep 这段时间取两次差分
     * @param topN         返回前 N 个按 bps 排序的流
     */
    std::vector<FlowRate> sampleTopFlows(int intervalSec, int topN);

    /**
     * @brief 异常流量检测
     * @param intervalSec          采样窗口（秒）
     * @param burstThresholdBps    突发阈值（默认 10MB/s）
     * @param suspiciousThresholdBps 可疑阈值（默认 50MB/s）
     * @param burstMultiplier      突发倍数阈值（默认 3.0x 基线）
     */
    std::vector<TrafficAnomaly> detectAnomalies(int intervalSec, 
                                               uint64_t burstThresholdBps = 10*1024*1024,
                                               uint64_t suspiciousThresholdBps = 50*1024*1024,
                                               double burstMultiplier = 3.0);

    /// 获取所有流的历史记录（供上层 UI 或诊断）
    std::map<std::string, TrafficHistory> getTrafficHistory();

    /// 动态调整异常检测参数（运行时可调）
    void setAnomalyDetectionParams(uint64_t burstThreshold, uint64_t suspiciousThreshold, double burstMultiplier);

    /**
     * @brief 获取实时流量统计快照
     *
     * 从内核态 map 直接读取累计值，差分后返回速率。
     */
    struct RealTimeStats {
        uint64_t totalBps = 0;       ///< 当前总字节速率
        uint64_t totalPps = 0;       ///< 当前总包速率
        size_t activeFlows = 0;       ///< 活跃流数量
        std::chrono::system_clock::time_point timestamp;
    };
    RealTimeStats getRealTimeStats();

    /// 清空所有历史数据（适配器重启、周期性归零等场景）
    void clearHistory();

private:
    NetTrafficAnalyzer() = default;
    static std::once_flag s_onceFlag;
    static std::shared_ptr<NetTrafficAnalyzer> s_instance;

    std::string bpfObjPath_ = "build/flow_rate.bpf.o";
    std::string boundIface_;

    // ---- eBPF 句柄 ----
    void* bpfObj_ = nullptr;         ///< bpf_object*（libbpf 不暴露具体类型）
    void* linkTcp_ = nullptr;        ///< TCP hook 的 bpf_link*
    void* linkUdp_ = nullptr;        ///< UDP hook 的 bpf_link*
    int mapCurrFd_ = -1;             ///< 当前流量累计 map fd
    int mapCfgFd_  = -1;             ///< 配置 map fd
    int mapProcessStatsFd_ = -1;     ///< process_stats map fd（供 ProcessNetProfiler 共享）
    bool attached_ = false;

    // ---- 异常检测相关 ----
    mutable std::mutex historyMutex_;  ///< 保护 trafficHistory_
    std::map<std::string, TrafficHistory> trafficHistory_;
    mutable std::mutex mapMutex_;      ///< 保护 map fd 访问
    uint64_t burstThresholdBps_ = 10*1024*1024;      ///< 10MB/s
    uint64_t suspiciousThresholdBps_ = 50*1024*1024;  ///< 50MB/s
    double burstMultiplier_ = 3.0;                   ///< 突发倍数阈值
    static constexpr size_t MAX_HISTORY_SIZE = 60;   ///< 保留最多 60 个历史记录

    // ---- 内部辅助 ----
    /// 生成流唯一 key（src:sport→dst:dport proto）
    std::string generateFlowKey(const FlowRate& flow);
    /// 判断是否突发流量（当前 bps > burstMultiplier × 基线均值）
    bool isBurstTraffic(const TrafficHistory& history, uint64_t currentBps);
    /// 判断是否可疑流量（绝对阈值 + pid=0 等特征）
    bool isSuspiciousTraffic(uint64_t currentBps, uint32_t pid);
    /// 计算异常严重程度（0.0-1.0，基于超出阈值的倍数）
    double calculateSeverity(uint64_t currentBps, uint64_t threshold, double multiplier);
};

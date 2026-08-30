/**
 * @file ebpf_monitor_interface.hpp
 * @brief eBPF 监控器统一接口（抽象基类）
 *
 * 所有 eBPF 监控器（DnsMonitor / HttpLatencyMonitor / WifiPacketLossMonitor /
 * TcpRetransMonitor / ProcessNetProfiler / BtAudioAnalyzer）都继承 IEbpfMonitor，
 * 提供统一的健康查询和指标读取能力。
 *
 * 设计动机：
 *   上层（server.cpp / dbus_service.cpp）不需要知道具体实现，
 *   通过 IEbpfMonitor 指针数组遍历即可完成健康检查和指标聚合。
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace weaknet_dbus {

/// eBPF 监控器生命周期状态机
enum class EbpfMonitorState {
    Uninitialized,   ///< 尚未调用 init()
    Initializing,    ///< init() 进行中（加载 BPF 对象、挂载程序）
    Attached,        ///< 已成功挂载，正常运行
    Fallback,        ///< 挂载失败，已降级为离线模式（用户空间不崩溃）
    Error,           ///< 运行期发生不可恢复错误
    Stopped          ///< 已显式 stop()
};

/// 将状态枚举转为人类可读字符串（日志/D-Bus 上报用）
const char* ebpfMonitorStateName(EbpfMonitorState state);

/// 监控器健康快照（用于 D-Bus GetHealth 方法上报）
struct EbpfMonitorHealth {
    std::string name;                   ///< 监控器名称（如 "DnsMonitor"）
    EbpfMonitorState state = EbpfMonitorState::Uninitialized;
    bool available = false;             ///< eBPF 程序是否挂载成功（Attached 或 Fallback 之外）
    bool healthy = false;               ///< 最近一次 map 读取是否成功（available && consecutiveErrors < 阈值）
    uint64_t lastSuccessfulSampleNs = 0;  ///< 最近一次成功采样的时间戳（CLOCK_MONOTONIC ns）
    uint64_t consecutiveErrors = 0;     ///< 连续 map 读取失败次数（用于告警）
    uint64_t totalErrors = 0;           ///< 累计错误次数
    std::string status;                 ///< 人类可读状态描述（如 "ok"、"fallback: no cap"）
};

/// 监控器运行指标（Prometheus 风格，可用于导出）
struct EbpfMonitorMetrics {
    uint64_t attachedProbes = 0;       ///< 成功挂载的 kprobe/kretprobe/uprobe 数量
    uint64_t mapReads = 0;             ///< 累计 map 读取次数
    uint64_t mapReadErrors = 0;        ///< map 读取失败次数
    uint64_t samples = 0;              ///< 有效采样次数
    uint64_t totalReadTimeUs = 0;      ///< map 读取累计耗时（微秒）
    uint64_t averageReadTimeUs = 0;    ///< map 读取平均耗时（微秒，定期重新计算）
    std::string lastError;             ///< 最近一次错误信息
};

/**
 * @brief eBPF 监控器抽象接口
 *
 * 所有 eBPF 监控器必须实现此接口，提供统一的健康/指标查询。
 * 实现类通常还会扩展自己的 init() / stop() / getStats() 等方法，
 * 这些方法不在接口中（因为参数差异大），由调用方通过 dynamic_cast
 * 或直接持有具体类型指针访问。
 */
class IEbpfMonitor {
public:
    virtual ~IEbpfMonitor() = default;

    /// 监控器名称（用于日志和健康查询区分）
    virtual const char* monitorName() const = 0;

    /// 当前生命周期状态（EbpfMonitorState）
    virtual EbpfMonitorState commonState() const = 0;

    /// eBPF 程序是否可用（是否至少有一个挂点成功）
    virtual bool isAvailable() const = 0;

    /// 健康快照
    virtual EbpfMonitorHealth health() const = 0;

    /// 运行指标快照
    virtual EbpfMonitorMetrics metrics() const = 0;

    /// 重置所有统计指标（用于测试或周期性归零）
    virtual void resetMetrics() = 0;
};

}  // namespace weaknet_dbus

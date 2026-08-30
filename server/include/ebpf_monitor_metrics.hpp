/**
 * @file ebpf_monitor_metrics.hpp
 * @brief eBPF 监控器运行指标追踪与状态管理
 *
 * 提供两个辅助类，封装 EbpfMonitorHealth / EbpfMonitorMetrics 的内部管理：
 *
 *   - EbpfMonitorMetricsTracker：纯指标追踪（map 读取耗时、成功率、错误计数）
 *   - EbpfMonitorStateSupport：状态机 + 指标追踪的组合体，供各监控器作为成员复用
 *
 * 使用方式：各 eBPF 监控器持有 EbpfMonitorStateSupport 成员，
 * 在 init() 中调用 setState(Attached, true)，在每个采样周期调用
 * recordReadSuccess/recordReadFailure，健康查询直接转发给 stateSupport_。
 *
 * 线程安全：所有公开方法均加锁（mutex_ / tracker_.mutex_）。
 */

#pragma once

#include "ebpf_monitor_interface.hpp"
#include <chrono>
#include <mutex>

namespace weaknet_dbus {

/**
 * @brief eBPF 监控器运行指标追踪器
 *
 * 负责记录 map 读取耗时、错误计数、连续失败次数，
 * 并定期（每次 snapshot 时）重新计算 averageReadTimeUs。
 */
class EbpfMonitorMetricsTracker {
public:
    /// 记录一个 probe（kprobe/kretprobe/uprobe）成功挂载
    void recordProbeAttached();

    /**
     * @brief 记录一次成功的 map 读取
     * @param elapsedUs  本次读取耗时（微秒）
     * @param sample     是否计为有效采样（默认 true；心跳检查可传 false）
     */
    void recordReadSuccess(uint64_t elapsedUs, bool sample = true);

    /// 记录一次失败的 map 读取（同时增加 consecutiveErrors 和 totalErrors）
    void recordReadFailure(const std::string& error);

    /// 当前连续错误次数（用于上层判断 healthy）
    uint64_t consecutiveErrors() const;

    /// 最近一次成功采样的时间戳（CLOCK_MONOTONIC ns）
    uint64_t lastSuccessfulSampleNs() const;

    /// 获取指标快照（计算 averageReadTimeUs 后返回副本）
    EbpfMonitorMetrics snapshot() const;

    /// 清零所有指标（用于测试或周期性归零）
    void reset();

private:
    mutable std::mutex mutex_;
    EbpfMonitorMetrics metrics_;
    uint64_t consecutiveErrors_ = 0;
    uint64_t lastSuccessfulSampleNs_ = 0;

    friend class EbpfMonitorStateSupport;  // 允许直接访问 metrics_ / consecutiveErrors_
};

/**
 * @brief eBPF 监控器状态机 + 指标追踪组合体
 *
 * 各 eBPF 监控器通常持有一个 EbpfMonitorStateSupport 成员，
 * 将状态转换、指标记录、健康查询全部委托给它，
 * 自己只实现 init() / stop() / getStats() 等具体逻辑。
 *
 * 使用示例：
 * @code
 *   class DnsMonitor : public IEbpfMonitor {
 *       EbpfMonitorStateSupport stateSupport_{"DnsMonitor"};
 *   public:
 *       bool init(const std::string& path) {
 *           stateSupport_.setState(EbpfMonitorState::Initializing, false);
 *           // ... 加载 BPF ...
 *           if (ok) { stateSupport_.setState(EbpfMonitorState::Attached, true); }
 *           else    { stateSupport_.setState(EbpfMonitorState::Fallback, false, "no cap"); }
 *       }
 *       void sample() {
 *           auto t0 = now();
 *           if (read_map_ok) stateSupport_.recordReadSuccess(elapsed_us);
 *           else             stateSupport_.recordReadFailure(err_msg);
 *       }
 *       EbpfMonitorHealth health() const override { return stateSupport_.health(); }
 *   };
 * @endcode
 */
class EbpfMonitorStateSupport {
public:
    /// 构造时传入监控器名称（用于 health() 返回）
    explicit EbpfMonitorStateSupport(const char* name) : name_(name) {}

    /**
     * @brief 设置状态
     * @param state      新状态
     * @param available  eBPF 是否可用（IsAttached 时为 true，Fallback 时也可为 true）
     * @param status     人类可读状态描述（默认空）
     */
    void setState(EbpfMonitorState state, bool available, const std::string& status = "");

    /**
     * @brief 根据一次读取结果自动更新状态
     *
     * 读取成功：重置 consecutiveErrors，刷新 lastSuccessfulSampleNs；
     * 读取失败：增加 consecutiveErrors 和 totalErrors。
     *
     * @param success  本次 map 读取是否成功
     * @param error    失败时的错误信息
     */
    void setStateFromRead(bool success, const std::string& error = "");

    /// 生成健康快照（healthy = available && consecutiveErrors < 3）
    EbpfMonitorHealth health() const;

    /// 当前生命周期状态
    EbpfMonitorState state() const;

    /// eBPF 是否可用
    bool isAvailable() const;

    /// 重置指标
    void resetMetrics();

    /// 获取指标快照（直接转发给 tracker_）
    EbpfMonitorMetrics metrics() const { return tracker_.snapshot(); }

    /// 成功挂载了一个 probe（转发）
    void recordProbeAttached() { tracker_.recordProbeAttached(); }

    /// 成功读取了一次 map（转发，const 版本以允许在 const 成员函数中调用）
    void recordReadSuccess(uint64_t elapsedUs, bool sample = true) const { tracker_.recordReadSuccess(elapsedUs, sample); }

    /// 失败读取了一次 map（转发，const 版本）
    void recordReadFailure(const std::string& error) const { tracker_.recordReadFailure(error); }

private:
    const char* name_;                          ///< 监控器名称（非 owning，生命周期由外部保证）
    mutable std::mutex mutex_;                  ///< 保护 state_ / available_ / status_
    EbpfMonitorState state_ = EbpfMonitorState::Uninitialized;
    bool available_ = false;
    std::string status_;
    mutable EbpfMonitorMetricsTracker tracker_;  ///< 指标追踪（内部自带锁）
};

}  // namespace weaknet_dbus

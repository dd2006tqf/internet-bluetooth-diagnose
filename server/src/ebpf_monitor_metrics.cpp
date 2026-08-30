/**
 * @file ebpf_monitor_metrics.cpp
 * @brief eBPF 监控器通用指标追踪与状态管理实现
 *
 * 本文件实现了 EbpfMonitorMetricsTracker 和 EbpfMonitorStateSupport 两个工具类，
 * 为所有 eBPF 监控器（DNS/HTTP/Wi-Fi/TCP 重传/进程画像）提供统一的：
 *   - 探针挂载计数（recordProbeAttached）
 *   - BPF Map 读取耗时统计（recordReadSuccess）
 *   - 错误计数与连续错误追踪（recordReadFailure）
 *   - 健康状态查询（health）
 *
 * 设计目标：
 *   - 线程安全：所有方法均通过 std::mutex 保护内部状态
 *   - 轻量级：无堆分配，适合在高频热路径调用（每次 Map 读取都会调用）
 *   - 可观测性：支持将指标快照序列化为 JSON，用于 Web UI 展示监控器健康度
 *
 * 线程模型：
 *   - EbpfMonitorMetricsTracker：被多个监控线程（如 DNS/HTTP/TCP）并发调用，
 *     通过 std::mutex 保证 metrics_ 和错误计数器的原子更新
 *   - EbpfMonitorStateSupport：封装 EbpfMonitorMetricsTracker，额外持有
 *     state_（初始化/挂载/回退/错误/停止）和 available_ 标志
 */

#include "ebpf_monitor_metrics.hpp"
#include <chrono>

namespace weaknet_dbus {

namespace {
/**
 * @brief 获取当前单调时钟时间戳（纳秒）
 *
 * 使用 steady_clock 而非 system_clock，避免 NTP 时钟跳跃影响差值计算。
 * @return CLOCK_MONOTONIC 时间戳（纳秒）
 */
uint64_t monotonicNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
}

/**
 * @brief 将 EbpfMonitorState 枚举值转为可读字符串
 *
 * 用于日志输出和 JSON 序列化
 * @param state 监控器状态枚举
 * @return 状态字符串："uninitialized" / "initializing" / "attached" / "fallback" / "error" / "stopped"
 */
const char* ebpfMonitorStateName(EbpfMonitorState state) {
    switch (state) {
        case EbpfMonitorState::Uninitialized: return "uninitialized";
        case EbpfMonitorState::Initializing: return "initializing";
        case EbpfMonitorState::Attached: return "attached";
        case EbpfMonitorState::Fallback: return "fallback";
        case EbpfMonitorState::Error: return "error";
        case EbpfMonitorState::Stopped: return "stopped";
    }
    return "unknown";
}

/**
 * @brief 记录探针挂载成功
 *
 * 每次成功挂载一个 BPF 探针（kprobe/tracepoint/kretprobe）时调用，
 * 用于最终诊断"该监控器到底挂了几个探针"。
 */
void EbpfMonitorMetricsTracker::recordProbeAttached() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++metrics_.attachedProbes;
}

/**
 * @brief 记录一次 BPF Map 读取成功
 *
 * 更新平均读取耗时、最后成功采样时间戳、连续错误计数器归零。
 *
 * @param elapsedUs 本次 Map 读取耗时（微秒），由调用方传入
 * @param sample    true 表示本次读取返回了有效数据（result 非空），统计入 samples
 */
void EbpfMonitorMetricsTracker::recordReadSuccess(uint64_t elapsedUs, bool sample) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++metrics_.mapReads;
    if (sample) ++metrics_.samples;
    metrics_.totalReadTimeUs += elapsedUs;
    // 平均耗时 = 累计耗时 / 读取次数（避免除以零）
    metrics_.averageReadTimeUs = metrics_.mapReads == 0 ? 0 : metrics_.totalReadTimeUs / metrics_.mapReads;
    lastSuccessfulSampleNs_ = monotonicNs();  // 记录成功采样的时间戳
    consecutiveErrors_ = 0;                    // 连续错误归零
}

/**
 * @brief 记录一次 BPF Map 读取失败
 *
 * 累计失败次数和连续错误计数，用于触发降级或告警。
 *
 * @param error 错误描述字符串（如 "map unavailable"、"map lookup failed"）
 */
void EbpfMonitorMetricsTracker::recordReadFailure(const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++metrics_.mapReadErrors;
    metrics_.lastError = error;
    ++consecutiveErrors_;
}

/**
 * @brief 获取指标快照（深拷贝，无副作用）
 * @return EbpfMonitorMetrics 快照
 */
EbpfMonitorMetrics EbpfMonitorMetricsTracker::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metrics_;
}

/**
 * @brief 获取连续错误次数
 *
 * 典型用途：连续错误 > 5 次时，上层可以触发监控器重启或降级处理
 * @return 自上次成功采样以来的连续错误次数
 */
uint64_t EbpfMonitorMetricsTracker::consecutiveErrors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return consecutiveErrors_;
}

/**
 * @brief 获取最后一次成功采样的时间戳（纳秒）
 *
 * 典型用途：上层可以判断监控器是否"卡死"——
 * 如果 lastSuccessfulSampleNs 超过 N 秒没更新，说明 BPF Map 可能一直读不到数据
 * @return CLOCK_MONOTONIC 时间戳（纳秒）；从未成功过返回 0
 */
uint64_t EbpfMonitorMetricsTracker::lastSuccessfulSampleNs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastSuccessfulSampleNs_;
}

/**
 * @brief 重置所有指标（清零）
 *
 * 在监控器重启或配置变更后调用
 */
void EbpfMonitorMetricsTracker::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_ = {};
    consecutiveErrors_ = 0;
    lastSuccessfulSampleNs_ = 0;
}

// ---- EbpfMonitorStateSupport ----

/**
 * @brief 更新监控器状态和可用性
 *
 * 在 init/stop/fallback 等状态迁移点调用
 *
 * @param state     新状态（Initializing/Attached/Fallback/Stopped/Error）
 * @param available 是否可用（libbpf 不可用或所有探针 attach 失败时为 false）
 * @param status    人类可读的状态描述（如 "BPF probes attached"）
 */
void EbpfMonitorStateSupport::setState(EbpfMonitorState state, bool available, const std::string& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = state;
    available_ = available;
    status_ = status;
}

/**
 * @brief 基于 Map 读取结果更新状态
 *
 * 成功时清除错误描述，失败时记录错误字符串。
 * 此方法会调用内部 tracker_ 的 recordReadSuccess/recordReadFailure，
 * 保证 Metrics 和 State 的一致性。
 *
 * @param success 本次 Map 读取是否成功
 * @param error   失败时的错误描述（成功时忽略）
 */
void EbpfMonitorStateSupport::setStateFromRead(bool success, const std::string& error) {
    if (success) {
        tracker_.recordReadSuccess(0);
        std::lock_guard<std::mutex> lock(mutex_);
        status_.clear();
    } else {
        tracker_.recordReadFailure(error);
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = error;
    }
}

/**
 * @brief 生成健康状态快照
 *
 * 将 state/available/tracker_ 的聚合指标打包为 EbpfMonitorHealth，
 * 供上层 Web UI 展示单个监控器的健康度仪表盘。
 *
 * @return 包含 name/state/available/healthy/错误计数等字段的健康快照
 */
EbpfMonitorHealth EbpfMonitorStateSupport::health() const {
    std::lock_guard<std::mutex> lock(mutex_);
    EbpfMonitorHealth result;
    result.name = name_;
    result.state = state_;
    result.available = available_;
    auto metrics = tracker_.snapshot();
    result.lastSuccessfulSampleNs = tracker_.lastSuccessfulSampleNs();
    result.consecutiveErrors = tracker_.consecutiveErrors();
    result.totalErrors = metrics.mapReadErrors;
    result.status = status_;
    // 健康判定：可用 + 状态为 Attached（探针已挂载）
    result.healthy = available_ && (state_ == EbpfMonitorState::Attached);
    return result;
}

/**
 * @brief 查询当前状态枚举值
 * @return EbpfMonitorState 状态值
 */
EbpfMonitorState EbpfMonitorStateSupport::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

/**
 * @brief 查询监控器是否可用
 * @return true 可用（libbpf 可用且探针至少有一个挂载成功）
 */
bool EbpfMonitorStateSupport::isAvailable() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return available_;
}

/**
 * @brief 重置 Metrics 追踪器（状态和可用性保持不变）
 */
void EbpfMonitorStateSupport::resetMetrics() {
    tracker_.reset();
}

}  // namespace weaknet_dbus

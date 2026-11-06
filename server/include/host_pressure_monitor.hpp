/**
 * @file host_pressure_monitor.hpp
 * @brief 采集端自身 CPU 压力监控器
 *
 * 目的不是"多采一个指标"，而是补上证据链里的一个真实漏洞：
 *
 *   现有 Observer 质量判定（assurance/dns_service_evaluator.hpp 的
 *   checkObserverQuality）只看 eBPF 的 capture_emit_failure_ratio。
 *   但"BPF 掉证据"有两个完全不同的成因：
 *     (a) 网络/内核真的出了问题
 *     (b) 板子 CPU 被打满，采集线程抢不到 CPU，perf buffer 来不及消费
 *   旧证据无法区分这两者，于是 (b) 会被误判成网络故障。
 *
 * 本监控器把"观测者自身状态"变成可判定的证据，让归因能往下分辨一层。
 *
 * 数据来源：kmod/weaknet_cpu_stat，内核 hrtimer 每 4 ms 采集 per-CPU 累计时间
 * （user/nice/system/idle/iowait/irq/softirq/steal/guest/guest_nice），
 * 经 /dev/weaknet_cpu_stat 只读 mmap 暴露，seqlock 保证快照一致性。
 * 该模块已实测 30 分钟长稳：5851 万次读取零失败、进程 RSS 零增长。
 *
 * 重要语义边界（不要越过）：
 *   CPU 忙 只能说明"采集端可能来不及处理"，**不能**说明网络坏了，
 *   也**不能**说明网络没坏。它是归因辅助证据，不是故障判据本身。
 *
 * 可用性降级（关键）：
 *   内核模块不在 OTA manifest 里，现场设备大概率没有这个设备节点。
 *   此时必须发布 UNAVAILABLE 并让整条业务链路表现得与引入本插件之前
 *   完全一致 —— 绝不伪造 0，也绝不因为缺数据而改变任何已有判定。
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace weaknet_dbus {

class ServerContext;

/**
 * @brief 一次 CPU 压力采样结果
 */
struct HostCpuPressure {
    /// 本轮是否得到了可用的比率。false 时 @busy_ratio 无意义。
    bool available{false};

    /// 全系统 CPU 忙碌占比 0~1。
    ///
    /// 多核**按累计时间加权**：sum(busy) / sum(total)，不是各核比率的算术平均。
    /// 这不是风格问题而是语义问题：8 核中 1 核 100%、其余空闲时两者都等于 0.125，
    /// 但当各核 total 不等（CPU 隔离、tickless 空闲核）时会分叉，
    /// 只有加权和才等于"系统整体花在 busy 上的时间比例"。
    double busy_ratio{0.0};

    /// 参与本轮差值的 CPU 数（已排除离线核与累计值被热插拔重置的核）
    uint32_t sampled_cpus{0};

    /// 系统在线 CPU 总数（含本轮未参与差值的核）
    uint32_t online_cpus{0};

    /// available=false 时的机器可读原因：
    ///   "device_absent"    设备节点不存在（模块未加载）
    ///   "open_failed"      打开失败（权限/被占用）
    ///   "mmap_failed"      映射失败（ABI 尺寸不匹配）
    ///   "abi_mismatch"     abi_version 不符
    ///   "baseline_only"    首轮采样，只建立基线，无差值可算
    ///   "read_failed"      seqlock 重试耗尽
    ///   "no_delta_cpus"    两次采样间没有任何核可做差值
    std::string reason;
};

/**
 * @brief CPU 压力采样器
 *
 * 有状态：保留上一份快照才能做区间差值。
 *
 * 为什么必须做差值而不是直接用累计值：
 *   /dev/weaknet_cpu_stat 发布的是**自开机以来的累计纳秒**。
 *   直接用累计值算比率得到的是"开机至今的平均负载"——一个持续十小时的
 *   开机过程里，本轮评估期间的瞬时尖峰会被完全抹平，用来判断
 *   "这一轮采集端是否过载"毫无意义。只有相邻两次采样的差值反映当前区间。
 *
 * 线程安全：非线程安全。同一实例只应由一个线程持有使用。
 */
class HostCpuPressureSampler {
public:
    explicit HostCpuPressureSampler(std::string dev_path = "/dev/weaknet_cpu_stat");
    ~HostCpuPressureSampler();

    HostCpuPressureSampler(const HostCpuPressureSampler&) = delete;
    HostCpuPressureSampler& operator=(const HostCpuPressureSampler&) = delete;

    /**
     * @brief 采一次样并与上一样本做差值
     *
     * 首次调用只建立基线，返回 available=false, reason="baseline_only"。
     * 这是刻意行为：没有前一份样本时任何比率都是凭空捏造。
     *
     * 设备不存在时返回 available=false, reason="device_absent"；
     * 调用方据此发布 UNAVAILABLE，不得回落成 0。
     */
    HostCpuPressure sample();

    const std::string& devPath() const { return dev_path_; }

private:
    struct Impl;

    std::string dev_path_;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief 启动 CPU 压力监控线程
 *
 * 循环内每轮从线程安全配置现读 interval_ms，支持 D-Bus 实时调参。
 * 采样结果经 MetricsRegistry::publishScope(MetricScope::HOST, ...) 发布，
 * 命名空间键为 "host:"（见 metrics_registry.hpp 的 scopeKey）。
 */
void start_host_pressure_monitor_thread(ServerContext* ctx, std::thread* worker);

}  // namespace weaknet_dbus

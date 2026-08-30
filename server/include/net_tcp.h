/**
 * @file net_tcp.h
 * @brief TCP 丢包率采样与计算工具（单例）
 *
 * 实现 Phase 1 丢包率监控方案：从 /proc/net/snmp 读取全局 TCP 计数，
 * 对两次采样做差分计算丢包率。
 *
 * Phase 2 替代方案：TcpRetransMonitor（eBPF）提供连接粒度的重传统计，
 * 更精准但需要更高内核版本和权限。本工具作为降级后备。
 *
 * 线程安全：getInstance() 使用 std::once_flag 保证线程安全懒汉初始化。
 *           sample() 内部读取 /proc 文件（原子读），可多线程并发。
 */

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

/// TCP 累计计数快照（对应 /proc/net/snmp Tcp 段）
struct TcpStats {
    uint64_t inSegs = 0;      ///< Tcp: InSegs（累计接收段数）
    uint64_t outSegs = 0;     ///< Tcp: OutSegs（累计发送段数）
    uint64_t retransSegs = 0; ///< Tcp: RetransSegs（累计重传段数）
    bool valid = false;       ///< 本次采样是否成功
};

/// 丢包率计算结果（包含分级）
struct TcpLossResult {
    double ratePercent = 0.0;   ///< 丢包率 = (deltaRetrans / deltaOut) * 100
    uint64_t sentDelta = 0;     ///< 本次采样的发送段增量
    uint64_t retransDelta = 0;  ///< 本次采样的重传段增量
    std::string level;          ///< "good" / "degraded" / "poor" / "insufficient"
};

/**
 * @brief TCP 丢包率监控器（Phase 1，/proc/net/snmp 差分方案）
 *
 * 单例模式，通过 getInstance() 获取。
 * 典型用法：
 * @code
 *   auto mon = TcpLossMonitor::getInstance();
 *   TcpStats prev, curr;
 *   mon->sample(prev);
 *   sleep(2);
 *   mon->sample(curr);
 *   auto result = mon->compute(prev, curr);
 * @endcode
 */
class TcpLossMonitor {
public:
    /// 线程安全懒汉单例
    static std::shared_ptr<TcpLossMonitor> getInstance();

    /**
     * @brief 读取全局 TCP 计数（来自 /proc/net/snmp）
     * @param outStats 输出快照（valid=false 表示采样失败）
     * @return true 采样成功
     */
    bool sample(TcpStats& outStats);

    /**
     * @brief 针对指定接口名统计（预留扩展，目前与 sample() 行为相同）
     *
     * 未来可接入 netlink per-interface tcp_info，实现接口粒度统计。
     *
     * @param ifaceName 网卡名（预留参数，当前不区分）
     * @param outStats  输出快照
     * @return true 采样成功
     */
    bool sampleForInterface(const std::string& ifaceName, TcpStats& outStats);

    /**
     * @brief 基于两次采样计算丢包率
     *
     * @param prev                 前一次采样
     * @param curr                 当前采样
     * @param minSent              最小发送段阈值（默认 10），低于此值流量不足
     * @param degradedThresholdPct degraded 等级下限（默认 1.0%）
     * @param poorThresholdPct     poor 等级下限（默认 5.0%）
     * @return 丢包率计算结果（含 level 分级）
     */
    TcpLossResult compute(const TcpStats& prev,
                          const TcpStats& curr,
                          uint64_t minSent = 10,
                          double degradedThresholdPct = 1.0,
                          double poorThresholdPct = 5.0);

private:
    TcpLossMonitor() = default;
    static std::once_flag s_onceFlag;
    static std::shared_ptr<TcpLossMonitor> s_instance;
};

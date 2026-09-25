/**
 * @file net_tcp.h
 * @brief TCP 丢包率采样与计算工具（单例）
 *
 * 实现方式：通过 netlink SOCK_DIAG（inet_diag）向内核请求 TCP socket
 * 诊断信息，累加 tcp_info.tcpi_total_retrans 作为分子、接口粒度近似发送
 * 段数作为分母，对两次采样做差分计算丢包率。
 *
 * Phase 2 替代方案：TcpRetransMonitor（eBPF）提供连接粒度的重传统计，
 * 更精准但需要更高内核版本和权限。本工具作为降级后备。
 *
 * 线程安全：getInstance() 使用 std::once_flag 保证线程安全懒汉初始化。
 *           sampleForInterface() 每次调用自建 netlink socket、无共享状态，
 *           可多线程并发。
 */

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

/// TCP 累计计数快照（netlink SOCK_DIAG 近似统计）
///
/// 说明：这里没有"接收段数"字段。此前存在过 inSegs，但它在
/// diagDumpFamilyIface 内只被初始化为 0、从未累加（内核 inet_diag_msg
/// 不提供该维度），且全仓无任何读者，属于恒为 0 的死输出，已删除。
struct TcpStats {
    uint64_t outSegs = 0;     ///< 近似发送段数（分母）
    uint64_t retransSegs = 0; ///< 累计重传段数（分子）
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
 * @brief TCP 丢包率监控器（netlink SOCK_DIAG 差分方案，单例）
 *
 * 单例模式，通过 getInstance() 获取。
 * 典型用法：
 * @code
 *   auto mon = TcpLossMonitor::getInstance();
 *   TcpStats prev, curr;
 *   mon->sampleForInterface("wlan0", prev);
 *   sleep(10);
 *   mon->sampleForInterface("wlan0", curr);
 *   auto result = mon->compute(prev, curr);
 * @endcode
 *
 * 说明：此前还有一个全局版 sample()（不过滤接口、累加全系统 socket）。
 * 它无任何调用者——生产路径只用带接口过滤的 sampleForInterface()，
 * 因为丢包率必须归因到"当前上网网卡"，全系统计数无法回答这个问题。
 * 该全局接口已删除，避免保留第二个含义不同的采样入口。
 */
class TcpLossMonitor {
public:
    /// 线程安全懒汉单例
    static std::shared_ptr<TcpLossMonitor> getInstance();

    /**
     * @brief 采样指定接口的 TCP 统计（IPv4 + IPv6 两地址族，按 idiag_if 过滤）
     *
     * @param ifaceName 网卡名（如 "eth0"、"wlan0"）
     * @param outStats  输出快照（valid=false 表示采样失败）
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

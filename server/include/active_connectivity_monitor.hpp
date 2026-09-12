#pragma once

/**
 * @file active_connectivity_monitor.hpp
 * @brief 受控主动连通性探测 — 网络 I/O 层
 *
 * ## 职责边界（硬约束）
 *
 *   本类负责**主动 I/O**，产出 ProbeTargetResult。
 *   语义判定在 ActiveConnectivityEvaluator（纯函数），
 *   本类不做任何状态/阈值判断，也不持有 SLE 结论。
 *
 * ## 为什么 DNS 探测自构造报文，而不用 getaddrinfo
 *
 *   libc resolver 会发出调用方并不等待的附加查询（AAAA/MX 等），
 *   这些查询在 DNS 捕获视角下就是"发出但无响应"。此前正是因此把
 *   一个完全健康的解析器误判为故障。
 *   自构造 UDP 查询可精确指定 QTYPE 并控制超时，测得的是
 *   "配置的 resolver 能否解析这个 QNAME"，语义明确且可复现。
 *
 * ## 本版本覆盖范围
 *
 *   DNS / TCP  —— 已实现
 *   TLS / HTTPS / Portal —— 未实现（无 TLS 开发依赖）。
 *   两者在 evaluator 中明确返回 NO_CAPABILITY，绝不伪造。
 */

#include "assurance/active_connectivity.hpp"
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace weaknet_dbus {

/// 单个探测目标配置
struct ActiveProbeTargetConfig {
    std::string id;
    std::string hostname;
    uint16_t tcp_port{443};
};

struct ActiveProbeConfig {
    bool enabled{false};
    uint32_t interval_sec{30};
    uint32_t timeout_sec{3};
    std::vector<ActiveProbeTargetConfig> targets;
};

/**
 * @brief 主动连通性探测器
 *
 * 线程安全：runProbeRound() 由探测线程独占调用；results() 可被评估线程读取。
 */
class ActiveConnectivityMonitor {
public:
    ActiveConnectivityMonitor() = default;

    /// 重新配置（启动时一次；运行中变更目标需重启服务）
    void configure(const ActiveProbeConfig& cfg);

    const ActiveProbeConfig& config() const { return cfg_; }

    /// 配置是否可用（enabled 且至少有 1 个目标）
    bool isUsable() const { return cfg_.enabled && !cfg_.targets.empty(); }

    /**
     * @brief 执行一轮探测（对全部目标）
     * @return 本轮各目标的原始结果
     */
    std::vector<weaknet::ProbeTargetResult> runProbeRound();

    /// 最近一轮结果（拷贝）
    std::vector<weaknet::ProbeTargetResult> results() const;

    /// 启动时的配置摘要（用于 startup log）
    std::string describeConfig() const;

private:
    ActiveProbeConfig cfg_;
    std::vector<weaknet::ProbeTargetResult> last_results_;
    mutable std::mutex mutex_;
    uint64_t round_counter_{0};
};

}  // namespace weaknet_dbus

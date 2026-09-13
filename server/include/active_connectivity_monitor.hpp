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
 *   DNS / TCP            —— 已实现
 *   TLS / HTTPS          —— 由 TlsProbeClient 实现（复用 TCP 已建立的 fd）
 *   Captive Portal       —— 受控 oracle，明文 HTTP + 预期响应比对
 *
 * 编译期无 TLS 依赖时（WEAKNET_HAVE_TLS 未定义），TLS/HTTP 阶段保持
 * attempted=false，由 evaluator 表达为 NO_CAPABILITY，绝不伪造。
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
    /// 故障域标识（默认取 hostname）。
    /// Portal quorum 要求信号来自不同故障域，避免同一 CDN 的多个域名
    /// 被当成独立目标凑数。
    std::string failure_domain;
};

/// Portal oracle 的单次探测配置
struct PortalProbeConfig {
    bool enabled{false};
    std::string path{"/"};
    /// 响应正文必须包含的子串；空则跳过正文比对
    std::string expect_body;
    std::vector<ActiveProbeTargetConfig> targets;
};

struct ActiveProbeConfig {
    bool enabled{false};
    uint32_t interval_sec{30};
    uint32_t timeout_sec{3};
    std::vector<ActiveProbeTargetConfig> targets;
    /// 是否在 TCP 成功后继续做 TLS + HTTP（HTTPS capability）
    bool https_enabled{true};
    PortalProbeConfig portal;
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

    /// 最近一轮 Portal oracle 结果（由 runProbeRound 一并产出）
    std::vector<weaknet::ProbeTargetResult> portalResults() const;

    /// 运行期是否具备 TLS 能力（编译期决定）
    static bool tlsAvailable();

    /// 启动时的配置摘要（用于 startup log）
    std::string describeConfig() const;

private:
    /// Portal oracle 探测（明文 HTTP，独立于 capability 目标）
    std::vector<weaknet::ProbeTargetResult>
    runPortalRound(const ActiveProbeConfig& cfg, const std::string& resolver);

    ActiveProbeConfig cfg_;
    std::vector<weaknet::ProbeTargetResult> last_results_;
    std::vector<weaknet::ProbeTargetResult> last_portal_results_;
    mutable std::mutex mutex_;
    uint64_t round_counter_{0};
};

}  // namespace weaknet_dbus

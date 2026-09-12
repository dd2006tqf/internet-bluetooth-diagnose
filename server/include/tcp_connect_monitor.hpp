#pragma once

/**
 * @file tcp_connect_monitor.hpp
 * @brief TCP 建连（Connect）可观测性 — 用户态接口
 *
 * 数据源：tracepoint/sock/inet_sock_set_state（tcp_connect.bpf.c）
 *   该 tracepoint 在同一次事件中同时给出连接四元组与状态迁移，
 *   因此不需要任何跨 hook 关联（与 DNS 捕获遵循同一原则）。
 *
 * 上报的建连事实：
 *   SYN_SENT → ESTABLISHED   attempt + success
 *   SYN_SENT → CLOSE         attempt + failure
 *
 * 语义边界：本监控器只回答"TCP 建连是否成功、耗时多少"。
 *   TCP 重传率属于 Reliability SLE，不在此重复评价。
 */

#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <mutex>
#include <chrono>
#include <atomic>

#include "ebpf_monitor_interface.hpp"
#include "ebpf_monitor_metrics.hpp"

namespace weaknet_dbus {

/// 单次 TCP 建连的观测事实
struct TcpConnectObservation {
    uint32_t saddr{0};      ///< 本机地址（原始网络序字节）
    uint32_t daddr{0};      ///< 对端地址
    uint16_t sport{0};      ///< 本机端口（主机序）
    uint16_t dport{0};      ///< 对端端口
    bool     success{false};          ///< 是否最终 ESTABLISHED
    bool     attempted{false};        ///< 是否观察到 SYN_SENT
    double   latency_ms{0.0};         ///< SYN_SENT → 终态耗时
    std::chrono::steady_clock::time_point observed_at;
};

/// 建连聚合统计（供诊断与 Evidence Quality 使用）
struct TcpConnectStats {
    uint64_t attempts{0};
    uint64_t successes{0};
    uint64_t failures{0};
    uint64_t unmatched_terminal{0};   ///< 未见 SYN_SENT 的终态（证据不完整）

    uint64_t evaluable() const { return successes + failures; }
    double successRatio() const {
        const uint64_t e = evaluable();
        return e == 0 ? 0.0 : static_cast<double>(successes) / static_cast<double>(e);
    }
    double failureRatio() const {
        const uint64_t e = evaluable();
        return e == 0 ? 0.0 : static_cast<double>(failures) / static_cast<double>(e);
    }
};

/**
 * @brief TCP 建连监控器
 *
 * 与 DnsMonitor 同构：perf buffer 消费 + 有界排空 + 诊断计数器，
 * 复用 EbpfMonitorStateSupport 提供统一健康语义。
 */
class TcpConnectMonitor : public IEbpfMonitor {
public:
    TcpConnectMonitor();
    ~TcpConnectMonitor() override;

    bool init(const std::string& bpfObjPath = "build/tcp_connect.bpf.o",
              uint32_t capture_pages = 32);
    void stop();

    bool isInitialized() const { return initialized_; }
    bool isAvailable() const override { return available_; }

    // ---- IEbpfMonitor ----
    const char* monitorName() const override { return "TcpConnectMonitor"; }
    EbpfMonitorState commonState() const override { return stateSupport_.state(); }
    EbpfMonitorHealth health() const override { return stateSupport_.health(); }
    EbpfMonitorMetrics metrics() const override { return stateSupport_.metrics(); }
    void resetMetrics() override { stateSupport_.resetMetrics(); }

    /**
     * @brief 排空 perf buffer 并归并建连观测
     * @param window_duration 仅保留该窗口内的观测
     * @return 本次消费的事件数
     */
    size_t drain(std::chrono::milliseconds window_duration = std::chrono::milliseconds(120000));

    /// 当前窗口内的建连观测（拷贝，供无状态 evaluator 使用）
    std::vector<TcpConnectObservation> recentObservations() const;

    /// 当前窗口聚合统计
    TcpConnectStats stats() const;

    /// 捕获链路诊断（BPF 计数器 + drain 统计）
    std::string getCaptureDiagnostics();

    /// perf buffer 丢失事件（消费后清零）
    uint64_t consumeLostEvents();

public:
    struct Impl;   // 供 .cpp 内 perf 回调访问（与 DnsMonitor 同构）
private:
    std::unique_ptr<Impl> impl_;

    bool initialized_ = false;
    bool available_ = false;
    EbpfMonitorStateSupport stateSupport_{"TcpConnectMonitor"};
};

}  // namespace weaknet_dbus

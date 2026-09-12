#pragma once

/**
 * @file active_connectivity.hpp
 * @brief 受控主动连通性探测 — 数据模型与 SLE 评估
 *
 * ## 为什么需要主动探测
 *
 * 被动观测（real traffic）只能回答"观察到了什么"，目标由用户业务决定，
 * 可能本来就该失败。唯有**目标由我们选定**的受控探测，才能回答
 * "本机当前是否具备上网能力"。因此只有 Active Probe 证据有资格
 * 判定 INTERNET_ACCESS。
 *
 * ## 分层纪律（硬边界）
 *
 *   ActiveConnectivityMonitor  负责主动 I/O，产出 ProbeTargetResult
 *          ↓
 *   ActiveConnectivityEvaluator  纯函数，绝不发起网络请求
 *          ↓
 *   SleResult → OverallPolicy
 *
 * ## 依赖截断（避免一次故障污染多个 SLE）
 *
 *   DNS 失败 → 该 target 的 TCP/HTTPS/Portal 记为 blocked_by_dns（UNKNOWN），
 *             而不是四个 SLE 同时变负面。
 *   TCP 失败 → HTTPS/Portal 记为 blocked_by_tcp。
 *
 * ## 本轮覆盖范围
 *
 *   DNS / TCP  capability  —— 已实现
 *   HTTPS / Portal         —— NOT_AVAILABLE（无 TLS 开发依赖，
 *                             以"可复现构建"方式另行补齐，绝不手写 ABI
 *                             或 shell-out）。能力没有就明确 UNKNOWN/NONE，
 *                             不伪造，也不能因为 DNS+TCP 成功就推出 HTTPS 成功。
 */

#include "assurance/health_state.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace weaknet {

/// 单个探测阶段的原始事实
struct ProbeStageResult {
    bool attempted{false};      ///< 是否执行了该阶段
    bool success{false};
    double latency_ms{0.0};
    std::string detail;         ///< 失败原因 / 解析到的地址等（诊断用）
};

/**
 * @brief 单个目标的一次完整探测结果
 *
 * 保留各阶段独立事实，使阈值与 quorum 调整无需重做网络 I/O。
 */
struct ProbeTargetResult {
    std::string target_id;
    std::string hostname;
    uint16_t tcp_port{0};

    ProbeStageResult dns;
    ProbeStageResult tcp;
    ProbeStageResult tls;    ///< 本轮恒为 attempted=false（无 TLS 能力）
    ProbeStageResult http;   ///< 本轮恒为 attempted=false
    bool portal_detected{false};

    uint64_t timestamp_ns{0};
};

struct ActiveConnectivityConfig {
    /// 授予 capability 判定所需的最少可用目标数。
    /// 单个目标的失败可能是该目标自身的问题，不足以判定整体能力。
    size_t min_eligible_targets{2};
};

/**
 * @brief 主动连通性 SLE 集合
 *
 * 四个维度分开表达：DNS/TCP 为已实现能力，HTTPS/Portal 明确标为
 * 未具备能力（UNKNOWN/NONE），而非"证据不足"。
 */
struct ActiveConnectivityResult {
    SleResult dns;
    SleResult tcp;
    SleResult https;
    SleResult portal;
};

/**
 * @brief 主动连通性评估器（纯函数，不做任何网络 I/O）
 */
class ActiveConnectivityEvaluator {
public:
    using Config = ActiveConnectivityConfig;

    static ActiveConnectivityResult evaluate(const std::vector<ProbeTargetResult>& targets,
                                             bool probe_enabled,
                                             const Config& cfg = Config()) {
        ActiveConnectivityResult out;

        // HTTPS / Portal：本轮不具备探测能力。
        // 明确表达为 NO_CAPABILITY，而不是"证据不足"（PARTIAL）——
        // 二者含义不同：前者是我们没有这个能力，后者是有能力但数据不够。
        setUnavailable(out.https, "no_tls_probe_capability");
        setUnavailable(out.portal, "no_portal_probe_capability");

        // 探测未启用：能力维度整体 UNKNOWN，绝不退回被动证据替代
        if (!probe_enabled || targets.empty()) {
            setUnavailable(out.dns,
                probe_enabled ? "no_probe_targets" : "active_probe_disabled");
            setUnavailable(out.tcp,
                probe_enabled ? "no_probe_targets" : "active_probe_disabled");
            return out;
        }

        // ---- DNS capability ----
        std::vector<const ProbeTargetResult*> dns_eligible;
        size_t dns_ok = 0;
        for (const auto& t : targets) {
            if (!t.dns.attempted) continue;
            dns_eligible.push_back(&t);
            if (t.dns.success) dns_ok++;
        }
        out.dns.source = EvidenceSource::ACTIVE_PROBE;
        out.dns.scope = EvidenceScope::NETWORK_PATH;

        if (dns_eligible.size() < cfg.min_eligible_targets) {
            out.dns.state = HealthState::UNKNOWN;
            out.dns.coverage = Coverage::NONE;
            out.dns.reason = "insufficient_probe_targets";
        } else if (dns_ok == 0) {
            // 所有受控目标的名称解析都失败 → 本机解析能力失效
            out.dns.state = HealthState::BAD;
            out.dns.coverage = Coverage::FULL_FOR_PROFILE;
            out.dns.capability_level_negative = true;
            out.dns.reason = "active_dns_capability_failed";
        } else {
            // 只要还有受控目标解析成功，就不能宣称解析能力整体失效
            out.dns.state = HealthState::GOOD;
            out.dns.coverage = Coverage::FULL_FOR_PROFILE;
            out.dns.reason = "active_dns_capability_ok";
        }
        out.dns.evidence.push_back({"probe_targets_total",
            static_cast<double>(targets.size()), "Configured probe targets"});
        out.dns.evidence.push_back({"probe_dns_success",
            static_cast<double>(dns_ok), "Targets whose name resolution succeeded"});

        // ---- TCP capability（依赖截断：仅在 DNS 成功的 target 上评估）----
        out.tcp.source = EvidenceSource::ACTIVE_PROBE;
        out.tcp.scope = EvidenceScope::NETWORK_PATH;

        std::vector<const ProbeTargetResult*> tcp_eligible;
        size_t tcp_ok = 0;
        for (const auto& t : targets) {
            if (!t.dns.attempted || !t.dns.success) continue;  // blocked_by_dns
            if (!t.tcp.attempted) continue;
            tcp_eligible.push_back(&t);
            if (t.tcp.success) tcp_ok++;
        }

        if (tcp_eligible.empty()) {
            // 没有任何 target 走到 TCP 阶段：这是上游 DNS 的后果，
            // 不得记为 TCP 失败，否则一次 DNS 故障会污染多个 SLE。
            out.tcp.state = HealthState::UNKNOWN;
            out.tcp.coverage = Coverage::NONE;
            out.tcp.reason = "blocked_by_dns";
        } else if (tcp_eligible.size() < cfg.min_eligible_targets) {
            out.tcp.state = HealthState::UNKNOWN;
            out.tcp.coverage = Coverage::NONE;
            out.tcp.reason = "insufficient_probe_targets";
        } else if (tcp_ok == 0) {
            // 多个受控目标的 TCP 建连全部失败。
            // 测的是 host-level capability（目标由我们选定且应稳定可达），
            // 因此这是强负面证据，有资格参与 INTERNET_ACCESS 判定。
            out.tcp.state = HealthState::BAD;
            out.tcp.coverage = Coverage::FULL_FOR_PROFILE;
            out.tcp.capability_level_negative = true;
            out.tcp.reason = "active_tcp_capability_failed";
        } else {
            out.tcp.state = HealthState::GOOD;
            out.tcp.coverage = Coverage::FULL_FOR_PROFILE;
            out.tcp.reason = "active_tcp_capability_ok";
        }
        out.tcp.evidence.push_back({"probe_tcp_eligible",
            static_cast<double>(tcp_eligible.size()), "Targets reaching TCP stage"});
        out.tcp.evidence.push_back({"probe_tcp_success",
            static_cast<double>(tcp_ok), "Targets with successful TCP connect"});

        return out;
    }

private:
    static void setUnavailable(SleResult& r, const char* reason) {
        r.state = HealthState::UNKNOWN;
        r.coverage = Coverage::NONE;
        r.source = EvidenceSource::UNSPECIFIED;
        r.scope = EvidenceScope::NO_CAPABILITY;
        r.capability_level_negative = false;
        r.reason = reason;
    }
};

}  // namespace weaknet

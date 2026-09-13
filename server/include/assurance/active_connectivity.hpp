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
#include <algorithm>
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

/// TLS 阶段附加事实（证书与协议层，用于根因区分）
struct TlsStageInfo {
    bool cert_verified{false};
    std::string version;        ///< 如 TLSv1.3
    std::string cipher;
    std::string peer_subject;
    std::string issuer;
};

/**
 * @brief Captive Portal 观测信号
 *
 * 只有**受控 oracle**（预期响应已知的 connectivity-check 端点）才能产生
 * 这些信号。普通业务流量里的 301/302 不构成任何门户语义。
 */
enum class PortalSignal {
    NONE = 0,           ///< 响应符合预期
    NOT_PROBED,         ///< 未执行 oracle 探测
    REDIRECTED,         ///< 被重定向到非预期主机
    CONTENT_MISMATCH,   ///< 状态码正常但内容不符合预期
};

/// HTTP 阶段附加事实（Portal oracle 判定所需）
struct HttpStageInfo {
    int status_code{0};
    std::string location;       ///< Location 响应头
    std::string server_header;
    std::string content_type;
    bool body_matches_expected{false};
    PortalSignal portal_signal{PortalSignal::NOT_PROBED};
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
    /// 该目标的故障域标识（默认取 hostname）。
    /// Portal quorum 要求信号来自**不同故障域**，同一 CDN 的多个域名
    /// 实为单点 oracle，不能凑数。
    std::string failure_domain;

    ProbeStageResult dns;
    ProbeStageResult tcp;
    ProbeStageResult tls;
    ProbeStageResult http;
    TlsStageInfo tls_info;
    HttpStageInfo http_info;
    bool portal_detected{false};

    uint64_t timestamp_ns{0};
};

struct ActiveConnectivityConfig {
    /// 授予 capability 判定所需的最少可用目标数。
    /// 单个目标的失败可能是该目标自身的问题，不足以判定整体能力。
    size_t min_eligible_targets{2};

    /// 编译期是否具备 TLS 能力（由调用方以 TlsProbeClient::available() 填入）。
    ///
    /// 必须**显式声明**而不是从 targets 推断：二者语义不同 ——
    ///   tls_available=false            → 我们没有这个能力（NO_CAPABILITY）
    ///   tls_available=true 但无数据     → 有能力，但本轮证据不足（UNKNOWN/PARTIAL）
    /// 把后者表达成前者会掩盖"探测本身没跑起来"这类真实缺陷。
    /// 默认 false 是保守取值：未声明能力时绝不宣称 HTTPS 可用。
    bool tls_available{false};

    /// 底层链路（IP/DNS/TCP）是否健康。
    /// Portal 判定要求底层健康 —— 底层故障时应把解释权交回底层 SLE，
    /// 不得把底层故障误归因门户（评价体系硬约束）。
    bool underlying_healthy{true};

    /// Captive Portal 判定所需的一致信号故障域数下限。
    size_t portal_min_domains{2};
};

/**
 * @brief 主动连通性 SLE 集合
 *
 * 四个维度分开表达。DNS/TCP/HTTPS 为 capability 语义，
 * Portal 为受控 oracle 语义。
 */
struct ActiveConnectivityResult {
    SleResult dns;
    SleResult tcp;
    SleResult https;
    SleResult portal;
    /// Portal 观测到信号但未达 quorum 时置位，供 UI 提示而不改变状态
    bool portal_suspected{false};
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

        // HTTPS / Portal：能力由配置显式声明，不从数据推断。
        // 未声明能力时明确表达为 NO_CAPABILITY，而不是"证据不足"（PARTIAL）——
        // 二者含义不同：前者是我们没有这个能力，后者是有能力但数据不够。
        if (!cfg.tls_available) {
            setUnavailable(out.https, "no_tls_probe_capability");
        }
        // Portal 能力的缺失在下方按"是否探测过 oracle"单独表达。

        // 探测未启用：能力维度整体 UNKNOWN，绝不退回被动证据替代
        if (!probe_enabled || targets.empty()) {
            setUnavailable(out.dns,
                probe_enabled ? "no_probe_targets" : "active_probe_disabled");
            setUnavailable(out.tcp,
                probe_enabled ? "no_probe_targets" : "active_probe_disabled");
            if (!cfg.tls_available || !probe_enabled || targets.empty()) {
                setUnavailable(out.https,
                    probe_enabled ? "no_probe_targets" : "active_probe_disabled");
            }
            setUnavailable(out.portal,
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

        // ---- HTTPS capability（依赖截断：仅在 TCP 成功的 target 上评估）----
        evaluateHttps(out, targets, cfg);

        // ---- Captive Portal（受控 oracle，依赖截断 + 底层健康门禁）----
        evaluatePortal(out, targets, cfg);

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

    /**
     * @brief HTTPS capability 判定
     *
     * 语义边界（与评价体系一致）：
     *   收到**任意合法 HTTP 状态码**即证明 HTTPS transport 可用。
     *   404/500 是 endpoint 自身的业务语义 —— 我们选的受控目标返回 5xx，
     *   说明该 endpoint 有问题，不能据此判定本机 HTTPS 能力失效。
     *   因此这里的 success 条件是 tls.ok && http.ok（http.ok 与状态码解耦）。
     *
     * 证书错误单独归因：全部目标都因证书校验失败时，这是强负面证据
     * （可能是中间人/劫持），reason 与"连不上"区分开。
     */
    static void evaluateHttps(ActiveConnectivityResult& out,
                              const std::vector<ProbeTargetResult>& targets,
                              const Config& cfg) {
        if (!cfg.tls_available) return;   // 已由 setUnavailable 表达

        out.https.source = EvidenceSource::ACTIVE_PROBE;
        out.https.scope = EvidenceScope::NETWORK_PATH;

        std::vector<const ProbeTargetResult*> eligible;
        size_t ok = 0, cert_fail = 0;
        for (const auto& t : targets) {
            // 依赖截断：上游未走到 TLS 阶段的不计入，避免一次 DNS/TCP
            // 故障在 HTTPS 维度再产生一次"失败"
            if (!t.dns.attempted || !t.dns.success) continue;
            if (!t.tcp.attempted || !t.tcp.success) continue;
            if (!t.tls.attempted) continue;
            eligible.push_back(&t);
            if (t.tls.success && t.http.success) ok++;
            else if (!t.tls.success && t.tls.detail == "cert_verify_failed") cert_fail++;
        }

        if (eligible.empty()) {
            out.https.state = HealthState::UNKNOWN;
            out.https.coverage = Coverage::NONE;
            out.https.reason = "blocked_by_tcp";
            return;
        }
        if (eligible.size() < cfg.min_eligible_targets) {
            out.https.state = HealthState::UNKNOWN;
            out.https.coverage = Coverage::NONE;
            out.https.reason = "insufficient_probe_targets";
            return;
        }
        if (ok == 0) {
            out.https.state = HealthState::BAD;
            out.https.coverage = Coverage::FULL_FOR_PROFILE;
            out.https.capability_level_negative = true;
            // 全部失败且全部是证书问题 → 精确归因，不混进"网络不可达"
            out.https.reason = (cert_fail == eligible.size())
                ? "active_https_cert_verification_failed"
                : "active_https_capability_failed";
        } else {
            // 只要有一个受控目标建立了 TLS 并得到合法 HTTP 响应，
            // 就证明本机 HTTPS capability 存在
            out.https.state = HealthState::GOOD;
            out.https.coverage = Coverage::FULL_FOR_PROFILE;
            out.https.reason = "active_https_capability_ok";
        }
        out.https.evidence.push_back({"probe_https_eligible",
            static_cast<double>(eligible.size()), "Targets reaching TLS stage"});
        out.https.evidence.push_back({"probe_https_success",
            static_cast<double>(ok), "Targets with TLS + valid HTTP response"});
        out.https.evidence.push_back({"probe_https_cert_fail",
            static_cast<double>(cert_fail), "Targets failing certificate verification"});
    }

    /**
     * @brief Captive Portal 判定（受控 oracle）
     *
     * 判定纪律：
     *   1. 底层链路不健康时不产生门户结论 —— 把解释权交回底层 SLE，
     *      避免把"网断了"误归因成"被门户拦截"。
     *   2. 单个端点的异常只置 portal_suspected，不判 CAPTIVE_PORTAL。
     *      一个 connectivity-check 端点挂掉是常见运维事件，不是门户。
     *   3. 达到 quorum 的门槛是**故障域**而非目标条数：同一 CDN 的多个
     *      域名实为单点 oracle，凑数会让判定退化成单点依赖。
     */
    static void evaluatePortal(ActiveConnectivityResult& out,
                               const std::vector<ProbeTargetResult>& targets,
                               const Config& cfg) {
        const bool any_probed = [&] {
            for (const auto& t : targets) {
                if (t.http_info.portal_signal != PortalSignal::NOT_PROBED) return true;
            }
            return false;
        }();

        if (!any_probed) {
            setUnavailable(out.portal, "no_portal_probe_capability");
            return;
        }

        out.portal.source = EvidenceSource::ACTIVE_PROBE;
        out.portal.scope = EvidenceScope::NETWORK_PATH;

        // 底层不健康：不解释为门户。UNKNOWN 而非 GOOD，
        // 因为此时我们也无法确认网络是干净的。
        if (!cfg.underlying_healthy) {
            out.portal.state = HealthState::UNKNOWN;
            out.portal.coverage = Coverage::PARTIAL;
            out.portal.reason = "portal_signal_but_underlying_unhealthy";
            return;
        }

        size_t probed = 0, positive = 0, expected = 0;
        std::vector<std::string> signal_domains;
        for (const auto& t : targets) {
            const auto sig = t.http_info.portal_signal;
            if (sig == PortalSignal::NOT_PROBED) continue;
            probed++;
            if (sig == PortalSignal::REDIRECTED || sig == PortalSignal::CONTENT_MISMATCH) {
                positive++;
                const std::string dom =
                    t.failure_domain.empty() ? t.hostname : t.failure_domain;
                if (std::find(signal_domains.begin(), signal_domains.end(), dom)
                    == signal_domains.end()) {
                    signal_domains.push_back(dom);
                }
            } else if (sig == PortalSignal::NONE) {
                expected++;
            }
        }

        if (probed < cfg.min_eligible_targets) {
            out.portal.state = HealthState::UNKNOWN;
            out.portal.coverage = Coverage::NONE;
            out.portal.reason = "insufficient_portal_probe_targets";
            return;
        }

        // quorum：≥2 个**独立故障域**给出一致信号
        const bool quorum = signal_domains.size() >= cfg.portal_min_domains;
        if (quorum) {
            out.portal.state = HealthState::BAD;
            out.portal.coverage = Coverage::FULL_FOR_PROFILE;
            out.portal.capability_level_negative = true;
            out.portal.reason = "captive_portal_detected";
        } else if (positive > 0) {
            // 单端点异常：提示但不改变状态
            out.portal_suspected = true;
            out.portal.state = HealthState::UNKNOWN;
            out.portal.coverage = Coverage::PARTIAL;
            out.portal.reason = "portal_suspected_single_endpoint";
        } else if (expected > 0) {
            out.portal.state = HealthState::GOOD;
            out.portal.coverage = Coverage::FULL_FOR_PROFILE;
            out.portal.reason = "no_captive_portal";
        } else {
            out.portal.state = HealthState::UNKNOWN;
            out.portal.coverage = Coverage::NONE;
            out.portal.reason = "portal_signal_inconclusive";
        }

        out.portal.evidence.push_back({"probe_portal_signal_domains",
            static_cast<double>(signal_domains.size()),
            "Distinct failure domains reporting a consistent portal signal"});
        out.portal.evidence.push_back({"probe_portal_positive",
            static_cast<double>(positive), "Targets reporting a portal signal"});
    }
};

}  // namespace weaknet

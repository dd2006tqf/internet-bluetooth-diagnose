#pragma once

/**
 * @file http_access_evaluator.hpp
 * @brief Passive Cleartext HTTP Experience SLE — 无状态纯函数评估
 *
 * **命名与 scope 说明（重要）**：
 *   本 SLE 只能观测**明文 HTTP**（capture 靠方法前缀 "GET "/"POST " 识别，
 *   TLS 加密后无法匹配）。因此它**不是** "HTTP/HTTPS Access"，
 *   不能代表完整的 Internet 应用层可用性。
 *   其结果是 non-blocking 的"观测到的业务体验"，无权单独判定 Internet 可用性。
 *   scope = CLEARTEXT_PER_DESTINATION
 *
 * 回答的问题（观测语义）：在**明文 HTTP** 流量中，应用层是否得到了有效响应？
 *
 * 语义边界（关键，与业务语义严格分离）：
 *   本 SLE 判断的是"HTTP 服务链路是否工作"，不是"网页内容是否符合用户期望"。
 *   - 404 Not Found / 403 Forbidden / 其他 4xx：
 *     服务端**正常应答**了，链路是通的 → 属传输健康，不判 BAD
 *     （就像 DNS 的 NXDOMAIN 属事务成功）
 *   - 5xx Server Error：服务端明确报告故障 → 直接负面证据
 *   - 3xx 重定向：可能只是正常跳转；若重定向到认证门户，由 Captive Portal
 *     维度单独判定，不在此处归因
 *   - 无响应 / TLS 握手失败 / 超时：缺席推导证据，受 Evidence Quality 约束
 *
 * Coverage 与 Evidence Quality 与 DNS / TCP SLE 遵循同一纪律：
 *   - 直接坏证据可在 PARTIAL coverage 下成立
 *   - 证据不足 → UNKNOWN，绝不伪造 GOOD
 *   - 观测不可靠 → UNKNOWN，不嫁祸业务
 */

#include "assurance/health_state.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace weaknet {

/// 单次 HTTP 事务观测（与 HttpTxnInfo 语义对齐，避免跨层耦合）
struct HttpAccessSample {
    uint16_t status_code{0};   ///< 0 表示未取得有效响应
    double   ttfb_ms{0.0};     ///< 首字节延迟
    bool     tls_failed{false};///< TLS 握手是否失败
};

struct HttpAccessEvaluatorConfig {
    double server_error_ratio_degraded{0.05};   ///< 5xx 占比 ≥5% → DEGRADED
    double server_error_ratio_bad{0.20};        ///< ≥20% → BAD
    double no_response_ratio_degraded{0.10};    ///< 无有效响应占比 ≥10% → DEGRADED
    double no_response_ratio_bad{0.30};         ///< ≥30% → BAD
    double median_ttfb_degraded_ms{1500.0};     ///< TTFB 中位数 ≥1.5s → DEGRADED
    double median_ttfb_bad_ms{5000.0};          ///< ≥5s → BAD
    uint64_t min_samples{5};                    ///< 最小样本门禁
};

class HttpAccessEvaluator {
public:
    using Config = HttpAccessEvaluatorConfig;

    struct Input {
        std::vector<HttpAccessSample> samples;
        uint64_t capture_events{0};   ///< 内核上报事件数（用于观测质量）
        uint64_t capture_lost{0};     ///< perf 投递丢失
    };

    static SleResult evaluate(const Input& in, const Config& cfg = Config()) {
        SleResult res;
        res.applicability = Applicability::APPLICABLE;
        // 证据来自真实业务 HTTP 流量，且**仅覆盖明文 HTTP**
        // （TLS 加密后无法识别方法与状态码）。
        // 不得对外宣称覆盖 HTTPS；这是 non-blocking 的 observed service。
        res.source = EvidenceSource::PASSIVE_REAL_TRAFFIC;
        res.scope = EvidenceScope::CLEARTEXT_PER_DESTINATION;

        // 1. 样本门禁
        if (in.samples.size() < cfg.min_samples) {
            res.state = HealthState::UNKNOWN;
            res.coverage = in.samples.empty() ? Coverage::NONE : Coverage::PARTIAL;
            res.reason = in.samples.empty() ? "no_http_observations"
                                            : "insufficient_http_samples";
            return res;
        }

        const uint64_t total = in.samples.size();
        uint64_t server_error = 0;   // 5xx：服务端明确故障（直接负面证据）
        uint64_t no_response = 0;    // 无有效响应 / TLS 失败（缺席推导）
        std::vector<double> ttfb;
        ttfb.reserve(total);

        for (const auto& s : in.samples) {
            if (s.tls_failed || s.status_code == 0) {
                no_response++;      // 链路层面没拿到响应
                continue;
            }
            if (s.status_code >= 500) {
                server_error++;     // 真实观察到的服务端错误
                continue;
            }
            // 1xx/2xx/3xx/4xx：服务端正常应答了，链路可用
            // （4xx 是业务语义，不是传输故障；3xx 由 Captive Portal 维度处理）
            ttfb.push_back(s.ttfb_ms);
        }

        const double server_error_ratio = static_cast<double>(server_error) / static_cast<double>(total);
        const double no_response_ratio = static_cast<double>(no_response) / static_cast<double>(total);
        const bool has_direct_negative = (server_error > 0);

        res.evidence.push_back({"server_error_ratio", server_error_ratio * 100.0, "HTTP 5xx ratio %"});
        res.evidence.push_back({"no_response_ratio", no_response_ratio * 100.0, "HTTP no-response ratio %"});

        // 2. Observer 质量（perf 投递丢失）
        std::string observer_reason;
        if (in.capture_events >= 20) {
            const double loss = static_cast<double>(in.capture_lost) /
                static_cast<double>(in.capture_events + in.capture_lost);
            if (loss > 0.02) observer_reason = "observer_unreliable_event_loss";
        }
        if (!observer_reason.empty()) {
            res.evidence.push_back({"capture_loss_ratio",
                static_cast<double>(in.capture_lost) /
                static_cast<double>(in.capture_events + in.capture_lost) * 100.0,
                "Perf delivery loss %"});
            // 观测不可靠且无直接坏证据 → 不知道，绝不说好
            if (!has_direct_negative) {
                res.state = HealthState::UNKNOWN;
                res.coverage = Coverage::PARTIAL;
                res.reason = observer_reason;
                return res;
            }
        }
        res.coverage = observer_reason.empty() ? Coverage::FULL_FOR_PROFILE : Coverage::PARTIAL;

        // 3. TTFB 中位数（仅在成功响应上计算）
        double median_ttfb = 0.0;
        if (!ttfb.empty()) {
            std::sort(ttfb.begin(), ttfb.end());
            const size_t mid = ttfb.size() / 2;
            median_ttfb = (ttfb.size() % 2 == 0) ? (ttfb[mid - 1] + ttfb[mid]) / 2.0
                                                 : ttfb[mid];
            res.evidence.push_back({"median_ttfb_ms", median_ttfb, "Median TTFB"});
        }

        // 4. 状态阶梯（负面证据优先）
        if (server_error_ratio >= cfg.server_error_ratio_bad ||
            no_response_ratio >= cfg.no_response_ratio_bad) {
            res.state = HealthState::BAD;
            res.reason = (server_error_ratio >= cfg.server_error_ratio_bad)
                ? "high_http_server_error_rate" : "high_http_no_response_rate";
            return res;
        }
        if (median_ttfb >= cfg.median_ttfb_bad_ms) {
            res.state = HealthState::BAD;
            res.reason = "excessive_http_latency";
            return res;
        }
        if (server_error_ratio >= cfg.server_error_ratio_degraded ||
            no_response_ratio >= cfg.no_response_ratio_degraded) {
            res.state = HealthState::DEGRADED;
            res.reason = (server_error_ratio >= cfg.server_error_ratio_degraded)
                ? "elevated_http_server_error_rate" : "elevated_http_no_response_rate";
            return res;
        }
        if (median_ttfb >= cfg.median_ttfb_degraded_ms) {
            res.state = HealthState::DEGRADED;
            res.reason = "elevated_http_latency";
            return res;
        }

        res.state = HealthState::GOOD;
        res.reason = "cleartext_http_experience_healthy";
        return res;
    }
};

}  // namespace weaknet

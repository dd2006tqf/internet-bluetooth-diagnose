#pragma once

#include "assurance/health_state.hpp"
#include "assurance/dns_types.hpp"
#include "assurance/observer_attribution.hpp"
#include <string>
#include <vector>
#include <optional>

namespace weaknet {

struct NetworkExperience {
    std::string iface;
    HealthState overall{HealthState::UNKNOWN};
    Coverage overall_coverage{Coverage::NONE};
    AssessmentProfile assessment_profile{AssessmentProfile::INTERNET_ACCESS}; // IR-3

    SleResult ip_reachability;
    SleResult responsiveness;
    SleResult reliability;
    SleResult rf_health;   // Advisory
    SleResult dns_service;   // Phase 2A: DNS Service SLE
    SleResult tcp_connect;    // Stage 2: TCP Connect SLE
    SleResult tcp_rtt;        // Passive TCP RTT (trace_tcp_probe srtt) SLE
    SleResult http_access;    // Stage 2: HTTP/HTTPS Access SLE
    SleResult captive_portal; // Stage 2: Captive Portal SLE

    // 受控主动探测（host-level Internet capability）——唯一有资格
    // 判定 INTERNET_ACCESS 的服务层证据。与被动 observed experience 分开表达。
    SleResult active_dns;
    SleResult active_tcp;
    SleResult active_https;
    SleResult active_portal;

    std::vector<std::string> warnings;
    std::optional<std::string> primary_issue;
    int display_score{50}; // 仅供展示与兼容映射

    /**
     * @brief 观测者降级归因（HOST 域证据的消费者）
     *
     * 当某个 SLE 报 observer 降级时，这里记录"为什么会降级"：
     *   SELF_INFLICTED      → 采集端 CPU 过载，证据丢失很可能是自己造成的
     *   NOT_HOST_CPU_BOUND  → CPU 正常，排除了过载这一个成因
     *   UNATTRIBUTED        → 没有 CPU 数据，老老实实说不知道
     *   NONE                → 本轮没有任何 SLE 报 observer 降级
     *
     * 重要边界：本字段**不参与** OverallPolicy 的评分与 veto。
     * 过载是本机运行状态，不是被评估链路的健康度。它的作用是让排障
     * 能区分"网络真的坏了"和"板子自己忙不过来"，而不是改变结论。
     *
     * 默认 NONE：在没有任何 observer 降级时，与引入该字段前行为完全一致。
     */
    ObserverAttribution observer_attribution{ObserverAttribution::NONE};

    /// 归因的人类可读解释（NONE 时为空）
    std::string observer_attribution_detail;
};

} // namespace weaknet

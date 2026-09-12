#pragma once

#include <string>
#include <vector>

namespace weaknet {

enum class HealthState {
    GOOD,
    DEGRADED,
    BAD,
    UNKNOWN
};

inline const char* healthStateToString(HealthState s) {
    switch (s) {
        case HealthState::GOOD: return "GOOD";
        case HealthState::DEGRADED: return "DEGRADED";
        case HealthState::BAD: return "BAD";
        case HealthState::UNKNOWN: return "UNKNOWN";
        default: return "UNKNOWN";
    }
}

enum class Coverage {
    FULL_FOR_PROFILE,
    PARTIAL,
    NONE
};

inline const char* coverageToString(Coverage c) {
    switch (c) {
        case Coverage::FULL_FOR_PROFILE: return "FULL_FOR_PROFILE";
        case Coverage::PARTIAL: return "PARTIAL";
        case Coverage::NONE: return "NONE";
        default: return "NONE";
    }
}

/// SLE 对当前链路形态/profile 的适用性。NOT_APPLICABLE 表示该维度在此环境下不存在
/// （如有线链路的 RF Health），不参与 OverallPolicy vote、coverage 惩罚、warning 与 primary_issue。
enum class Applicability {
    APPLICABLE,
    NOT_APPLICABLE
};

inline const char* applicabilityToString(Applicability a) {
    switch (a) {
        case Applicability::APPLICABLE: return "APPLICABLE";
        case Applicability::NOT_APPLICABLE: return "NOT_APPLICABLE";
        default: return "APPLICABLE";
    }
}

struct EvidenceItem {
    std::string metric;
    double value{0.0};
    std::string detail;
};

/**
 * @brief 证据来源。
 *
 * 决定该 SLE 是否有资格影响 INTERNET_ACCESS 的判定：真实业务观察只能描述
 * "观察到了什么"；受控探测才能回答"当前是否具备上网能力"。
 * 二者不可互相冒充。
 */
enum class EvidenceSource {
    UNSPECIFIED,            ///< 未声明（默认；不应长期保持）
    PASSIVE_REAL_TRAFFIC,   ///< 真实业务流量观察（目标不可控）
    ACTIVE_PROBE,           ///< 受控探测（目标由我们选定）
    SYSTEM_TELEMETRY        ///< 系统/内核遥测（如 netlink、nl80211）
};

inline const char* evidenceSourceToString(EvidenceSource s) {
    switch (s) {
        case EvidenceSource::PASSIVE_REAL_TRAFFIC: return "PASSIVE_REAL_TRAFFIC";
        case EvidenceSource::ACTIVE_PROBE: return "ACTIVE_PROBE";
        case EvidenceSource::SYSTEM_TELEMETRY: return "SYSTEM_TELEMETRY";
        case EvidenceSource::UNSPECIFIED:
        default: return "UNSPECIFIED";
    }
}

/**
 * @brief 证据覆盖范围（scope）。
 *
 * 让消费者知道结论"覆盖到哪"，避免把局部观测误读为全面结论
 * （例如仅覆盖明文 HTTP 却被当作"HTTP/HTTPS 正常"）。
 */
enum class EvidenceScope {
    UNSPECIFIED,
    NETWORK_PATH,               ///< IP 路径可达性
    LINK,                       ///< 链路/射频层
    HOST_RESOLVER_CAPABILITY,   ///< 本机名字解析能力（host-level）
    PER_DESTINATION,            ///< 单个目的地的连接结果
    CLEARTEXT_PER_DESTINATION,  ///< 仅明文 HTTP 的单个目的地结果
    NO_CAPABILITY               ///< 当前无该维度的观测能力
};

inline const char* evidenceScopeToString(EvidenceScope s) {
    switch (s) {
        case EvidenceScope::NETWORK_PATH: return "NETWORK_PATH";
        case EvidenceScope::LINK: return "LINK";
        case EvidenceScope::HOST_RESOLVER_CAPABILITY: return "HOST_RESOLVER_CAPABILITY";
        case EvidenceScope::PER_DESTINATION: return "PER_DESTINATION";
        case EvidenceScope::CLEARTEXT_PER_DESTINATION: return "CLEARTEXT_PER_DESTINATION";
        case EvidenceScope::NO_CAPABILITY: return "NO_CAPABILITY";
        case EvidenceScope::UNSPECIFIED:
        default: return "UNSPECIFIED";
    }
}

struct SleResult {
    HealthState state{HealthState::UNKNOWN};
    Coverage coverage{Coverage::NONE};
    std::vector<EvidenceItem> evidence;
    std::string reason;
    // 默认 APPLICABLE：未显式声明不适用的 SLE 一律按参与决策处理（安全默认）。
    Applicability applicability{Applicability::APPLICABLE};

    // --- 证据元数据（追加在尾部，保持既有聚合初始化兼容）---
    EvidenceSource source{EvidenceSource::UNSPECIFIED};
    EvidenceScope scope{EvidenceScope::UNSPECIFIED};

    /**
     * @brief 该负面证据是否达到"本机能力级"。
     *
     * 这是判定能否 hard-veto INTERNET_ACCESS 的依据：
     *   true  —— 本机某项能力失效（如名字解析、IP 路径），上网体验确实受损
     *   false —— 单目的地/单域名的结果，可能只是对方的问题，无权否决整体
     * 只有 capability 级负面证据才具备否决权，避免"某个目标连不上"
     * 被当作"Internet 不可用"。
     */
    bool capability_level_negative{false};
};

} // namespace weaknet

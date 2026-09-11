#pragma once

#include <cstdint>
#include <string>
#include <optional>
#include <chrono>
#include <vector>

namespace weaknet {

enum class AddressFamily : uint8_t {
    IPv4,
    IPv6
};

enum class FingerprintQuality : uint8_t {
    ENRICHED, // 全部配置字段可用 (client/resolver ip, port, txid, qname_hash, qtype)
    PARTIAL   // 仅基础 4 元组可用
};

enum class DnsTransactionState : uint8_t {
    ACTIVE,
    NOERROR,
    NXDOMAIN,
    SERVFAIL,
    REFUSED,
    RESPONSE_TRUNCATED, // SR-14: TC=1 (互斥分类，高于 RCODE)
    RESPONSE_OTHER,     // malformed 或其他 RCODE
    TIMEOUT_EXPIRED,    // 5s 未收到响应超时
    LATE_RESPONSE,      // 迟到响应（已超时进入 Tombstone 后到达）
    TRACKING_AMBIGUOUS  // TxID 冲突或 partial 无法唯一匹配
};

inline const char* dnsTransactionStateToString(DnsTransactionState s) {
    switch (s) {
        case DnsTransactionState::ACTIVE: return "ACTIVE";
        case DnsTransactionState::NOERROR: return "NOERROR";
        case DnsTransactionState::NXDOMAIN: return "NXDOMAIN";
        case DnsTransactionState::SERVFAIL: return "SERVFAIL";
        case DnsTransactionState::REFUSED: return "REFUSED";
        case DnsTransactionState::RESPONSE_TRUNCATED: return "RESPONSE_TRUNCATED";
        case DnsTransactionState::RESPONSE_OTHER: return "RESPONSE_OTHER";
        case DnsTransactionState::TIMEOUT_EXPIRED: return "TIMEOUT_EXPIRED";
        case DnsTransactionState::LATE_RESPONSE: return "LATE_RESPONSE";
        case DnsTransactionState::TRACKING_AMBIGUOUS: return "TRACKING_AMBIGUOUS";
        default: return "UNKNOWN";
    }
}

/**
 * @brief 归一化 Canonical Key (SR-7)
 * 统一以 Client 视角归一化：
 * Query:  saddr -> client, daddr -> resolver, sport -> client_port
 * Response: saddr -> resolver, daddr -> client, dport -> client_port
 */
struct DnsCanonicalKey {
    AddressFamily family{AddressFamily::IPv4};
    uint32_t client_ip{0};   // 网络序或主机序，需两端统一（以主机序存储）
    uint32_t resolver_ip{0};
    uint16_t client_port{0};
    uint16_t txid{0};
    std::optional<uint64_t> qname_hash{std::nullopt}; // tracker-only ephemeral
    std::optional<uint16_t> qtype{std::nullopt};

    bool operator==(const DnsCanonicalKey& o) const {
        return family == o.family &&
               client_ip == o.client_ip &&
               resolver_ip == o.resolver_ip &&
               client_port == o.client_port &&
               txid == o.txid &&
               qname_hash == o.qname_hash &&
               qtype == o.qtype;
    }
};

struct DnsCanonicalKeyHash {
    size_t operator()(const DnsCanonicalKey& k) const noexcept {
        size_t h = static_cast<size_t>(k.family);
        h ^= std::hash<uint32_t>{}(k.client_ip) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>{}(k.resolver_ip) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= (static_cast<size_t>(k.client_port) << 16) | static_cast<size_t>(k.txid);
        if (k.qname_hash) {
            h ^= std::hash<uint64_t>{}(*k.qname_hash) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        if (k.qtype) {
            h ^= std::hash<uint16_t>{}(*k.qtype) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        return h;
    }
};

/**
 * @brief 单事务生命周期记录 (IR-2, SR-7)
 */
struct DnsTransactionRecord {
    DnsCanonicalKey key;
    FingerprintQuality fingerprint_quality{FingerprintQuality::ENRICHED};
    uint64_t transaction_generation{0};
    uint64_t binding_epoch{0};    // 发起时绑定的网络/resolver epoch (SR-4)
    std::chrono::steady_clock::time_point first_sent_at; // 重传保持
    std::chrono::steady_clock::time_point last_sent_at;  // 重传更新
    std::chrono::steady_clock::time_point completed_at;  // 终态时间
    uint32_t attempt_count{1};
    DnsTransactionState state{DnsTransactionState::ACTIVE};
    uint8_t rcode{0};
    bool tc{false};
    double latency_ms{0.0};
};

/**
 * @brief 一致性快照 (修正 #3)
 * Tracker 一次性提供，避免多次调用之间并发修改造成不一致。
 */
struct DnsTrackerSnapshot {
    uint64_t binding_epoch{0};
    size_t   current_inflight{0};     // 仅统计 snapshot.binding_epoch 下处于 ACTIVE 的事务
    uint64_t revision{0};
    std::chrono::steady_clock::time_point cutoff{std::chrono::steady_clock::now()};
};

/**
 * @brief 终端事务聚合统计窗口 (SR-1, SR-8, SR-10, SR-14, 修正 #2)
 */
struct DnsMetricWindow {
    uint64_t queries_started{0};
    uint64_t responses_noerror{0};
    uint64_t responses_nxdomain{0};
    uint64_t responses_servfail{0};
    uint64_t responses_refused{0};
    uint64_t responses_truncated{0}; // SR-14: TC=1
    uint64_t responses_other{0};
    uint64_t timeouts{0};

    std::vector<double> latencies_ms; // 成功响应事务的端到端时延（用于计算中位数）

    uint64_t unmatched{0};               // SR-8: 无对应请求且非墓碑的响应
    uint64_t tracking_ambiguous{0};      // SR-8: 存在歧义的匹配
    uint64_t late_responses{0};          // 迟到响应（已超时进入墓碑）
    uint64_t tracker_insert_failures{0}; // IR-1: 容量溢出拒绝
    uint64_t event_delivery_loss{0};     // 修正 #2: eBPF transport 丢失

    uint64_t response_match_attempts{0}; // 响应匹配尝试总数
    uint64_t query_capture_attempts{0};  // 查询捕获尝试总数
    uint64_t total_captured_events{0};   // 捕获事件总数

    size_t   current_inflight{0};        // SR-13: Tracker 实时提供
    uint64_t binding_epoch{0};
    std::chrono::steady_clock::time_point evaluation_cutoff;
    uint64_t evidence_revision{0};

    // 衍生语义计算
    uint64_t knownSuccess() const {
        return responses_noerror + responses_nxdomain;
    }

    uint64_t knownFailure() const {
        return responses_servfail + responses_refused + timeouts;
    }

    uint64_t evaluableTerminals() const {
        return knownSuccess() + knownFailure();
    }

    double failureRatio() const {
        uint64_t eval = evaluableTerminals();
        if (eval == 0) return 0.0;
        return static_cast<double>(knownFailure()) / static_cast<double>(eval);
    }

    double ambiguityRatio() const {
        if (response_match_attempts == 0) return 0.0;
        return static_cast<double>(tracking_ambiguous) / static_cast<double>(response_match_attempts);
    }

    double insertFailureRatio() const {
        if (query_capture_attempts == 0) return 0.0;
        return static_cast<double>(tracker_insert_failures) / static_cast<double>(query_capture_attempts);
    }

    double eventDeliveryLossRatio() const {
        if (total_captured_events == 0) return 0.0;
        return static_cast<double>(event_delivery_loss) / static_cast<double>(total_captured_events);
    }

    double classificationCoverage() const {
        uint64_t total = evaluableTerminals() + responses_other + responses_truncated;
        if (total == 0) return 1.0;
        return static_cast<double>(evaluableTerminals()) / static_cast<double>(total);
    }
};

/**
 * @brief Assessment Profile (IR-3)
 */
enum class AssessmentProfile : uint8_t {
    NETWORK_ONLY,     // 局域网/无外网环境（DNS 故障仅作为 Advisory 告警，不杀死整体评级）
    INTERNET_ACCESS   // 互联网访问形态（DNS 故障一票否决至 BAD）
};

inline const char* assessmentProfileToString(AssessmentProfile p) {
    switch (p) {
        case AssessmentProfile::NETWORK_ONLY: return "NETWORK_ONLY";
        case AssessmentProfile::INTERNET_ACCESS: return "INTERNET_ACCESS";
        default: return "INTERNET_ACCESS";
    }
}

inline AssessmentProfile parseAssessmentProfile(const std::string& s) {
    if (s == "NETWORK_ONLY") return AssessmentProfile::NETWORK_ONLY;
    return AssessmentProfile::INTERNET_ACCESS;
}

} // namespace weaknet

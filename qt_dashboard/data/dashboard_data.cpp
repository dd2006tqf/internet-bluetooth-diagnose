/**
 * @file dashboard_data.cpp
 * @brief POD 解析实现
 *
 * 每个 parse_xxx 都是纯函数：输入 JSON 字符串，输出 POD，不碰全局状态。
 * 这样可以在没有显示器、没有 D-Bus、没有服务端的情况下单元测试。
 */

#include "dashboard_data.hpp"
#include "json_reader.hpp"

#include <algorithm>

namespace weaknet {
namespace dashboard {

namespace {

/**
 * @brief 从 JSON 的某个对象路径下解析一个 SLE 维度
 *
 * @param reader 已构造的 JsonReader
 * @param base   形如 "network_health.ip_reachability"
 * @param key    维度标识，写回 SleItem::key
 */
SleItem parse_sle(const JsonReader& reader, const std::string& base,
                  const std::string& key) {
    SleItem item;
    item.key = key;
    item.state = reader.getString(base + ".state", "UNKNOWN");
    item.coverage = reader.getString(base + ".coverage", "NONE");
    item.applicability = reader.getString(base + ".applicability", "NOT_APPLICABLE");
    item.reason = reader.getString(base + ".reason");
    return item;
}

/**
 * @brief 判断 JSON 是否为数组（首个非空白字符是 '['）
 */
bool starts_with_array(const std::string& json) {
    for (char c : json) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        return c == '[';
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// ExperienceSnapshot
// ---------------------------------------------------------------------------

void ExperienceSnapshot::count_by_state(int* healthy, int* degraded,
                                        int* failed, int* unknown) const {
    int h = 0, d = 0, f = 0, u = 0;
    const SleItem* items[] = {
        &ip_reachability, &responsiveness, &reliability, &rf_health,
        &dns_service, &tcp_connect, &tcp_rtt, &http_access, &captive_portal,
        &active_dns, &active_tcp, &active_https, &active_portal,
    };
    // 状态字符串来自 healthStateToString()：GOOD / DEGRADED / BAD / UNKNOWN
    for (const SleItem* item : items) {
        if (!item->has_value()) {
            ++u;
        } else if (item->state == "GOOD") {
            ++h;
        } else if (item->state == "DEGRADED") {
            ++d;
        } else if (item->state == "BAD") {
            ++f;
        } else {
            ++u;
        }
    }
    if (healthy) *healthy = h;
    if (degraded) *degraded = d;
    if (failed) *failed = f;
    if (unknown) *unknown = u;
}

ExperienceSnapshot parse_experience(const std::string& json) {
    ExperienceSnapshot snap;
    JsonReader reader(json);
    if (!reader.valid()) {
        snap.unavailable_reason = "invalid_json";
        return snap;
    }

    snap.schema_version = reader.getInt("schema_version", 0);

    // 先判断是否是「无快照 / 过期」的降级形状：
    // {"schema_version":2,"state":"UNKNOWN","coverage":"NONE","reason":"..."}
    // 这种形状顶层没有 overall 对象。
    const bool has_overall = reader.has("overall.state");
    if (!has_overall) {
        snap.overall_state = reader.getString("state", "UNKNOWN");
        snap.overall_coverage = reader.getString("coverage", "NONE");
        snap.unavailable_reason = reader.getString("reason", "unknown");
        return snap;
    }

    snap.interface_name = reader.getString("interface");
    snap.assessment_profile = reader.getString("assessment_profile");
    snap.overall_state = reader.getString("overall.state", "UNKNOWN");
    snap.overall_coverage = reader.getString("overall.coverage", "NONE");
    snap.display_score = reader.getDouble("overall.display_score", 0.0);

    snap.ip_reachability =
        parse_sle(reader, "network_health.ip_reachability", "ip_reachability");
    snap.responsiveness =
        parse_sle(reader, "network_health.responsiveness", "responsiveness");
    snap.reliability =
        parse_sle(reader, "network_health.reliability", "reliability");
    snap.rf_health = parse_sle(reader, "network_health.rf_health", "rf_health");

    snap.dns_service = parse_sle(reader, "service_health.dns", "dns");
    snap.tcp_connect = parse_sle(reader, "service_health.tcp_connect", "tcp_connect");
    snap.tcp_rtt = parse_sle(reader, "service_health.tcp_rtt", "tcp_rtt");
    snap.http_access = parse_sle(reader, "service_health.http_access", "http_access");
    snap.captive_portal =
        parse_sle(reader, "service_health.captive_portal", "captive_portal");
    snap.active_dns = parse_sle(reader, "service_health.active_dns", "active_dns");
    snap.active_tcp = parse_sle(reader, "service_health.active_tcp", "active_tcp");
    snap.active_https =
        parse_sle(reader, "service_health.active_https", "active_https");
    snap.active_portal =
        parse_sle(reader, "service_health.active_portal", "active_portal");

    snap.warnings = reader.getStringArray("warnings");
    snap.primary_issue = reader.getString("primary_issue");

    return snap;
}

// ---------------------------------------------------------------------------
// DiagnosisSnapshot
// ---------------------------------------------------------------------------

DiagnosisSnapshot parse_diagnosis(const std::string& json) {
    DiagnosisSnapshot snap;
    snap.raw_json = json;

    JsonReader reader(json);
    if (!reader.valid()) {
        return snap;
    }

    snap.valid = true;
    snap.fault_domain = reader.getString("fault_domain", "UNKNOWN");
    snap.primary_issue = reader.getString("primary_issue", "no_issue_detected");
    // confidence 是字符串枚举 HIGH / MEDIUM / LOW，不是数值
    snap.confidence = reader.getString("confidence", "LOW");
    snap.secondary_issues = reader.getStringArray("secondary_issues");
    snap.evidence_refs = reader.getStringArray("evidence_refs");
    snap.summary_template = reader.getString("default_summary_template");

    for (const std::string& raw : reader.getObjectArray("excluded_causes")) {
        JsonReader item(raw);
        ExcludedCause cause;
        cause.cause = item.getString("cause");
        cause.reason = item.getString("reason");
        if (!cause.cause.empty()) {
            snap.excluded_causes.push_back(std::move(cause));
        }
    }

    for (const std::string& raw : reader.getObjectArray("actions")) {
        JsonReader item(raw);
        ActionSuggestion action;
        action.priority = item.getInt("priority", 1);
        action.action_id = item.getString("action_id");
        if (!action.action_id.empty()) {
            snap.actions.push_back(std::move(action));
        }
    }

    return snap;
}

// ---------------------------------------------------------------------------
// HistorySeries
// ---------------------------------------------------------------------------

HistorySeries parse_history(const std::string& json) {
    HistorySeries series;
    JsonReader reader(json);
    if (!reader.valid()) {
        return series;
    }

    // 数据库不可用时服务端返回 {"error":"database not available"}（对象）
    if (!starts_with_array(json)) {
        return series;
    }

    std::vector<HistoryPoint> points;
    for (const std::string& raw : reader.getRootObjectArray()) {
        JsonReader item(raw);
        HistoryPoint point;
        point.ts = item.getString("ts");
        point.score = item.getDouble("score", 0.0);
        point.overall_quality = item.getString("overall_quality");
        point.iface = item.getString("iface");
        point.legacy = item.getBool("legacy", false);
        points.push_back(std::move(point));
    }

    // 服务端按 ts DESC 返回（最新在前），翻转为时间正序便于画图
    std::reverse(points.begin(), points.end());
    series.points = std::move(points);
    if (!series.points.empty()) {
        series.interface_name = series.points.front().iface;
    }
    return series;
}

} // namespace dashboard
} // namespace weaknet

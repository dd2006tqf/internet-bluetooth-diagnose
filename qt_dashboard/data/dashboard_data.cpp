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
#include <ctime>
#include <iomanip>
#include <sstream>

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

    // schema_version 是本层唯一能判断「契约是否还是我以为的那个」的依据。
    // 2 是当前服务端版本；0 表示字段缺失（老样本/降级形状兼容）。
    // 其余版本说明服务端契约已经变了，继续按 v2 字段名取值只会静默取空，
    // 所以直接标成不可用，让界面显示「等待评估」而不是一堆 UNKNOWN。
    if (snap.schema_version != 0 && snap.schema_version != 2) {
        snap.unavailable_reason =
            "unsupported_schema_version_" + std::to_string(snap.schema_version);
        return snap;
    }

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

bool is_history_metric_absent(HistoryMetric metric, double value) {
    switch (metric) {
        case HistoryMetric::Score:
            // 分数没有「未测量」语义：服务端总会落一个分数，
            // 0 分就是「极差」，不能当成缺数据。
            return false;
        case HistoryMetric::Rtt:
            return value <= kNoRttMs;
        case HistoryMetric::Jitter:
            return value <= kNoJitterMs;
        case HistoryMetric::Rssi:
            return value <= kNoRssiDbm;
        case HistoryMetric::TcpLoss:
            return value <= kNoTcpLossPct;
    }
    return true;
}

double HistoryPoint::value(HistoryMetric metric) const {
    switch (metric) {
        case HistoryMetric::Score:
            return score;
        case HistoryMetric::Rtt:
            return rtt_ms;
        case HistoryMetric::Jitter:
            return jitter_ms;
        case HistoryMetric::Rssi:
            return rssi_dbm;
        case HistoryMetric::TcpLoss:
            return tcp_loss;
    }
    return 0.0;
}

int HistorySeries::count_with(HistoryMetric metric) const {
    int count = 0;
    for (const HistoryPoint& p : points) {
        if (p.has(metric)) {
            ++count;
        }
    }
    return count;
}

const char* history_metric_label(HistoryMetric metric) {
    switch (metric) {
        case HistoryMetric::Score:
            return "综合分";
        case HistoryMetric::Rtt:
            return "RTT";
        case HistoryMetric::Jitter:
            return "抖动";
        case HistoryMetric::Rssi:
            return "RSSI";
        case HistoryMetric::TcpLoss:
            return "TCP 丢包率";
    }
    return "未知指标";
}

const char* history_metric_unit(HistoryMetric metric) {
    switch (metric) {
        case HistoryMetric::Score:
            return "";
        case HistoryMetric::Rtt:
        case HistoryMetric::Jitter:
            return "ms";
        case HistoryMetric::Rssi:
            return "dBm";
        case HistoryMetric::TcpLoss:
            return "%";
    }
    return "";
}

HistoryRange history_metric_range(HistoryMetric metric) {
    HistoryRange range;
    switch (metric) {
        case HistoryMetric::Score:
            // 归一化量纲，按数据自适应即可
            return range;
        case HistoryMetric::Rtt:
            // 0-300ms：覆盖「良好（<50）」到「严重（>200，见
            // responsiveness_evaluator 的判定量级）」的全程，
            // 又不至于让 20ms 的日常波动看起来像断崖
            range.lo = 0.0;
            range.hi = 300.0;
            range.fixed = true;
            return range;
        case HistoryMetric::Jitter:
            // 0-150ms：抖动比 RTT 更早劣化，阈值量级也更小
            range.lo = 0.0;
            range.hi = 150.0;
            range.fixed = true;
            return range;
        case HistoryMetric::Rssi:
            // -100 到 -30 dBm：Wi-Fi 的物理下限到「贴着 AP」。
            // 真机常在 -40~-70，落在这条轴的中间偏右，波动幅度看起来是
            // 它真实的样子，不会被放大成噪声
            range.lo = -100.0;
            range.hi = -30.0;
            range.fixed = true;
            return range;
        case HistoryMetric::TcpLoss:
            // 0-10%：可靠性评估的 bad 阈值是 4%（reliability_evaluator.hpp:14），
            // 10% 的上限让它落在轴的 40% 位置，仍有余量看清更差的情况
            range.lo = 0.0;
            range.hi = 10.0;
            range.fixed = true;
            return range;
    }
    return range;
}

int history_window_seconds(HistoryWindow window) {
    switch (window) {
        case HistoryWindow::Last1h:
            return 60 * 60;
        case HistoryWindow::Last6h:
            return 6 * 60 * 60;
        case HistoryWindow::Last13h:
            // 13 小时不是产品审美选择，是服务端能力上界反推出来的：
            // kMaxHistoryLimit = 10000，落库周期 5 秒 → 最多覆盖 50000 秒
            // ≈ 13.89 小时。取 13 留约 6% 余量。
            //
            // 为什么不是 24h：需要 17280 条，超出上限 73%，服务端会静默
            // 钳制到 10000 条（板端实测：请求 7 天 / limit=20000 时返回
            // 恰好 10000 条，跨度 69.99h）。那会变成「标签写 24h、
            // 图里只有 13.9h」——代码注释里早就警告过这类谎报。
            return 13 * 60 * 60;
    }
    return 60 * 60;
}

int history_window_limit(HistoryWindow window) {
    return history_window_seconds(window) / kHistorySampleIntervalSeconds;
}

const char* history_window_label(HistoryWindow window) {
    switch (window) {
        case HistoryWindow::Last1h:
            return "1 小时";
        case HistoryWindow::Last6h:
            return "6 小时";
        case HistoryWindow::Last13h:
            return "13 小时";
    }
    return "未知窗口";
}

std::string history_window_start(HistoryWindow window,
                                 long long now_epoch_seconds) {
    const std::time_t moment =
        static_cast<std::time_t>(now_epoch_seconds -
                                 history_window_seconds(window));

    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &moment) != 0) {
        return std::string();
    }
#else
    if (localtime_r(&moment, &local) == nullptr) {
        return std::string();
    }
#endif

    // 格式必须与 `database_manager.cpp` 的 `currentTimestamp()` 逐字一致：
    // `%Y-%m-%dT%H:%M:%S`，用 localtime。理由：
    //   1. `ts` 列按 TEXT 存、按字符串比较（`ts >= ?`），格式不同会静默比较失败
    //   2. 用 UTC 而数据库存的是本地时间，会整体偏 8 小时（本机在东八区），
    //      表现为「13h 窗口只取回 5 小时」
    std::ostringstream oss;
    oss << std::put_time(&local, "%Y-%m-%dT%H:%M:%S");
    return oss.str();
}

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
        point.legacy = item.getBool("legacy", false);

        // 服务端对未测量的指标输出 JSON null（见 database_manager.cpp 的
        // queryHistory()：`row.rtt_null ? "null" : ...`）。
        //
        // JsonReader::getDouble 对 null 返回 fallback（"null" 的首字符是 'n'，
        // 不满足数字起始字符，直接走 fallback 分支），所以把 fallback
        // 设成哨兵值，null 与「字段缺失」就自动落到同一个语义上。
        //
        // 绝不能把 fallback 写成 0.0：那会让「没测到 RTT」在图上显示成
        // 「RTT = 0ms」，是最好看的假象。
        point.rtt_ms = item.getDouble("rtt_ms", kNoRttMs);
        point.jitter_ms = item.getDouble("jitter_ms", kNoJitterMs);
        point.rssi_dbm = item.getDouble("rssi_dbm", kNoRssiDbm);
        point.tcp_loss = item.getDouble("tcp_loss", kNoTcpLossPct);

        points.push_back(std::move(point));
    }

    // 服务端按 ts DESC 返回（最新在前），翻转为时间正序便于画图
    std::reverse(points.begin(), points.end());
    series.points = std::move(points);
    return series;
}

} // namespace dashboard
} // namespace weaknet

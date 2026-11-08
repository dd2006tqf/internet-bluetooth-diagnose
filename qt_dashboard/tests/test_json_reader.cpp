/**
 * @file test_json_reader.cpp
 * @brief 数据层单元测试
 *
 * ## 测试样本的来源
 *
 * 所有 JSON 样本都**逐字取自服务端代码**，不是臆造的：
 *
 *   - 完整快照      ← `server/include/assurance/legacy_adapter.hpp:162-227`
 *   - 无快照        ← `server/src/dbus_service.cpp:916-917`
 *   - 快照过期      ← `server/src/dbus_service.cpp:919-921`
 *   - 历史数组      ← `server/src/database_manager.cpp:703-740`
 *   - 历史不可用    ← `server/src/dbus_service.cpp:1530`
 *   - 诊断事实      ← `server/include/assurance/diagnosis_engine.hpp:44-85`
 *
 * 这样做的好处：如果服务端改字段名，这些测试会失败，而不是等到
 * 板子接上显示器才发现界面空白。
 *
 * ## 为什么能脱离 Qt 和显示器运行
 *
 * 数据层不依赖 Qt，也不依赖 D-Bus。纯字符串进、纯结构体出。
 * 因此可以在 x86 上用 gtest 直接跑，不需要开发板、不需要 HDMI。
 */

#include <gtest/gtest.h>

#include <algorithm>

#include "data/dashboard_data.hpp"
#include "data/json_reader.hpp"

namespace {

using weaknet::dashboard::DiagnosisSnapshot;
using weaknet::dashboard::ExperienceSnapshot;
using weaknet::dashboard::HistoryPoint;
using weaknet::dashboard::HistorySeries;
using weaknet::dashboard::JsonReader;

// ---------------------------------------------------------------------------
// JsonReader 基础能力
// ---------------------------------------------------------------------------

TEST(JsonReaderTest, RejectsEmptyAndGarbage) {
    EXPECT_FALSE(JsonReader("").valid());
    EXPECT_FALSE(JsonReader("   ").valid());
    EXPECT_FALSE(JsonReader("null").valid());
    EXPECT_FALSE(JsonReader("\"a string\"").valid());
    EXPECT_FALSE(JsonReader("<html>500</html>").valid());
}

TEST(JsonReaderTest, AcceptsObjectAndArray) {
    EXPECT_TRUE(JsonReader("{}").valid());
    EXPECT_TRUE(JsonReader("[]").valid());
    EXPECT_TRUE(JsonReader("  {\"a\":1}  ").valid());
    EXPECT_TRUE(JsonReader("[1,2,3]").valid());
}

TEST(JsonReaderTest, ReadsTopLevelScalars) {
    const JsonReader r(R"({"i":2,"d":95.5,"b":true,"s":"wlan0","n":null})");
    EXPECT_EQ(r.getInt("i", -1), 2);
    EXPECT_DOUBLE_EQ(r.getDouble("d", 0.0), 95.5);
    EXPECT_TRUE(r.getBool("b", false));
    EXPECT_EQ(r.getString("s"), "wlan0");

    // null 视为不存在
    EXPECT_FALSE(r.has("n"));
    EXPECT_EQ(r.getString("n", "fallback"), "fallback");
}

TEST(JsonReaderTest, ReadsNestedPaths) {
    const JsonReader r(R"({"overall":{"state":"GOOD","display_score":88.5}})");
    EXPECT_EQ(r.getString("overall.state"), "GOOD");
    EXPECT_DOUBLE_EQ(r.getDouble("overall.display_score", 0.0), 88.5);
    EXPECT_FALSE(r.has("overall.missing"));
    EXPECT_FALSE(r.has("missing.state"));
}

TEST(JsonReaderTest, MissingKeyReturnsFallback) {
    const JsonReader r(R"({"a":1})");
    EXPECT_EQ(r.getString("nope", "def"), "def");
    EXPECT_EQ(r.getInt("nope", 42), 42);
    EXPECT_DOUBLE_EQ(r.getDouble("nope", 3.5), 3.5);
    EXPECT_TRUE(r.getBool("nope", true));
}

TEST(JsonReaderTest, ReadsStringArray) {
    const JsonReader r(R"({"warnings":["rssi_weak","dns_slow"]})");
    const auto warnings = r.getStringArray("warnings");
    ASSERT_EQ(warnings.size(), 2u);
    EXPECT_EQ(warnings[0], "rssi_weak");
    EXPECT_EQ(warnings[1], "dns_slow");
}

TEST(JsonReaderTest, EmptyArrayYieldsEmptyVector) {
    const JsonReader r(R"({"warnings":[]})");
    EXPECT_TRUE(r.getStringArray("warnings").empty());
}

TEST(JsonReaderTest, ReadsObjectArray) {
    const JsonReader r(R"({"evidence":[{"metric":"rtt","value":25},{"metric":"loss","value":0}]})");
    const auto items = r.getObjectArray("evidence");
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(JsonReader(items[0]).getString("metric"), "rtt");
    EXPECT_DOUBLE_EQ(JsonReader(items[1]).getDouble("value", -1), 0.0);
}

TEST(JsonReaderTest, HandlesEscapedStrings) {
    const JsonReader r(R"({"s":"a\"b\\c\nd"})");
    EXPECT_EQ(r.getString("s"), "a\"b\\c\nd");
}

TEST(JsonReaderTest, DoesNotConfuseNestedBracesInStrings) {
    // 字符串里的 } 和 , 不应干扰扫描
    const JsonReader r(R"({"a":"x},y{","b":2})");
    EXPECT_EQ(r.getString("a"), "x},y{");
    EXPECT_EQ(r.getInt("b", -1), 2);
}

TEST(JsonReaderTest, ReadsRootArray) {
    const JsonReader r(R"([{"x":1},{"x":2}])");
    const auto elems = r.getRootObjectArray();
    ASSERT_EQ(elems.size(), 2u);
    EXPECT_EQ(JsonReader(elems[1]).getInt("x", -1), 2);
}

TEST(JsonReaderTest, EmptyRootArray) {
    const JsonReader r("[]");
    EXPECT_TRUE(r.getRootObjectArray().empty());
}

// ---------------------------------------------------------------------------
// ExperienceSnapshot —— 完整快照
// ---------------------------------------------------------------------------

// 逐字取自 legacy_adapter.hpp:162-227 的 toExperienceJsonV2() 输出形状
const char* kFullSnapshot = R"({
  "schema_version":2,
  "interface":"wlan0",
  "assessment_profile":"FULL",
  "overall":{"state":"DEGRADED","coverage":"FULL_FOR_PROFILE","display_score":72.5},
  "network_health":{
    "ip_reachability":{"state":"GOOD","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"","evidence":[]},
    "responsiveness":{"state":"DEGRADED","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"rtt_high","evidence":[{"metric":"rtt_ms","value":180,"detail":"p95"}]},
    "reliability":{"state":"GOOD","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"","evidence":[]},
    "rf_health":{"state":"DEGRADED","coverage":"PARTIAL","applicability":"APPLICABLE","reason":"rssi_weak","evidence":[]}
  },
  "service_health":{
    "dns":{"state":"GOOD","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"","evidence":[]},
    "tcp_connect":{"state":"GOOD","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"","evidence":[]},
    "tcp_rtt":{"state":"DEGRADED","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"","evidence":[]},
    "http_access":{"state":"GOOD","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"","evidence":[]},
    "captive_portal":{"state":"UNKNOWN","coverage":"NONE","applicability":"NOT_APPLICABLE","reason":"no_captive_probe","evidence":[]},
    "active_dns":{"state":"GOOD","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"","evidence":[]},
    "active_tcp":{"state":"GOOD","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"","evidence":[]},
    "active_https":{"state":"GOOD","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"","evidence":[]},
    "active_portal":{"state":"UNKNOWN","coverage":"NONE","applicability":"NOT_APPLICABLE","reason":"","evidence":[]}
  },
  "warnings":["rssi_weak","rtt_high"],
  "primary_issue":"rtt_high"
})";

TEST(ExperienceTest, ParsesFullSnapshot) {
    const ExperienceSnapshot snap = weaknet::dashboard::parse_experience(kFullSnapshot);

    ASSERT_TRUE(snap.available());
    EXPECT_TRUE(snap.unavailable_reason.empty());
    EXPECT_EQ(snap.schema_version, 2);
    EXPECT_EQ(snap.interface_name, "wlan0");
    EXPECT_EQ(snap.assessment_profile, "FULL");
    EXPECT_EQ(snap.overall_state, "DEGRADED");
    EXPECT_EQ(snap.overall_coverage, "FULL_FOR_PROFILE");
    EXPECT_DOUBLE_EQ(snap.display_score, 72.5);
}

TEST(ExperienceTest, ParsesNetworkHealthItems) {
    const ExperienceSnapshot snap = weaknet::dashboard::parse_experience(kFullSnapshot);

    EXPECT_EQ(snap.ip_reachability.state, "GOOD");
    EXPECT_EQ(snap.ip_reachability.coverage, "FULL_FOR_PROFILE");
    EXPECT_EQ(snap.ip_reachability.applicability, "APPLICABLE");

    EXPECT_EQ(snap.responsiveness.state, "DEGRADED");
    EXPECT_EQ(snap.responsiveness.reason, "rtt_high");

    EXPECT_EQ(snap.reliability.state, "GOOD");
    // 部分覆盖的维度要保留 PARTIAL，供 UI 标注
    EXPECT_EQ(snap.rf_health.coverage, "PARTIAL");
}

TEST(ExperienceTest, ParsesServiceHealthItems) {
    const ExperienceSnapshot snap = weaknet::dashboard::parse_experience(kFullSnapshot);

    EXPECT_EQ(snap.dns_service.state, "GOOD");
    EXPECT_EQ(snap.tcp_rtt.state, "DEGRADED");
    EXPECT_EQ(snap.active_https.state, "GOOD");

    // portal 类维度不适用时，应保留 NOT_APPLICABLE 而不是误报
    EXPECT_EQ(snap.captive_portal.applicability, "NOT_APPLICABLE");
    EXPECT_EQ(snap.captive_portal.coverage, "NONE");
    EXPECT_FALSE(snap.captive_portal.has_value());
    EXPECT_FALSE(snap.active_portal.has_value());
}

TEST(ExperienceTest, ParsesWarningsAndPrimaryIssue) {
    const ExperienceSnapshot snap = weaknet::dashboard::parse_experience(kFullSnapshot);
    ASSERT_EQ(snap.warnings.size(), 2u);
    EXPECT_EQ(snap.warnings[0], "rssi_weak");
    EXPECT_EQ(snap.primary_issue, "rtt_high");
}

TEST(ExperienceTest, CountsStatesAcrossAllDimensions) {
    const ExperienceSnapshot snap = weaknet::dashboard::parse_experience(kFullSnapshot);
    int h = 0, d = 0, f = 0, u = 0;
    snap.count_by_state(&h, &d, &f, &u);

    // 13 个维度中：GOOD 8（ip/reliability/dns/tcp_connect/http/active_dns/active_tcp/active_https）
    //              DEGRADED 3（responsiveness/rf_health/tcp_rtt）
    //              BAD 0
    //              UNKNOWN 2（captive_portal/active_portal）
    EXPECT_EQ(h, 8);
    EXPECT_EQ(d, 3);
    EXPECT_EQ(f, 0);
    EXPECT_EQ(u, 2);
    EXPECT_EQ(h + d + f + u, 13);
}

// ---------------------------------------------------------------------------
// ExperienceSnapshot —— 降级形状
// ---------------------------------------------------------------------------

TEST(ExperienceTest, ParsesNoAssessmentYet) {
    // 逐字取自 server/src/dbus_service.cpp:916-917
    const char* json =
        R"({"schema_version":2,"state":"UNKNOWN","coverage":"NONE","reason":"no_assessment_yet"})";
    const ExperienceSnapshot snap = weaknet::dashboard::parse_experience(json);

    EXPECT_FALSE(snap.available());
    EXPECT_EQ(snap.unavailable_reason, "no_assessment_yet");
    EXPECT_EQ(snap.overall_state, "UNKNOWN");
    EXPECT_DOUBLE_EQ(snap.display_score, 0.0);
}

TEST(ExperienceTest, ParsesStaleAssessment) {
    // 逐字取自 server/src/dbus_service.cpp:919-921
    const char* json = R"({"schema_version":2,"state":"UNKNOWN","coverage":"NONE",)"
                       R"("reason":"stale_assessment",)"
                       R"("note":"waiting for next evaluation with current config/epoch"})";
    const ExperienceSnapshot snap = weaknet::dashboard::parse_experience(json);

    EXPECT_FALSE(snap.available());
    EXPECT_EQ(snap.unavailable_reason, "stale_assessment");
}

TEST(ExperienceTest, InvalidJsonYieldsUnavailable) {
    const ExperienceSnapshot snap = weaknet::dashboard::parse_experience("not json at all");
    EXPECT_FALSE(snap.available());
    EXPECT_EQ(snap.unavailable_reason, "invalid_json");
}

TEST(ExperienceTest, EmptyStringYieldsUnavailable) {
    const ExperienceSnapshot snap = weaknet::dashboard::parse_experience("");
    EXPECT_FALSE(snap.available());
}

TEST(ExperienceTest, UnknownSchemaVersionIsRejectedNotMisparsed) {
    // schema v3 的字段布局可能与 v2 不同。继续按 v2 的路径取值会静默取到空串，
    // 界面上表现为「13 个维度全是 UNKNOWN」，比明确说「不支持」更难排查。
    const char* json = R"({"schema_version":3,"overall":{"state":"GOOD",)"
                       R"("coverage":"FULL_FOR_PROFILE","display_score":90}})";
    const ExperienceSnapshot snap = weaknet::dashboard::parse_experience(json);

    EXPECT_FALSE(snap.available());
    EXPECT_EQ(snap.unavailable_reason, "unsupported_schema_version_3");
    // 关键：没有把 v3 的数据按 v2 解析出来
    EXPECT_DOUBLE_EQ(snap.display_score, 0.0);
}

TEST(ExperienceTest, MissingSchemaVersionStillParses) {
    // 缺 schema_version 时不拒绝：降级形状与老样本都没有这个字段，
    // 一刀切拒绝会把可用的数据也挡在外面。
    const char* json = R"({"overall":{"state":"GOOD","coverage":"FULL_FOR_PROFILE",)"
                       R"("display_score":88},"interface":"wlan0"})";
    const ExperienceSnapshot snap = weaknet::dashboard::parse_experience(json);

    EXPECT_TRUE(snap.available());
    EXPECT_EQ(snap.schema_version, 0);
    EXPECT_DOUBLE_EQ(snap.display_score, 88.0);
}

// ---------------------------------------------------------------------------
// DiagnosisSnapshot
// ---------------------------------------------------------------------------

TEST(DiagnosisTest, ParsesDiagnosisFacts) {
    // 按 diagnosis_engine.hpp:44-85 的 toJson() 输出构造
    const char* json = R"({
      "fault_domain":"DNS",
      "primary_issue":"dns_resolution_slow",
      "confidence":"MEDIUM",
      "secondary_issues":["rtt_high"],
      "evidence_refs":["ev-001","ev-002"],
      "excluded_causes":[{"cause":"RF","reason":"rssi_normal"}],
      "actions":[{"priority":1,"action_id":"restart_dns_resolver","params":{"iface":"wlan0"}}],
      "default_summary_template":"DNS 解析偏慢"
    })";
    const DiagnosisSnapshot snap = weaknet::dashboard::parse_diagnosis(json);

    ASSERT_TRUE(snap.valid);
    EXPECT_EQ(snap.fault_domain, "DNS");
    EXPECT_EQ(snap.primary_issue, "dns_resolution_slow");
    EXPECT_EQ(snap.confidence, "MEDIUM");  // 字符串枚举，不是数值
    ASSERT_EQ(snap.secondary_issues.size(), 1u);
    ASSERT_EQ(snap.evidence_refs.size(), 2u);
    ASSERT_EQ(snap.excluded_causes.size(), 1u);
    EXPECT_EQ(snap.excluded_causes[0].cause, "RF");
    EXPECT_EQ(snap.excluded_causes[0].reason, "rssi_normal");
    ASSERT_EQ(snap.actions.size(), 1u);
    EXPECT_EQ(snap.actions[0].priority, 1);
    EXPECT_EQ(snap.actions[0].action_id, "restart_dns_resolver");
    EXPECT_EQ(snap.summary_template, "DNS 解析偏慢");
}

TEST(DiagnosisTest, ParsesNotReadyFallback) {
    // 逐字取自 server/src/dbus_service.cpp:427-432 的 fallback 形状
    const char* json = R"({"fault_domain":"NONE","primary_issue":"snapshot_not_ready",)"
                       R"("confidence":"LOW","secondary_issues":[],"evidence_refs":[],)"
                       R"("excluded_causes":[],"actions":[],)"
                       R"("default_summary_template":"系统监控评估快照正在采集中，尚未生成，请稍候。"})";
    const DiagnosisSnapshot snap = weaknet::dashboard::parse_diagnosis(json);

    EXPECT_TRUE(snap.valid);
    EXPECT_EQ(snap.fault_domain, "NONE");
    EXPECT_EQ(snap.primary_issue, "snapshot_not_ready");
    EXPECT_EQ(snap.confidence, "LOW");
    EXPECT_TRUE(snap.actions.empty());
}

TEST(DiagnosisTest, InvalidJsonIsNotValid) {
    EXPECT_FALSE(weaknet::dashboard::parse_diagnosis("garbage").valid);
}

// ---------------------------------------------------------------------------
// HistorySeries
// ---------------------------------------------------------------------------

TEST(HistoryTest, ParsesHistoryArray) {
    // 按 database_manager.cpp:703-740 的输出构造（含大量未映射字段，应被忽略）
    const char* json = R"([
      {"ts":"2026-11-05 14:23:20","iface":"wlan0","rtt_ms":25,"quality":"GOOD",
       "overall_quality":"GOOD","overall_score":88.000000,"score":88.000000,"legacy":false},
      {"ts":"2026-11-05 14:23:10","iface":"wlan0","rtt_ms":180,"quality":"POOR",
       "overall_quality":"POOR","overall_score":45.000000,"score":45.000000,"legacy":false}
    ])";
    const HistorySeries series = weaknet::dashboard::parse_history(json);

    ASSERT_EQ(series.points.size(), 2u);

    // 服务端按 ts DESC 返回，解析后应翻转为时间正序
    EXPECT_EQ(series.points[0].ts, "2026-11-05 14:23:10");
    EXPECT_DOUBLE_EQ(series.points[0].score, 45.0);
    EXPECT_EQ(series.points[1].ts, "2026-11-05 14:23:20");
    EXPECT_DOUBLE_EQ(series.points[1].score, 88.0);
}

TEST(HistoryTest, EmptyArray) {
    const HistorySeries series = weaknet::dashboard::parse_history("[]");
    EXPECT_TRUE(series.points.empty());
}

TEST(HistoryTest, DatabaseUnavailableObject) {
    // 逐字取自 server/src/dbus_service.cpp:1530
    const HistorySeries series =
        weaknet::dashboard::parse_history(R"({"error":"database not available"})");
    EXPECT_TRUE(series.points.empty());
}

TEST(HistoryTest, LegacyFlagIsPreserved) {
    const char* json = R"([{"ts":"t","iface":"wlan0","score":50,"legacy":true}])";
    const HistorySeries series = weaknet::dashboard::parse_history(json);
    ASSERT_EQ(series.points.size(), 1u);
    EXPECT_TRUE(series.points[0].legacy);
}

// ---------------------------------------------------------------------------
// HistoryPoint —— 多指标字段（趋势指标切换的数据前提）
//
// 这一组守的是**「未测量」不能伪装成 0** 这条不变量。
// 现场最怕的假象是：明明没有 RSSI 观测，图上却画出一条漂亮的 -0 dBm 直线。
// ---------------------------------------------------------------------------

TEST(HistoryMetricTest, ParsesAllNumericFields) {
    const char* json = R"([{"ts":"2026-11-08T10:00:00","score":88,
      "rtt_ms":25,"jitter_ms":3.5,"rssi_dbm":-47,"tcp_loss":0.75}])";
    const HistorySeries series = weaknet::dashboard::parse_history(json);

    ASSERT_EQ(series.points.size(), 1u);
    const auto& p = series.points[0];
    EXPECT_DOUBLE_EQ(p.score, 88.0);
    EXPECT_DOUBLE_EQ(p.rtt_ms, 25.0);
    EXPECT_DOUBLE_EQ(p.jitter_ms, 3.5);
    EXPECT_DOUBLE_EQ(p.rssi_dbm, -47.0);
    EXPECT_DOUBLE_EQ(p.tcp_loss, 0.75);
}

TEST(HistoryMetricTest, NullFieldsBecomeSentinelsNotZero) {
    // 服务端对未测量的指标输出 JSON null（database_manager.cpp:queryHistory
    // 里 `row.rtt_null ? "null" : ...`）。若解析层把 null 当 0，
    // 「没测到 RTT」会显示成「RTT = 0ms」——最快的一条假象。
    const char* json = R"([{"ts":"t","score":55,
      "rtt_ms":null,"jitter_ms":null,"rssi_dbm":null,"tcp_loss":null}])";
    const HistorySeries series = weaknet::dashboard::parse_history(json);

    ASSERT_EQ(series.points.size(), 1u);
    const auto& p = series.points[0];

    EXPECT_NE(p.rtt_ms, 0.0) << "null 不能变成 0ms";
    EXPECT_NE(p.jitter_ms, 0.0) << "null 不能变成 0ms";
    EXPECT_NE(p.rssi_dbm, 0.0) << "null 不能变成 0dBm";
    EXPECT_NE(p.tcp_loss, 0.0) << "null 不能变成 0% 丢包";

    // 并且要能被 has() 判出来
    EXPECT_FALSE(p.has(weaknet::dashboard::HistoryMetric::Rtt));
    EXPECT_FALSE(p.has(weaknet::dashboard::HistoryMetric::Jitter));
    EXPECT_FALSE(p.has(weaknet::dashboard::HistoryMetric::Rssi));
    EXPECT_FALSE(p.has(weaknet::dashboard::HistoryMetric::TcpLoss));

    // 分数没有「未测量」语义，null 之外一律照收
    EXPECT_TRUE(p.has(weaknet::dashboard::HistoryMetric::Score));
}

TEST(HistoryMetricTest, MissingFieldBehavesLikeNull) {
    // 字段整个缺失（老样本 / 服务端降级）与 null 应落同一个语义
    const char* json = R"([{"ts":"t","score":55}])";
    const HistorySeries series = weaknet::dashboard::parse_history(json);

    ASSERT_EQ(series.points.size(), 1u);
    EXPECT_FALSE(series.points[0].has(weaknet::dashboard::HistoryMetric::Rtt));
    EXPECT_FALSE(series.points[0].has(weaknet::dashboard::HistoryMetric::Rssi));
}

TEST(HistoryMetricTest, ZeroIsAValidMeasurementForLoss) {
    // 0% 丢包是「测了，没丢」——与「没测」完全不同。
    // 这条把它俩钉死成两个不同结果。
    const char* measured = R"([{"ts":"t","score":55,"tcp_loss":0}])";
    const char* missing = R"([{"ts":"t","score":55,"tcp_loss":null}])";

    const auto a = weaknet::dashboard::parse_history(measured);
    const auto b = weaknet::dashboard::parse_history(missing);

    ASSERT_EQ(a.points.size(), 1u);
    ASSERT_EQ(b.points.size(), 1u);

    EXPECT_TRUE(a.points[0].has(weaknet::dashboard::HistoryMetric::TcpLoss));
    EXPECT_FALSE(b.points[0].has(weaknet::dashboard::HistoryMetric::TcpLoss));
    EXPECT_DOUBLE_EQ(a.points[0].tcp_loss, 0.0);
}

TEST(HistoryMetricTest, ScoreZeroIsNotAbsent) {
    // 0 分是「极差」，不是「没数据」。若判成 absent，最差的时刻会从图上消失。
    const char* json = R"([{"ts":"t","score":0}])";
    const HistorySeries series = weaknet::dashboard::parse_history(json);

    ASSERT_EQ(series.points.size(), 1u);
    EXPECT_TRUE(series.points[0].has(weaknet::dashboard::HistoryMetric::Score));
    EXPECT_DOUBLE_EQ(series.points[0].value(weaknet::dashboard::HistoryMetric::Score),
                     0.0);
}

TEST(HistoryMetricTest, ValueAccessorDispatchesPerMetric) {
    HistoryPoint p;
    p.score = 77.0;
    p.rtt_ms = 25.0;
    p.jitter_ms = 3.5;
    p.rssi_dbm = -47.0;
    p.tcp_loss = 1.25;

    using weaknet::dashboard::HistoryMetric;
    EXPECT_DOUBLE_EQ(p.value(HistoryMetric::Score), 77.0);
    EXPECT_DOUBLE_EQ(p.value(HistoryMetric::Rtt), 25.0);
    EXPECT_DOUBLE_EQ(p.value(HistoryMetric::Jitter), 3.5);
    EXPECT_DOUBLE_EQ(p.value(HistoryMetric::Rssi), -47.0);
    EXPECT_DOUBLE_EQ(p.value(HistoryMetric::TcpLoss), 1.25);
}

TEST(HistoryMetricTest, AbsentDetectionUsesServerSentinelSemantics) {
    // 哨兵值本身来自服务端（net_info.hpp: 未测量 = -1.0；
    // database_manager.cpp: rssi 缺失 = -1000）。解析层只是沿用。
    using weaknet::dashboard::is_history_metric_absent;
    using weaknet::dashboard::HistoryMetric;

    EXPECT_TRUE(is_history_metric_absent(HistoryMetric::Rtt, -1.0));
    EXPECT_TRUE(is_history_metric_absent(HistoryMetric::Jitter, -1.0));
    EXPECT_TRUE(is_history_metric_absent(HistoryMetric::Rssi, -1000.0));
    EXPECT_TRUE(is_history_metric_absent(HistoryMetric::TcpLoss, -1.0));

    EXPECT_FALSE(is_history_metric_absent(HistoryMetric::Rtt, 0.0));
    EXPECT_FALSE(is_history_metric_absent(HistoryMetric::Rssi, -47.0));
    EXPECT_FALSE(is_history_metric_absent(HistoryMetric::TcpLoss, 0.0));
    // 分数永不缺席
    EXPECT_FALSE(is_history_metric_absent(HistoryMetric::Score, 0.0));
}

TEST(HistoryMetricTest, CountWithSkipsSentinelPoints) {
    const char* json = R"([
      {"ts":"a","score":60,"rtt_ms":20,"rssi_dbm":-50},
      {"ts":"b","score":55,"rtt_ms":null,"rssi_dbm":null},
      {"ts":"c","score":50,"rtt_ms":30,"rssi_dbm":-60}
    ])";
    const HistorySeries series = weaknet::dashboard::parse_history(json);

    using weaknet::dashboard::HistoryMetric;
    EXPECT_EQ(series.count_with(HistoryMetric::Score), 3);
    EXPECT_EQ(series.count_with(HistoryMetric::Rtt), 2);
    EXPECT_EQ(series.count_with(HistoryMetric::Rssi), 2);
    EXPECT_EQ(series.count_with(HistoryMetric::Jitter), 0);
}

TEST(HistoryMetricTest, LabelsAndUnits) {
    using weaknet::dashboard::history_metric_label;
    using weaknet::dashboard::history_metric_unit;
    using weaknet::dashboard::HistoryMetric;

    // 五个指标都要有非空中文名，否则下拉框会出现空条目
    for (HistoryMetric m : {HistoryMetric::Score, HistoryMetric::Rtt,
                            HistoryMetric::Jitter, HistoryMetric::Rssi,
                            HistoryMetric::TcpLoss}) {
        EXPECT_FALSE(std::string(history_metric_label(m)).empty());
    }

    EXPECT_STREQ(history_metric_unit(HistoryMetric::Score), "");
    EXPECT_STREQ(history_metric_unit(HistoryMetric::Rtt), "ms");
    EXPECT_STREQ(history_metric_unit(HistoryMetric::Jitter), "ms");
    EXPECT_STREQ(history_metric_unit(HistoryMetric::Rssi), "dBm");
    EXPECT_STREQ(history_metric_unit(HistoryMetric::TcpLoss), "%");
}

TEST(HistoryMetricTest, ScoreRangeIsAdaptiveOthersAreFixed) {
    using weaknet::dashboard::history_metric_range;
    using weaknet::dashboard::HistoryMetric;

    // 分数是归一化量纲，按数据自适应看相对起伏
    EXPECT_FALSE(history_metric_range(HistoryMetric::Score).fixed);

    // 其余三个有绝对含义，必须钉住范围，否则 25ms↔30ms 会被拉成断崖
    for (HistoryMetric m : {HistoryMetric::Rtt, HistoryMetric::Jitter,
                            HistoryMetric::Rssi, HistoryMetric::TcpLoss}) {
        const auto r = history_metric_range(m);
        EXPECT_TRUE(r.fixed) << history_metric_label(m);
        EXPECT_LT(r.lo, r.hi) << history_metric_label(m);
    }

    // RSSI 是负值区间：轴必须能装下 -47 这类真机值
    const auto rssi = history_metric_range(HistoryMetric::Rssi);
    EXPECT_LT(rssi.lo, -47.0);
    EXPECT_GT(rssi.hi, -47.0);
}

// ---------------------------------------------------------------------------
// 时间窗 —— 白名单批准的 1h / 6h / 13h
//
// 这一组守的是一条硬约束：**请求的点数必须 <= 服务端 10000 上限**。
// 若有人日后加一个「7 天」窗口而忘记算点数，这里会立刻失败。
// ---------------------------------------------------------------------------

TEST(HistoryWindowTest, ThreeWindowsHaveExpectedDurations) {
    using weaknet::dashboard::history_window_seconds;
    using weaknet::dashboard::HistoryWindow;

    EXPECT_EQ(history_window_seconds(HistoryWindow::Last1h), 3600);
    EXPECT_EQ(history_window_seconds(HistoryWindow::Last6h), 21600);
    EXPECT_EQ(history_window_seconds(HistoryWindow::Last13h), 13 * 3600);
}

TEST(HistoryWindowTest, LimitNeverExceedsServerCap) {
    // server/src/dbus_service.cpp:1515 的 kMaxHistoryLimit = 10000。
    // 超过它会被**静默钳制**（板端实测：请求 limit=20000 返回正好 10000 条，
    // 跨度 69.99h 而非请求的 7 天），图上就会出现「标签 X、实际只有一部分」。
    //
    // 13h 是当前能给的最大窗口：13*3600/5 = 9360 条，留 640 条余量。
    // 24h 需要 17280 条，超出上限 73% —— 因此界面不提供 24h。
    using weaknet::dashboard::history_window_limit;
    using weaknet::dashboard::HistoryWindow;

    constexpr int kServerCap = 10000;
    for (HistoryWindow w : {HistoryWindow::Last1h, HistoryWindow::Last6h,
                            HistoryWindow::Last13h}) {
        const int limit = history_window_limit(w);
        EXPECT_GT(limit, 0);
        EXPECT_LE(limit, kServerCap) << "窗口请求点数超过服务端上限";
    }
}

TEST(HistoryWindowTest, LimitMatchesSamplingInterval) {
    // 720 / 4320 / 9360 是按**真实落库周期 5 秒**算出来的确定值。
    //
    // 历史：这里原本按 10 秒算成 360 / 2160 / 8640，依据是 CLAUDE.md 里
    // 「数据采集间隔：10 秒」。但板端实测（2026-11-08，直调 libweaknet
    // 取 20 分钟样本）相邻记录间隔中位数 **5.0 秒**、最大 6.0 秒——落库周期
    // 是 `server/src/server.cpp` `start_history_persistence_thread()` 里
    // 硬编码的 5 秒，与 config.yaml 的各监控器 `interval_ms` 不是同一件事。
    //
    // 采样间隔少算一半，直接后果是每个窗口的 limit 只有实际所需的一半：
    // 服务端是 `ORDER BY ts DESC LIMIT ?`，先倒序再截断，于是 24h 请求
    // 8640 条只能覆盖约 12 小时，标签写 24h、图里画 12h。
    using weaknet::dashboard::history_window_limit;
    using weaknet::dashboard::HistoryWindow;

    EXPECT_EQ(history_window_limit(HistoryWindow::Last1h), 720);
    EXPECT_EQ(history_window_limit(HistoryWindow::Last6h), 4320);
    EXPECT_EQ(history_window_limit(HistoryWindow::Last13h), 9360);
}

TEST(HistoryWindowTest, SamplingIntervalComesFromPersistenceThread) {
    // 把「5 秒」这个事实本身钉成断言，而不是让它散落在注释里。
    //
    // 这个常量的唯一权威来源是服务端落库线程的循环周期。若服务端将来改成
    // 别的值，这条测试不会自动跟着变（它读的是看板侧的常量），但它会在
    // 有人「顺手调大」时立刻失败，强制其回去核对服务端。
    using weaknet::dashboard::kHistorySampleIntervalSeconds;
    EXPECT_EQ(kHistorySampleIntervalSeconds, 5)
        << "看板采样间隔必须等于 server.cpp start_history_persistence_thread() "
           "的落库周期（当前硬编码 5 秒）；两者不一致会让时间窗标签说谎";
}

TEST(HistoryWindowTest, StartTimestampHasServerFormat) {
    // database_manager.cpp 的 currentTimestamp() 用 `%Y-%m-%dT%H:%M:%S`。
    // 格式不一致会让 `ts >= ?` 静默比较失败，窗口看起来"没生效"。
    using weaknet::dashboard::history_window_start;
    using weaknet::dashboard::HistoryWindow;

    // 2026-11-08T12:00:00 UTC 附近的一个固定时刻，避免依赖真机时钟
    const long long now = 1794153600LL;  // 2026-11-08T16:00:00Z
    const std::string start = history_window_start(HistoryWindow::Last1h, now);

    ASSERT_EQ(start.size(), 19u) << "应为 YYYY-MM-DDTHH:MM:SS";
    EXPECT_EQ(start[4], '-');
    EXPECT_EQ(start[7], '-');
    EXPECT_EQ(start[10], 'T') << "必须带 T 分隔，与 currentTimestamp() 一致";
    EXPECT_EQ(start[13], ':');
    EXPECT_EQ(start[16], ':');
}

TEST(HistoryWindowTest, LongerWindowYieldsEarlierStart) {
    using weaknet::dashboard::history_window_start;
    using weaknet::dashboard::HistoryWindow;

    const long long now = 1794153600LL;
    const std::string h1 = history_window_start(HistoryWindow::Last1h, now);
    const std::string h6 = history_window_start(HistoryWindow::Last6h, now);
    const std::string h13 = history_window_start(HistoryWindow::Last13h, now);

    // ts 是固定宽度的同格式字符串，字典序即时间序
    EXPECT_LT(h13, h6);
    EXPECT_LT(h6, h1);
}

// ---------------------------------------------------------------------------
// 真机实测样本（2026-11-05，radxa-cubie-a7a）
//
// 上面那组样本是**从服务端序列化代码抄来的**，验证的是「代码怎么写」。
// 这一组是**从运行中的服务端真取回来的**，验证的是「实际返回什么」。
//
// 两者不能互相替代：代码抄本会漏掉运行时才出现的字段组合与取值
// （例如大量 UNKNOWN/NONE 的服务维度、PARTIAL 覆盖、中文摘要模板）。
// 样本原文见 tests/fixtures/board_experience_real.json。
// ---------------------------------------------------------------------------

// 逐字取自板端 weaknet_get_network_experience() 的真实返回（4129 字节原文，
// 此处保留结构与全部字段值，只删去 evidence 数组内容以保持可读）
const char* kBoardRealSnapshot = R"({
  "schema_version":2,
  "interface":"wlan0",
  "assessment_profile":"INTERNET_ACCESS",
  "overall":{"state":"DEGRADED","coverage":"PARTIAL","display_score":55},
  "network_health":{
    "ip_reachability":{"state":"GOOD","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"ip_path_fully_reachable","evidence":[]},
    "responsiveness":{"state":"BAD","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"rtt_severely_high_or_volatile","evidence":[]},
    "reliability":{"state":"GOOD","coverage":"PARTIAL","applicability":"APPLICABLE","reason":"partial_metrics_healthy","evidence":[]},
    "rf_health":{"state":"GOOD","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"strong_rf_signal","evidence":[]}
  },
  "service_health":{
    "dns":{"state":"UNKNOWN","coverage":"NONE","applicability":"APPLICABLE","reason":"no_dns_observations","evidence":[]},
    "tcp_connect":{"state":"UNKNOWN","coverage":"PARTIAL","applicability":"APPLICABLE","reason":"observer_unreliable_pairing_incomplete","evidence":[]},
    "tcp_rtt":{"state":"DEGRADED","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"elevated_passive_tcp_rtt_p90","evidence":[]},
    "http_access":{"state":"UNKNOWN","coverage":"NONE","applicability":"APPLICABLE","reason":"no_http_observations","evidence":[]},
    "captive_portal":{"state":"UNKNOWN","coverage":"NONE","applicability":"APPLICABLE","reason":"no_portal_probe_capability","evidence":[]},
    "active_dns":{"state":"GOOD","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"active_dns_capability_ok","evidence":[]},
    "active_tcp":{"state":"GOOD","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"active_tcp_capability_ok","evidence":[]},
    "active_https":{"state":"GOOD","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"active_https_capability_ok","evidence":[]},
    "active_portal":{"state":"GOOD","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"no_captive_portal","evidence":[]}
  },
  "warnings":[],
  "primary_issue":"High latency or excessive jitter"
})";

TEST(BoardRealSampleTest, ParsesRealSnapshotEndToEnd) {
    const ExperienceSnapshot snap =
        weaknet::dashboard::parse_experience(kBoardRealSnapshot);

    ASSERT_TRUE(snap.available());
    EXPECT_EQ(snap.schema_version, 2);
    EXPECT_EQ(snap.interface_name, "wlan0");
    // 真机是 INTERNET_ACCESS，不是代码抄本里的 FULL
    EXPECT_EQ(snap.assessment_profile, "INTERNET_ACCESS");
    EXPECT_EQ(snap.overall_state, "DEGRADED");
    EXPECT_EQ(snap.overall_coverage, "PARTIAL");
    EXPECT_DOUBLE_EQ(snap.display_score, 55.0);
}

TEST(BoardRealSampleTest, RealSnapshotHasMixedDimensionStates) {
    const ExperienceSnapshot snap =
        weaknet::dashboard::parse_experience(kBoardRealSnapshot);

    // 真机同时出现 GOOD / BAD / DEGRADED / UNKNOWN 四种状态，
    // 这是代码抄本样本覆盖不到的组合
    EXPECT_EQ(snap.ip_reachability.state, "GOOD");
    EXPECT_EQ(snap.responsiveness.state, "BAD");
    EXPECT_EQ(snap.reliability.state, "GOOD");
    EXPECT_EQ(snap.rf_health.state, "GOOD");
    EXPECT_EQ(snap.tcp_rtt.state, "DEGRADED");

    // 大量服务维度在真机上是 UNKNOWN + NONE（没有观测数据）
    EXPECT_EQ(snap.dns_service.state, "UNKNOWN");
    EXPECT_EQ(snap.http_access.state, "UNKNOWN");
    EXPECT_FALSE(snap.dns_service.has_value());
    EXPECT_FALSE(snap.http_access.has_value());

    // 但 tcp_connect 是 UNKNOWN + PARTIAL：有覆盖但不可用，
    // 与「完全没观测」不是同一回事，看板显示逻辑必须能区分
    EXPECT_EQ(snap.tcp_connect.state, "UNKNOWN");
    EXPECT_EQ(snap.tcp_connect.coverage, "PARTIAL");
}

TEST(BoardRealSampleTest, RealSnapshotPartialCoverageIsDetected) {
    const ExperienceSnapshot snap =
        weaknet::dashboard::parse_experience(kBoardRealSnapshot);

    // 看板对 PARTIAL 覆盖的维度会显示 "(部分)" 后缀，
    // 这里固定住真实数据里确实存在 PARTIAL 维度这一事实
    EXPECT_EQ(snap.overall_coverage, "PARTIAL");
    EXPECT_EQ(snap.reliability.coverage, "PARTIAL");
    EXPECT_EQ(snap.tcp_connect.coverage, "PARTIAL");

    int healthy = 0, degraded = 0, failed = 0, unknown = 0;
    snap.count_by_state(&healthy, &degraded, &failed, &unknown);
    EXPECT_EQ(healthy + degraded + failed + unknown, 13);
    EXPECT_GT(unknown, 0);
    EXPECT_GT(failed, 0);  // responsiveness 是 BAD，计入 failed
}

// 逐字取自板端 weaknet_get_diagnosis() 的真实返回（285 字节原文）
const char* kBoardRealDiagnosis = R"({"fault_domain":"UNKNOWN","primary_issue":"High latency or excessive jitter","confidence":"LOW","secondary_issues":[],"evidence_refs":[],"excluded_causes":[],"actions":[],"default_summary_template":"当前网络指标存在异常但缺乏明确的判定规则，建议人工关注。"})";

TEST(BoardRealSampleTest, ParsesRealDiagnosisWithChineseSummary) {
    const DiagnosisSnapshot snap =
        weaknet::dashboard::parse_diagnosis(kBoardRealDiagnosis);

    EXPECT_TRUE(snap.valid);
    EXPECT_EQ(snap.fault_domain, "UNKNOWN");
    EXPECT_EQ(snap.primary_issue, "High latency or excessive jitter");
    EXPECT_EQ(snap.confidence, "LOW");
    // 摘要模板是中文，且原文里包含 UTF-8 多字节字符。
    // 这里守住解析器不会在 UTF-8 上截断或乱码
    EXPECT_FALSE(snap.summary_template.empty());
    EXPECT_NE(snap.summary_template.find("当前网络指标"), std::string::npos);
    EXPECT_TRUE(snap.secondary_issues.empty());
    EXPECT_TRUE(snap.excluded_causes.empty());
    EXPECT_TRUE(snap.actions.empty());
}

// 逐字取自板端 weaknet_get_history() 的真实返回元素（字段取自 60 条中的一条）。
// 注意顺序：服务端按 ts DESC 返回，最新在前——这与真实响应一致。
const char* kBoardRealHistory = R"([
  {"ts":"2026-11-05T13:31:56","iface":"wlan0","rtt_ms":58,"jitter_ms":62.72,
   "rssi_dbm":-47,"rssi_source":"nl80211","rssi_estimated":false,
   "rssi_status":"valid","rtt_status":"valid","jitter_status":"valid",
   "tcp_loss_status":"valid","traffic_status":"valid","tcp_loss":0.0,
   "quality":"FAIR","link_quality":"FAIR","overall_quality":"FAIR",
   "overall_score":55.0,"score":55.0,"legacy":false},
  {"ts":"2026-11-05T13:27:00","iface":"wlan0","rtt_ms":58,"jitter_ms":62.72,
   "rssi_dbm":-47,"rssi_source":"nl80211","rssi_estimated":false,
   "rssi_status":"valid","rtt_status":"valid","jitter_status":"valid",
   "tcp_loss_status":"valid","traffic_status":"valid","tcp_loss":0.0,
   "quality":"FAIR","link_quality":"FAIR","overall_quality":"FAIR",
   "overall_score":55.0,"score":55.0,"legacy":false}
])";

TEST(BoardRealSampleTest, ParsesRealHistoryTimestampsAndFlipsOrder) {
    const HistorySeries series =
        weaknet::dashboard::parse_history(kBoardRealHistory);

    ASSERT_EQ(series.points.size(), 2u);
    // 服务端按 ts DESC 返回，parse 后应是正序
    EXPECT_EQ(series.points.front().ts, "2026-11-05T13:27:00");
    EXPECT_EQ(series.points.back().ts, "2026-11-05T13:31:56");
    EXPECT_LE(series.points.front().ts, series.points.back().ts);

    // 真机 ts 是 ISO 8601 带 T 分隔，不是 SQLite 的空格分隔形式
    EXPECT_NE(series.points.front().ts.find('T'), std::string::npos);

    EXPECT_DOUBLE_EQ(series.points.front().score, 55.0);
    EXPECT_FALSE(series.points.front().legacy);
}

TEST(BoardRealSampleTest, RealHistoryFlatScoreKeepsTimeOrderAndValues) {
    // 真机 60 个点的 score 全是 55.0。这种全等值序列是图表的边界情况：
    // 轴范围必须由 LineChartWidget 自己兜底（±5 最小跨度），
    // 数据层只负责如实保留原值，不做任何"修正"。
    const HistorySeries series =
        weaknet::dashboard::parse_history(kBoardRealHistory);

    ASSERT_EQ(series.points.size(), 2u);
    // 翻转后应是时间正序
    EXPECT_LE(series.points.front().ts, series.points.back().ts);

    double lo = 1e9, hi = -1e9;
    for (const auto& p : series.points) {
        lo = std::min(lo, p.score);
        hi = std::max(hi, p.score);
        EXPECT_DOUBLE_EQ(p.score, 55.0);
    }
    // 数据层如实报告全等值：min == max，由视图层负责展开轴范围
    EXPECT_DOUBLE_EQ(lo, 55.0);
    EXPECT_DOUBLE_EQ(hi, 55.0);
}

// ---------------------------------------------------------------------------
// 本轮新增行为：诊断 primary_issue 的兜底契约
// ---------------------------------------------------------------------------

TEST(DiagnosisTest, PrimaryIssueSurvivesWithoutSummaryTemplate) {
    // UI 在 default_summary_template 为空时用 primary_issue 兜底。
    // 这条测试守住前提：解析层必须把 primary_issue 留着，
    // 否则兜底分支永远走不到，那一行会整个隐藏。
    const char* json = R"({
      "fault_domain":"DNS",
      "primary_issue":"dns_resolution_slow",
      "confidence":"MEDIUM",
      "secondary_issues":[],
      "evidence_refs":[],
      "excluded_causes":[],
      "actions":[],
      "default_summary_template":""
    })";
    const DiagnosisSnapshot snap = weaknet::dashboard::parse_diagnosis(json);

    ASSERT_TRUE(snap.valid);
    EXPECT_TRUE(snap.summary_template.empty());
    EXPECT_EQ(snap.primary_issue, "dns_resolution_slow");
}

TEST(DiagnosisTest, MissingPrimaryIssueAndSummaryBothEmpty) {
    // 两者都缺失时 UI 会隐藏整行。这里确认解析层给的是空串而不是
    // 某个占位 token——否则界面会显示一句「首要问题 no_issue_detected」。
    // 注意 fault_domain 有 UNKNOWN 兜底，primary_issue 的兜底是
    // "no_issue_detected"，只在字段真的缺失时才生效。
    const char* json = R"({"fault_domain":"NONE","confidence":"LOW"})";
    const DiagnosisSnapshot snap = weaknet::dashboard::parse_diagnosis(json);

    ASSERT_TRUE(snap.valid);
    EXPECT_TRUE(snap.summary_template.empty());
    // 字段缺失 → 走 fallback
    EXPECT_EQ(snap.primary_issue, "no_issue_detected");
}

TEST(DiagnosisTest, EmptyStringPrimaryIssueIsNotFallback) {
    // 区分「字段缺失」和「字段存在但为空串」：
    // 后者不应被 getString 的 fallback 覆盖成 no_issue_detected，
    // 否则界面会给用户看一个服务端并没有说过的结论。
    const char* json = R"({
      "fault_domain":"NONE",
      "primary_issue":"",
      "confidence":"LOW",
      "default_summary_template":""
    })";
    const DiagnosisSnapshot snap = weaknet::dashboard::parse_diagnosis(json);

    ASSERT_TRUE(snap.valid);
    EXPECT_TRUE(snap.primary_issue.empty());
    EXPECT_TRUE(snap.summary_template.empty());
}

} // namespace

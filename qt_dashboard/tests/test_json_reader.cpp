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

#include "data/dashboard_data.hpp"
#include "data/json_reader.hpp"

namespace {

using weaknet::dashboard::DiagnosisSnapshot;
using weaknet::dashboard::ExperienceSnapshot;
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
    EXPECT_TRUE(r.rootIsArray());
    EXPECT_EQ(r.rootArraySize(), 2u);
    const auto elems = r.getRootObjectArray();
    ASSERT_EQ(elems.size(), 2u);
    EXPECT_EQ(JsonReader(elems[1]).getInt("x", -1), 2);
}

TEST(JsonReaderTest, EmptyRootArray) {
    const JsonReader r("[]");
    EXPECT_TRUE(r.rootIsArray());
    EXPECT_EQ(r.rootArraySize(), 0u);
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
    EXPECT_EQ(series.interface_name, "wlan0");

    // 服务端按 ts DESC 返回，解析后应翻转为时间正序
    EXPECT_EQ(series.points[0].ts, "2026-11-05 14:23:10");
    EXPECT_DOUBLE_EQ(series.points[0].score, 45.0);
    EXPECT_EQ(series.points[0].overall_quality, "POOR");
    EXPECT_EQ(series.points[1].ts, "2026-11-05 14:23:20");
    EXPECT_DOUBLE_EQ(series.points[1].score, 88.0);
}

TEST(HistoryTest, ScoreRangeForChartAxis) {
    const char* json = R"([
      {"ts":"t1","iface":"wlan0","score":70.0},
      {"ts":"t2","iface":"wlan0","score":95.0},
      {"ts":"t3","iface":"wlan0","score":40.0}
    ])";
    const HistorySeries series = weaknet::dashboard::parse_history(json);

    double lo = 0, hi = 0;
    series.score_range(&lo, &hi);
    EXPECT_DOUBLE_EQ(lo, 40.0);
    EXPECT_DOUBLE_EQ(hi, 95.0);
}

TEST(HistoryTest, EmptyArray) {
    const HistorySeries series = weaknet::dashboard::parse_history("[]");
    EXPECT_TRUE(series.empty());
}

TEST(HistoryTest, DatabaseUnavailableObject) {
    // 逐字取自 server/src/dbus_service.cpp:1530
    const HistorySeries series =
        weaknet::dashboard::parse_history(R"({"error":"database not available"})");
    EXPECT_TRUE(series.empty());
}

TEST(HistoryTest, EmptySeriesScoreRangeDefaultsTo0to100) {
    const HistorySeries series;
    double lo = -1, hi = -1;
    series.score_range(&lo, &hi);
    EXPECT_DOUBLE_EQ(lo, 0.0);
    EXPECT_DOUBLE_EQ(hi, 100.0);
}

TEST(HistoryTest, LegacyFlagIsPreserved) {
    const char* json = R"([{"ts":"t","iface":"wlan0","score":50,"legacy":true}])";
    const HistorySeries series = weaknet::dashboard::parse_history(json);
    ASSERT_EQ(series.points.size(), 1u);
    EXPECT_TRUE(series.points[0].legacy);
}

} // namespace

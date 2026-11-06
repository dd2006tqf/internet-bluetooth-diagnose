/**
 * @file dashboard_data.hpp
 * @brief Qt 看板使用的 POD 数据结构
 *
 * ## 数据来源
 *
 * 全部来自现有 D-Bus 只读方法，经 `client/weaknet_client.h` 的 C API 获取：
 *
 *   weaknet_get_network_experience()  → ExperienceSnapshot
 *   weaknet_get_diagnosis()           → DiagnosisSnapshot
 *   weaknet_get_history()             → vector<HistoryPoint>
 *
 * 不新增 D-Bus 方法，不修改服务端。
 *
 * ## 为什么单独定义 POD
 *
 * 参考 `monitor_system` 的 `monitor_data.h` 做法：**把传输层挡在 UI 之外**。
 *
 * 直接让 UI 解析 JSON 的后果：
 *   - JSON 字段改名 → 改 UI 代码
 *   - UI 代码里出现大量 `getString("overall.state")` 这类魔法字符串
 *   - 无法对数据映射逻辑做单元测试
 *
 * 分离后：
 *   - JSON → POD 是纯函数，可独立单测
 *   - UI 只读 POD，不知道 JSON 长什么样
 *
 * ## 字段与 schema v2 的对应
 *
 * schema v2 的实际形状由 `server/include/assurance/legacy_adapter.hpp:162`
 * 的 `toExperienceJsonV2()` 定义。本文件的字段与之逐项对应。
 */

#ifndef WEAKNET_QT_DASHBOARD_DASHBOARD_DATA_HPP
#define WEAKNET_QT_DASHBOARD_DASHBOARD_DATA_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace weaknet {
namespace dashboard {

/**
 * @brief 单个维度的 SLE 评估结果
 *
 * 对应 JSON: `network_health.ip_reachability` 等
 */
struct SleItem {
    /// 维度标识（如 "ip_reachability"、"dns"）
    std::string key;

    /// 状态：GOOD / DEGRADED / BAD / UNKNOWN
    ///
    /// 取值来自 `server/include/assurance/health_state.hpp:15-23`
    /// 的 `healthStateToString()`。注意不是 HEALTHY/FAILED。
    std::string state;

    /// 覆盖率：FULL_FOR_PROFILE / PARTIAL / NONE
    ///
    /// 取值来自同文件 `coverageToString()`（`:31-38`）。注意不是 FULL。
    std::string coverage;

    /// 适用性：APPLICABLE / NOT_APPLICABLE
    std::string applicability;

    /// 原因说明（机器可读 token）
    std::string reason;

    /// 是否携带有效评估（state != UNKNOWN 且 coverage != NONE）
    bool has_value() const {
        return state != "UNKNOWN" && !state.empty();
    }

    /// 该维度是否适用于当前链路形态（如 RF Health 在有线链路下不适用）
    bool applicable() const { return applicability != "NOT_APPLICABLE"; }
};

/**
 * @brief 网络体验快照（schema v2）
 *
 * 对应 `GetNetworkExperience` 的返回。
 *
 * **注意**：服务端在无快照或快照过期时返回的是
 * `{"schema_version":2,"state":"UNKNOWN","coverage":"NONE","reason":"..."}`，
 * 此时顶层没有 `overall` 对象。解析时需兼容这种形状。
 */
struct ExperienceSnapshot {
    int schema_version = 0;

    /// 被评估的网卡名（如 "wlan0"）
    std::string interface_name;

    /// 评估档位（如 "FULL" / "PARTIAL"）
    std::string assessment_profile;

    // --- overall ---
    std::string overall_state = "UNKNOWN";
    std::string overall_coverage = "NONE";
    double display_score = 0.0;

    // --- network_health（4 项）---
    SleItem ip_reachability;
    SleItem responsiveness;
    SleItem reliability;
    SleItem rf_health;

    // --- service_health（9 项）---
    SleItem dns_service;
    SleItem tcp_connect;
    SleItem tcp_rtt;
    SleItem http_access;
    SleItem captive_portal;
    SleItem active_dns;
    SleItem active_tcp;
    SleItem active_https;
    SleItem active_portal;

    /// 警告列表
    std::vector<std::string> warnings;

    /// 首要问题（无则为空）
    std::string primary_issue;

    /// 顶层失效原因（无快照 / 快照过期时非空）
    std::string unavailable_reason;

    /**
     * @brief 是否是可展示的有效快照
     *
     * 服务端返回 UNKNOWN 时（no_assessment_yet / stale_assessment），
     * 看板应显示「等待评估」而不是空白或 0 分。
     */
    bool available() const {
        return unavailable_reason.empty() && overall_state != "UNKNOWN";
    }

    /// 按状态汇总各维度计数：健康 / 降级 / 失败 / 未知
    void count_by_state(int* healthy, int* degraded, int* failed, int* unknown) const;
};

/**
 * @brief 被排除的候选原因
 *
 * 对应 JSON: `excluded_causes[].{cause,reason}`
 */
struct ExcludedCause {
    std::string cause;
    std::string reason;
};

/**
 * @brief 建议动作
 *
 * 对应 JSON: `actions[].{priority,action_id,params}`
 *
 * 只是**建议**。真正执行必须走 `weaknet_execute_action()`，
 * 且该 D-Bus 方法要求 root 权限（`dbus_service.cpp:requireRootCaller`）。
 * 看板不自行执行任何动作。
 */
struct ActionSuggestion {
    int priority = 1;
    std::string action_id;
};

/**
 * @brief 诊断结论快照
 *
 * 对应 `GetDiagnosis` 的返回（DiagnosisFacts JSON）。
 *
 * 字段逐项依据 `server/include/assurance/diagnosis_engine.hpp:44-85`
 * 的 `DiagnosisFacts::toJson()`：
 *
 * ```json
 * {"fault_domain":"...","primary_issue":"...","confidence":"HIGH|MEDIUM|LOW",
 *  "secondary_issues":["..."],"evidence_refs":["..."],
 *  "excluded_causes":[{"cause":"...","reason":"..."}],
 *  "actions":[{"priority":1,"action_id":"...","params":{...}}],
 *  "default_summary_template":"..."}
 * ```
 *
 * 注意 `confidence` 是**字符串枚举**，不是数值。
 */
struct DiagnosisSnapshot {
    /// 是否成功取得诊断
    bool valid = false;

    /// 故障域（如 "DNS" / "RF" / "TCP" / "HTTP" / "NONE"）
    std::string fault_domain;

    /// 首要问题描述（机器 token）
    std::string primary_issue;

    /// 置信度：HIGH / MEDIUM / LOW
    std::string confidence = "LOW";

    /// 次要问题
    std::vector<std::string> secondary_issues;

    /// 引用的证据 ID
    std::vector<std::string> evidence_refs;

    /// 被排除的候选原因
    std::vector<ExcludedCause> excluded_causes;

    /// 建议动作
    std::vector<ActionSuggestion> actions;

    /// 人读摘要模板
    std::string summary_template;
};

/**
 * @brief 历史趋势的单个数据点
 *
 * 对应 `GetHistory` 返回的数组元素。
 *
 * 字段名依据 `server/src/database_manager.cpp:703-740` 的 `queryHistory()`
 * 实际输出。注意 `ts` 是 **字符串** 而不是数字（SQLite 里按 TEXT 存），
 * 形如 "2026-11-05 14:23:10"。
 *
 * 该结构只保留看板画图需要的字段，其余（rssi/jitter/traffic 等）暂不映射，
 * 需要时再扩展。
 */
struct HistoryPoint {
    /// 采样时间（字符串，来自 `ts` 字段）
    std::string ts;

    /// 该时刻的综合分（来自 `score`，与 `overall_score` 同值）
    double score = 0.0;

    /// 该行是否为 v2 之前的历史数据（来自 `legacy`，即 `data_version < 2`）
    ///
    /// 关键语义：v1 用的是旧的加权模型，同一分数在不同模型下**不可直接比较**。
    /// 图表必须把它画得与 v2 点不同，否则会读出一条不存在的趋势。
    bool legacy = false;
};

/**
 * @brief 历史趋势序列
 *
 * 不携带网卡名：查询「全部网卡」时任意一条记录的 iface 都不代表整个序列，
 * 请求侧的网卡由调用方（MainWindow::interface_）持有。
 */
struct HistorySeries {
    std::vector<HistoryPoint> points;
};

// ---------------------------------------------------------------------------
// 解析函数（JSON 字符串 → POD）
// ---------------------------------------------------------------------------

/**
 * @brief 解析 schema v2 体验快照 JSON
 *
 * @param json `weaknet_get_network_experience()` 返回的字符串
 * @return 解析结果；输入无效时返回默认构造对象（available() 为 false）
 */
ExperienceSnapshot parse_experience(const std::string& json);

/**
 * @brief 解析诊断事实 JSON
 */
DiagnosisSnapshot parse_diagnosis(const std::string& json);

/**
 * @brief 解析历史趋势 JSON
 *
 * 服务端 `GetHistory` 实际返回的是顶层数组，见
 * `server/src/database_manager.cpp:686-744`。数据库不可用时返回
 * `{"error":"database not available"}`（对象而非数组），此时返回空序列。
 *
 * 各点按 `ORDER BY ts DESC` 逆序排列，这里统一翻转为时间正序，
 * 便于图表从左到右绘制。
 */
HistorySeries parse_history(const std::string& json);

} // namespace dashboard
} // namespace weaknet

#endif // WEAKNET_QT_DASHBOARD_DASHBOARD_DATA_HPP

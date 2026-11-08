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
 * @brief 历史趋势可切换的指标
 *
 * ## 为什么是这 5 个
 *
 * `network_history` 表里还有 `traffic_bps` / `flows` / `tcp_conn_active` 等
 * 十余个数值列（见 `server/src/database_manager.cpp` 的 `queryHistory()`
 * SELECT 列表），但现场交互终端的能力白名单只批准「趋势指标切换」一项。
 * 每多一个指标就多一份「这个轴该怎么读」的解释成本，而现场要的是
 * 秒级确认，不是分析工具。
 *
 * 选这 5 个是因为它们覆盖了三个层次，且每个都对应一个现场会问的问题：
 *
 * | 指标 | 层次 | 现场问题 |
 * | --- | --- | --- |
 * | Score | 整体结论 | 到底好不好 |
 * | Rtt | 响应性 | 慢不慢 |
 * | Jitter | 稳定性 | 抖不抖 |
 * | Rssi | 射频物理层 | 信号够不够 |
 * | TcpLoss | 传输层 | 丢不丢包 |
 *
 * ## 13 个 SLE 维度为什么进不来
 *
 * `ip_reachability` / `dns` / `tcp_connect` 等 SLE 维度在 `network_history`
 * 表里**没有存储列**——落库的是原始测量值，SLE 结论只在实时评估里算出来。
 * 想画 SLE 维度的趋势必须新增服务端存储表面，那是独立规划范围。
 */
enum class HistoryMetric {
    Score,
    Rtt,
    Jitter,
    Rssi,
    TcpLoss,
};

/**
 * @brief 各指标的「未测量」哨兵值
 *
 * ## 为什么不用 0，也不用 NaN
 *
 * 1. **0 是合法值**。`tcp_loss = 0` 表示「测了，没丢包」，与「没测」是
 *    完全不同的事；把它俩混成同一个 0，界面就会把「无观测」画成「完美」。
 * 2. **NaN 会污染比较**。`std::min_element` / `<` 遇到 NaN 的结果不确定，
 *    且服务端本来就没用 NaN。
 *
 * 因此这里沿用服务端自己的哨兵语义，不做第二套约定：
 * `server/include/net_info.hpp:237` 写的是
 * `tcp_loss_rate_ = -1.0 表示未测量`，`database_manager.cpp` 用 `-1000`
 * 表示 rssi 缺失。解析层只是把 JSON 的 `null` 翻译成同一套哨兵。
 */
constexpr double kNoRttMs = -1.0;
constexpr double kNoJitterMs = -1.0;
constexpr double kNoRssiDbm = -1000.0;
constexpr double kNoTcpLossPct = -1.0;

/// 某指标的取值是否为「未测量」哨兵
bool is_history_metric_absent(HistoryMetric metric, double value);

/**
 * @brief 历史趋势的单个数据点
 *
 * 对应 `GetHistory` 返回的数组元素。
 *
 * 字段名依据 `server/src/database_manager.cpp` 的 `queryHistory()` 实际输出。
 * 注意 `ts` 是 **字符串** 而不是数字（SQLite 里按 TEXT 存），形如
 * `"2026-11-05T13:27:00"`（`currentTimestamp()` 用 `%Y-%m-%dT%H:%M:%S`，
 * 带 T 分隔；此处早先注释写成空格分隔是错的，真机样本已确认）。
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

    /// 往返时延（毫秒）；`kNoRttMs` 表示未测量
    double rtt_ms = kNoRttMs;

    /// 抖动（毫秒）；`kNoJitterMs` 表示未测量
    double jitter_ms = kNoJitterMs;

    /// RSSI（dBm，负值）；`kNoRssiDbm` 表示未测量
    double rssi_dbm = kNoRssiDbm;

    /// TCP 丢包率（**百分比**，0-100）；`kNoTcpLossPct` 表示未测量
    ///
    /// 单位是 % 不是 0-1 比率，依据 `server/include/net_info.hpp:237` 的
    /// `tcp_loss_rate_（%）` 以及 `reliability_evaluator.hpp:13-14` 的
    /// 阈值 `tcp_loss_good_max{1.0}` / `tcp_loss_bad_min{4.0}`（都是百分数）。
    double tcp_loss = kNoTcpLossPct;

    /// 取某指标的值（未测量时返回对应哨兵，不返回 0）
    double value(HistoryMetric metric) const;

    /// 某指标在此点是否有有效值
    bool has(HistoryMetric metric) const {
        return !is_history_metric_absent(metric, value(metric));
    }
};

/**
 * @brief 历史趋势序列
 *
 * 不携带网卡名：查询「全部网卡」时任意一条记录的 iface 都不代表整个序列，
 * 请求侧的网卡由调用方（MainWindow::interface_）持有。
 */
struct HistorySeries {
    std::vector<HistoryPoint> points;

    /// 该序列里某指标的有效点数量（哨兵值不计）
    int count_with(HistoryMetric metric) const;
};

/**
 * @brief 指标的显示名（中文，用于图标题与下拉框）
 */
const char* history_metric_label(HistoryMetric metric);

/**
 * @brief 指标的单位后缀（如 "ms" / "dBm" / "%"），无量纲返回空串
 */
const char* history_metric_unit(HistoryMetric metric);

/**
 * @brief Y 轴的建议取值范围 {lo, hi}
 *
 * ## 为什么不让图表一律按数据自适应
 *
 * 分数 0-100 自适应是对的：它本身就是归一化量纲，看相对起伏即可。
 *
 * 但 RSSI 不行。真机 RSSI 常在 -40 到 -70 dBm 之间波动，若按数据自适应，
 * -45 与 -50 会被拉满整个绘图区，看上去像断崖。而这两个值的实际差别
 * 在工程上几乎无意义（都属于「信号很好」）。对这类有绝对含义的指标，
 * 必须钉住一个物理上有意义的范围，让「波动幅度」看起来就是它本来的大小。
 *
 * 同理 RTT：自适应会把 25ms↔30ms 放大成满屏落差，而现场关心的是
 * 「有没有超过 100ms」这个量级。
 *
 * @return 固定范围；返回 {0, 0} 表示调用方应按数据自适应（仅 Score）
 */
struct HistoryRange {
    double lo = 0.0;
    double hi = 0.0;
    bool fixed = false;
};

HistoryRange history_metric_range(HistoryMetric metric);

/**
 * @brief 现场终端的三个时间窗
 *
 * ## 为什么是 1h / 6h / 13h
 *
 * 上限硬约束：服务端 `kMaxHistoryLimit = 10000`
 * （`server/src/dbus_service.cpp:1515`），落库周期 5 秒（见下方
 * `kHistorySampleIntervalSeconds`），因此最长连续覆盖 = 10000 × 5s
 * ≈ 13.89 小时。
 *
 * 13h 需要 9360 点，在上限内且留了 640 点余量；6h 需 4320 点；1h 需 720 点。
 *
 * ## 为什么原本写的是 24h，以及为什么改掉
 *
 * 产品定位 v1.2 **最初**的白名单写的是「1h / 6h / 24h」（2026-11-08 已同步
 * 订正为 1h / 6h / 13h）。但 24h 按 5 秒落库需要 17280 点，**超出上限 73%**，
 * 服务端会静默钳到 10000 条：标签写 24h、图里只有 13.9h。板端实测已确认
 * 这个钳制行为（请求 7 天 / limit=20000 时恰好返回 10000 条、跨度 69.99h）。
 *
 * 挂一个必然说谎的 24h 按钮，比少一个窗口更坏——现场会据此判断「过去一天
 * 都正常」，而实际上那 10 小时压根没被看到。因此把最大窗口定在服务端能力
 * 边界内的 13h，并同步订正产品定位与权责契约文档。
 *
 * > 这不是「能力退化」，而是把假能力换成真能力：13h 全量可见 > 24h 只看到 14h。
 *
 * ## 为什么窗口定义放在数据层
 *
 * 「13h 窗口要请求多少个点」是一个**可验证的推算**，不是 UI 的风格选择：
 * 它由落库周期和服务端 limit 上限共同决定。放在 UI 里就只能靠
 * 人肉 review，放在这里可以单测——尤其是「不超过 10000」这条硬约束。
 */
enum class HistoryWindow {
    Last1h,
    Last6h,
    Last13h,
};

/**
 * @brief 服务端写库周期（秒）
 *
 * ## 这个值不是「采集间隔」，别再把两者混为一谈
 *
 * 板上同时存在三个不同的周期，很容易搞混：
 *
 * | 周期 | 值 | 出处 |
 * | --- | --- | --- |
 * | 各监控器采集 | 3~30 秒不等（多为 10 秒） | `config.yaml` 的 `interval_ms` |
 * | **网络历史落库** | **5 秒（硬编码）** | `server/src/server.cpp` `start_history_persistence_thread()` |
 * | 看板请求点数 | 窗口秒数 / 本常量 | 本文件 |
 *
 * 本常量必须等于**第二行**，因为 `network_history` 表里的行是落库线程写的，
 * 与监控器多久采一次无关。
 *
 * ## 踩过的坑
 *
 * 初版把这个常量当成 10（依据 CLAUDE.md「数据采集间隔：10 秒」），
 * 于是 24h 只请求 8640 条。而板端实测（2026-11-08，直调 `libweaknet`
 * 取 20 分钟样本）相邻记录间隔中位数 **5.0 秒**、最大 6.0 秒。
 * 服务端 `ORDER BY ts DESC LIMIT ?` 先倒序再截断，点给少了一半，
 * 24h 窗口实际只画到约 12 小时——标签与图形不一致。
 */
constexpr int kHistorySampleIntervalSeconds = 5;

/// 该窗口覆盖的秒数
int history_window_seconds(HistoryWindow window);

/**
 * @brief 该窗口应请求的条数
 *
 * = 窗口秒数 / 落库周期。三个窗口分别得到 720 / 4320 / 9360，
 * **全部低于服务端 10000 上限**——这是本函数存在的主要理由，
 * 单测会守住这条不变量，防止有人加窗口时忘记算。
 */
int history_window_limit(HistoryWindow window);

/// 界面标签（如 "1 小时"）
const char* history_window_label(HistoryWindow window);

/**
 * @brief 历史查询的起始时间串
 *
 * @param window 时间窗
 * @param now_epoch_seconds 当前时间（Unix 秒）。**显式传入而不是内部取时钟**：
 *        这样才能对结果做断言，否则单测只能验证「格式像」，验证不了「真的是 1 小时前」。
 * @return 形如 "2026-11-08T09:30:00" 的时间串（**本地时间**，与
 *         `database_manager.cpp` 的 `currentTimestamp()` 用 `localtime_r` 一致）。
 *         若换算失败返回空串，调用方应退化成「不限时间」。
 */
std::string history_window_start(HistoryWindow window, long long now_epoch_seconds);

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

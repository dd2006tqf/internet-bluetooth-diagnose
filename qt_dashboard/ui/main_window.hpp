/**
 * @file main_window.hpp
 * @brief 看板主窗口
 *
 * ## 布局
 *
 * ```
 * ┌─────────────────────────────────────────────┐
 * │ [刷新] wlan0  GOOD 85.2  2026-11-05 20:31   │  ← 顶栏
 * ├──────────────┬──────────────────────────────┤
 * │ 网络健康      │                              │
 * │  IP 可达  ●  │      历史趋势折线图            │
 * │  响应性   ●  │                              │
 * │  可靠性   ●  │                              │
 * │  RF 健康  ○  │                              │
 * ├──────────────┤                              │
 * │ 服务健康      │                              │
 * │  DNS      ●  │                              │
 * │  TCP 连接 ●  │                              │
 * │  ...         │                              │
 * ├──────────────┴──────────────────────────────┤
 * │ 诊断：故障域 DNS · 首要问题 dns_timeout      │  ← 底栏
 * └─────────────────────────────────────────────┘
 * ```
 *
 * ## 状态色的取值依据
 *
 * `server/include/assurance/health_state.hpp:15-23`：
 * GOOD / DEGRADED / BAD / UNKNOWN。
 */

#ifndef WEAKNET_QT_DASHBOARD_MAIN_WINDOW_HPP
#define WEAKNET_QT_DASHBOARD_MAIN_WINDOW_HPP

#include <QByteArray>
#include <QMainWindow>
#include <QString>

#include "data/dashboard_data.hpp"

class QComboBox;
class QHBoxLayout;
class QLabel;
class QPushButton;

namespace weaknet {
namespace dashboard {

class DataFetchThread;
class LineChartWidget;

/**
 * @brief 看板主窗口
 */
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    /**
     * @brief 启动后台取数与自动刷新
     *
     * 不从构造函数调用：构造时窗口还没显示，立刻取数会拖慢启动。
     */
    void start();

    /**
     * @brief 指定历史趋势查询的网卡，并据此设置图表标题
     *
     * 空字符串表示不限制（服务端按全部网卡返回），标题会写成「全部网卡」。
     *
     * 必须在 `start()` 之前调用。`main.cpp` **无条件调用**（不传 `-i` 时传空串）：
     * 它同时把网卡与当前时间窗同步给取数线程并重算标题，跳过调用会让
     * 标题与实际查询范围对不上。
     */
    void setInterface(const QString& interface_name);

    // -----------------------------------------------------------------------
    // 展示态的可观测面
    //
    // 这两个是**只读访问器**，给测试与将来可能的持久化用。
    // 不提供 setter：所有写入路径都必须走 onMetricSelected() /
    // onWindowSelected()，那两个函数里带着「切换不能重置另一维」的约束。
    // 一旦开了 setter，这个约束就没有守卫了。
    // -----------------------------------------------------------------------

    /// 当前绘制的指标
    HistoryMetric current_metric() const { return current_metric_; }

    /// 当前查询的时间窗
    HistoryWindow current_window() const { return current_window_; }

private slots:
    void onExperienceReady(const QByteArray& json);
    void onDiagnosisReady(const QByteArray& json);
    void onHistoryReady(const QByteArray& json);
    void onFetchFailed(const QString& stage, const QString& error);
    void onRefreshFinished();
    void onRefreshClicked();

    /// 指标选择器变更（白名单第 1 项）
    ///
    /// **只换视图，不重置时间窗**：用户先选「13 小时」再切到 RSSI，
    /// 期望看的仍然是 13 小时的 RSSI。
    void onMetricSelected(int index);

    /// 时间窗按钮点击（白名单第 2 项）
    ///
    /// **只换取数范围，不重置指标**，理由同上。
    void onWindowSelected(int window_id);

    /// 导出当前画面为 PNG（白名单第 3 项）
    void onExportClicked();

private:
    void build_ui();
    /// 顶栏右侧的展示态控制区（指标选择器 + 时间窗按钮 + 导出）
    void build_view_controls(QHBoxLayout* top);
    void apply_experience(const ExperienceSnapshot& snap);
    void apply_diagnosis(const DiagnosisSnapshot& snap);
    void update_status_label(const SleItem& item, QLabel* state_label,
                             QLabel* dot_label);
    static QString state_color(const QString& state);

    /// 重算图标题（网卡 + 指标 + 窗口），并把它交给 LineChartWidget
    void refresh_chart_title();

    /// 把当前画面存成 PNG，返回实际写入路径（失败为空）
    QString export_screenshot();

    DataFetchThread* fetcher_ = nullptr;
    LineChartWidget* chart_ = nullptr;

    /// 历史查询使用的网卡（空表示全部）。
    ///
    /// 这是**请求侧**的事实，不能从 `HistorySeries` 推断：
    /// 查询全部网卡时，任意一条记录的 iface 都不代表整个序列。
    /// 折线图标题直接用它，避免出现「标题写着 wlan0，实际混了 eth0」。
    QString interface_;

    // 顶栏
    QPushButton* refresh_button_ = nullptr;
    QLabel* iface_label_ = nullptr;
    QLabel* score_label_ = nullptr;
    QLabel* timestamp_label_ = nullptr;

    // -----------------------------------------------------------------------
    // 展示态控制（现场交互终端白名单的落地点）
    //
    // 产品定位 v1.2 允许现场改的只有「展示态」：看哪个指标、看多长时间、
    // 把当前画面导出。这三个控件就是那三项，**一个不多**。
    //
    // 绝对不在这里加的东西（违反白名单会直接越过权责契约）：
    //   - 触发 pending_actions / 执行动作
    //   - 改阈值、改配置
    //   - 重启服务、重连
    //   - 任何“就地算一个结论”的按钮
    //
    // 新增交互点前先读 `docs/产品定位-工业无线诊断网关.md` 第三节之二。
    // -----------------------------------------------------------------------

    /// 趋势指标选择器（白名单第 1 项）
    QComboBox* metric_combo_ = nullptr;

    /// 时间窗按钮（白名单第 2 项）：1h / 6h / 13h，互斥
    ///
    /// 最大窗口是 13h 不是 24h：服务端 `kMaxHistoryLimit = 10000` ×
    /// 落库 5 秒 ≈ 13.89h，24h 会被静默钳制。见 `data/dashboard_data.hpp`。
    QPushButton* window_1h_button_ = nullptr;
    QPushButton* window_6h_button_ = nullptr;
    QPushButton* window_13h_button_ = nullptr;

    /// 截图导出（白名单第 3 项）
    QPushButton* export_button_ = nullptr;

    /// 当前展示的指标与时间窗。切换时二者互不干扰：
    /// 换窗口不能把指标重置，换指标也不能把窗口重置。
    HistoryMetric current_metric_ = HistoryMetric::Score;
    HistoryWindow current_window_ = HistoryWindow::Last1h;

    /// 各维度状态汇总「● 3 ● 2 ● 1 ○ 7」（count_by_state 的消费者）
    QLabel* summary_label_ = nullptr;
    /// 评估档位与覆盖度（assessment_profile 的消费者）
    QLabel* profile_label_ = nullptr;

    // 维度状态标签（顺序与 dashboard_data.hpp 中字段一致）
    QLabel* ip_reachability_label_ = nullptr;
    QLabel* responsiveness_label_ = nullptr;
    QLabel* reliability_label_ = nullptr;
    QLabel* rf_health_label_ = nullptr;

    QLabel* dns_label_ = nullptr;
    QLabel* tcp_connect_label_ = nullptr;
    QLabel* tcp_rtt_label_ = nullptr;
    QLabel* http_access_label_ = nullptr;
    QLabel* captive_portal_label_ = nullptr;
    QLabel* active_dns_label_ = nullptr;
    QLabel* active_tcp_label_ = nullptr;
    QLabel* active_https_label_ = nullptr;
    QLabel* active_portal_label_ = nullptr;

    // 每个维度左侧的状态圆点。
    //
    // 圆点与状态文本分别持有指针：`update_status_label()` 要在一次调用里
    // 同时给两者上色。改进前圆点由 `make_status_row()` 创建后就没再被引用，
    // 永远保持默认前景色，14 行扫下来看不出哪几项异常。
    QLabel* ip_reachability_dot_ = nullptr;
    QLabel* responsiveness_dot_ = nullptr;
    QLabel* reliability_dot_ = nullptr;
    QLabel* rf_health_dot_ = nullptr;

    QLabel* dns_dot_ = nullptr;
    QLabel* tcp_connect_dot_ = nullptr;
    QLabel* tcp_rtt_dot_ = nullptr;
    QLabel* http_access_dot_ = nullptr;
    QLabel* captive_portal_dot_ = nullptr;
    QLabel* active_dns_dot_ = nullptr;
    QLabel* active_tcp_dot_ = nullptr;
    QLabel* active_https_dot_ = nullptr;
    QLabel* active_portal_dot_ = nullptr;

    // 底栏
    //
    // experience 与 diagnosis 是**两个独立的异步数据源**，返回时序不定
    // （DataFetchThread 先发 experienceReady 再发 diagnosisReady，但二者
    // 都是队列连接，实际到达顺序不保证）。因此它们各自拥有独立控件：
    // 共用一个 QLabel 会让后到者覆盖先到者的内容。
    /// 体验快照的结论行：等待评估原因 / 首要问题（experience 独占）
    QLabel* experience_label_ = nullptr;
    /// 体验快照的警告（experience 独占，无警告时隐藏）
    QLabel* experience_detail_label_ = nullptr;
    /// 诊断结论行：故障域 + 置信度 + 动作数（diagnosis 独占）
    QLabel* diagnosis_label_ = nullptr;
    /// 诊断的人读摘要（diagnosis 独占，default_summary_template 的消费者）
    QLabel* summary_template_label_ = nullptr;
    /// 次要问题 / 已排除原因 / 证据（diagnosis 独占，可折叠）
    QLabel* diagnosis_extra_label_ = nullptr;
    QLabel* status_bar_label_ = nullptr;

    /// 本轮刷新是否出现过失败。
    ///
    /// 不能用「状态栏文字是否以『取数失败』开头」判断：那是跨轮持久的状态，
    /// 一旦出现过失败，之后即使每轮都成功也永远不会复位回「就绪」。
    /// 这里按轮记录，onRefreshFinished 时消费并清空。
    bool round_had_failure_ = false;

    bool refresh_in_flight_ = false;
};

} // namespace dashboard
} // namespace weaknet

#endif // WEAKNET_QT_DASHBOARD_MAIN_WINDOW_HPP

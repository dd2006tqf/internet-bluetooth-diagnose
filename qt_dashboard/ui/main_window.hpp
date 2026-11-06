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
     * @brief 指定历史趋势查询的网卡
     *
     * 空字符串表示不限制（服务端按全部网卡返回）。
     * 必须在 start() 之前调用，否则首轮取数用的是默认值。
     */
    void setInterface(const QString& interface_name);

private slots:
    void onExperienceReady(const QByteArray& json);
    void onDiagnosisReady(const QByteArray& json);
    void onHistoryReady(const QByteArray& json);
    void onFetchFailed(const QString& stage, const QString& error);
    void onRefreshFinished();
    void onRefreshClicked();

private:
    void build_ui();
    void apply_experience(const ExperienceSnapshot& snap);
    void apply_diagnosis(const DiagnosisSnapshot& snap);
    void update_status_label(const SleItem& item, QLabel* state_label);
    static QString state_color(const QString& state);

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

    bool refresh_in_flight_ = false;
};

} // namespace dashboard
} // namespace weaknet

#endif // WEAKNET_QT_DASHBOARD_MAIN_WINDOW_HPP

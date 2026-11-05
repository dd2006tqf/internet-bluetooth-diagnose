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
    void update_status_label(const SleItem& item, QLabel* state_label);
    static QString state_color(const QString& state);

    DataFetchThread* fetcher_ = nullptr;
    LineChartWidget* chart_ = nullptr;

    // 顶栏
    QPushButton* refresh_button_ = nullptr;
    QLabel* iface_label_ = nullptr;
    QLabel* score_label_ = nullptr;
    QLabel* timestamp_label_ = nullptr;

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
    QLabel* diagnosis_label_ = nullptr;
    QLabel* status_bar_label_ = nullptr;

    bool refresh_in_flight_ = false;
};

} // namespace dashboard
} // namespace weaknet

#endif // WEAKNET_QT_DASHBOARD_MAIN_WINDOW_HPP

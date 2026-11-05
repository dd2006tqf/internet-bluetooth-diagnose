/**
 * @file main_window.cpp
 * @brief MainWindow 实现
 */

#include "main_window.hpp"

#include <QDateTime>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

#include "data/dashboard_data.hpp"
#include "data_fetch_thread.hpp"
#include "line_chart_widget.hpp"

namespace weaknet {
namespace dashboard {

namespace {

/**
 * @brief 自动刷新间隔
 *
 * 服务端采集间隔是 10 秒（CLAUDE.md「数据采集间隔：10 秒」），
 * 看板刷新快于采集没有意义，只会白耗 D-Bus。这里取 10 秒对齐。
 */
constexpr int kAutoRefreshMs = 10000;

/// 历史趋势默认取多少点
constexpr int kHistoryLimit = 120;

/**
 * @brief 创建一行「名称 + 状态点 + 状态文本」
 *
 * @param name        维度中文名
 * @param state_label 输出：状态标签控件
 * @return 该行的布局
 */
QWidget* make_status_row(const QString& name, QLabel** state_label) {
    auto* row = new QWidget;
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(4, 2, 4, 2);

    auto* name_label = new QLabel(name);
    name_label->setMinimumWidth(90);

    auto* dot = new QLabel(QStringLiteral("●"));
    auto* state = new QLabel(QStringLiteral("--"));
    state->setObjectName(QStringLiteral("state_text"));

    layout->addWidget(name_label);
    layout->addWidget(dot);
    layout->addWidget(state);
    layout->addStretch();

    *state_label = state;
    return row;
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("WeakNet 诊断看板"));
    resize(1024, 700);
    build_ui();

    fetcher_ = new DataFetchThread(this);

    connect(fetcher_, &DataFetchThread::experienceReady,
            this, &MainWindow::onExperienceReady);
    connect(fetcher_, &DataFetchThread::diagnosisReady,
            this, &MainWindow::onDiagnosisReady);
    connect(fetcher_, &DataFetchThread::historyReady,
            this, &MainWindow::onHistoryReady);
    connect(fetcher_, &DataFetchThread::fetchFailed,
            this, &MainWindow::onFetchFailed);
    connect(fetcher_, &DataFetchThread::refreshFinished,
            this, &MainWindow::onRefreshFinished);

    // 自动刷新：QTimer 在主线程，触发时只是给后台线程打个标记，不会阻塞
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this]() {
        if (!refresh_in_flight_) {
            fetcher_->requestRefresh();
        }
    });
    timer->start(kAutoRefreshMs);
}

MainWindow::~MainWindow() = default;

void MainWindow::start() {
    fetcher_->start();
    fetcher_->requestRefresh();
    refresh_in_flight_ = true;
}

void MainWindow::build_ui() {
    auto* central = new QWidget;
    auto* root = new QVBoxLayout(central);

    // --- 顶栏 ---
    auto* top = new QHBoxLayout;

    refresh_button_ = new QPushButton(QStringLiteral("刷新"));
    connect(refresh_button_, &QPushButton::clicked,
            this, &MainWindow::onRefreshClicked);
    top->addWidget(refresh_button_);

    iface_label_ = new QLabel(QStringLiteral("--"));
    QFont iface_font = iface_label_->font();
    iface_font.setBold(true);
    iface_label_->setFont(iface_font);
    top->addWidget(iface_label_);

    score_label_ = new QLabel(QStringLiteral("--"));
    QFont score_font = score_label_->font();
    score_font.setPointSizeF(score_font.pointSizeF() * 1.8);
    score_font.setBold(true);
    score_label_->setFont(score_font);
    top->addWidget(score_label_);

    top->addStretch();

    timestamp_label_ = new QLabel(QStringLiteral("--"));
    top->addWidget(timestamp_label_);

    root->addLayout(top);

    // --- 主区：左维度列表 / 右趋势图 ---
    auto* splitter = new QSplitter(Qt::Horizontal);

    // 左侧：两个分组
    auto* left = new QWidget;
    auto* left_layout = new QVBoxLayout(left);

    auto* net_group = new QGroupBox(QStringLiteral("网络健康"));
    auto* net_layout = new QVBoxLayout(net_group);
    net_layout->addWidget(make_status_row(QStringLiteral("IP 可达"), &ip_reachability_label_));
    net_layout->addWidget(make_status_row(QStringLiteral("响应性"), &responsiveness_label_));
    net_layout->addWidget(make_status_row(QStringLiteral("可靠性"), &reliability_label_));
    net_layout->addWidget(make_status_row(QStringLiteral("RF 健康"), &rf_health_label_));
    net_layout->addStretch();
    left_layout->addWidget(net_group);

    auto* svc_group = new QGroupBox(QStringLiteral("服务健康"));
    auto* svc_layout = new QVBoxLayout(svc_group);
    svc_layout->addWidget(make_status_row(QStringLiteral("DNS"), &dns_label_));
    svc_layout->addWidget(make_status_row(QStringLiteral("TCP 连接"), &tcp_connect_label_));
    svc_layout->addWidget(make_status_row(QStringLiteral("TCP RTT"), &tcp_rtt_label_));
    svc_layout->addWidget(make_status_row(QStringLiteral("HTTP 访问"), &http_access_label_));
    svc_layout->addWidget(make_status_row(QStringLiteral("Portal 检测"), &captive_portal_label_));
    svc_layout->addWidget(make_status_row(QStringLiteral("主动 DNS"), &active_dns_label_));
    svc_layout->addWidget(make_status_row(QStringLiteral("主动 TCP"), &active_tcp_label_));
    svc_layout->addWidget(make_status_row(QStringLiteral("主动 HTTPS"), &active_https_label_));
    svc_layout->addWidget(make_status_row(QStringLiteral("主动 Portal"), &active_portal_label_));
    svc_layout->addStretch();
    left_layout->addWidget(svc_group);

    splitter->addWidget(left);

    // 右侧：趋势图
    chart_ = new LineChartWidget;
    chart_->setTitle(QStringLiteral("历史趋势（分数）"));
    splitter->addWidget(chart_);

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    root->addWidget(splitter, 1);

    // --- 底栏：诊断结论 ---
    diagnosis_label_ = new QLabel(QStringLiteral("诊断：等待数据…"));
    diagnosis_label_->setWordWrap(true);
    root->addWidget(diagnosis_label_);

    setCentralWidget(central);

    status_bar_label_ = new QLabel(QStringLiteral("就绪"));
    statusBar()->addWidget(status_bar_label_);
}

// ---------------------------------------------------------------------------
// 槽函数
// ---------------------------------------------------------------------------

void MainWindow::onRefreshClicked() {
    if (refresh_in_flight_) {
        return;
    }
    refresh_in_flight_ = true;
    status_bar_label_->setText(QStringLiteral("刷新中…"));
    fetcher_->requestRefresh();
}

void MainWindow::onExperienceReady(const QByteArray& json) {
    const ExperienceSnapshot snap =
        parse_experience(std::string(json.constData(),
                                     static_cast<size_t>(json.size())));
    apply_experience(snap);
}

void MainWindow::onDiagnosisReady(const QByteArray& json) {
    const DiagnosisSnapshot snap =
        parse_diagnosis(std::string(json.constData(),
                                    static_cast<size_t>(json.size())));

    if (!snap.valid) {
        diagnosis_label_->setText(QStringLiteral("诊断：数据无效"));
        return;
    }

    QString text = QStringLiteral("诊断：故障域 %1 · 首要问题 %2 · 置信度 %3")
                       .arg(QString::fromStdString(snap.fault_domain),
                            QString::fromStdString(snap.primary_issue),
                            QString::fromStdString(snap.confidence));

    if (!snap.actions.empty()) {
        text += QStringLiteral(" · 建议动作 %1 项")
                    .arg(static_cast<int>(snap.actions.size()));
    }
    diagnosis_label_->setText(text);
}

void MainWindow::onHistoryReady(const QByteArray& json) {
    const HistorySeries series =
        parse_history(std::string(json.constData(),
                                  static_cast<size_t>(json.size())));
    chart_->setSeries(series);
}

void MainWindow::onFetchFailed(const QString& stage, const QString& error) {
    // 只在状态栏提示，不弹窗：自动刷新时弹窗会烦人
    status_bar_label_->setText(
        QStringLiteral("取数失败（%1）：%2").arg(stage, error));
}

void MainWindow::onRefreshFinished() {
    refresh_in_flight_ = false;
    if (!status_bar_label_->text().startsWith(QStringLiteral("取数失败"))) {
        status_bar_label_->setText(QStringLiteral("就绪"));
    }
}

// ---------------------------------------------------------------------------
// 渲染
// ---------------------------------------------------------------------------

void MainWindow::apply_experience(const ExperienceSnapshot& snap) {
    timestamp_label_->setText(
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));

    if (!snap.available()) {
        // 服务端明确说明「还没有评估」或「快照过期」——如实展示，不显示 0 分
        iface_label_->setText(QString::fromStdString(snap.interface_name));
        score_label_->setText(QStringLiteral("--"));
        score_label_->setStyleSheet(QString());
        diagnosis_label_->setText(
            QStringLiteral("等待评估：%1")
                .arg(QString::fromStdString(snap.unavailable_reason)));
        return;
    }

    iface_label_->setText(QString::fromStdString(snap.interface_name));
    score_label_->setText(QString::number(snap.display_score, 'f', 1));

    // 整体分数按状态着色
    const QString color = state_color(QString::fromStdString(snap.overall_state));
    score_label_->setStyleSheet(QStringLiteral("color: %1;").arg(color));

    update_status_label(snap.ip_reachability, ip_reachability_label_);
    update_status_label(snap.responsiveness, responsiveness_label_);
    update_status_label(snap.reliability, reliability_label_);
    update_status_label(snap.rf_health, rf_health_label_);

    update_status_label(snap.dns_service, dns_label_);
    update_status_label(snap.tcp_connect, tcp_connect_label_);
    update_status_label(snap.tcp_rtt, tcp_rtt_label_);
    update_status_label(snap.http_access, http_access_label_);
    update_status_label(snap.captive_portal, captive_portal_label_);
    update_status_label(snap.active_dns, active_dns_label_);
    update_status_label(snap.active_tcp, active_tcp_label_);
    update_status_label(snap.active_https, active_https_label_);
    update_status_label(snap.active_portal, active_portal_label_);
}

void MainWindow::update_status_label(const SleItem& item, QLabel* state_label) {
    if (!state_label) {
        return;
    }

    // 不适用的维度（如有线链路下的 RF 健康）显式标注，不显示 GOOD/BAD
    if (!item.applicable()) {
        state_label->setText(QStringLiteral("不适用"));
        state_label->setStyleSheet(QStringLiteral("color: #9E9E9E;"));
        return;
    }

    if (!item.has_value()) {
        state_label->setText(QStringLiteral("未知"));
        state_label->setStyleSheet(QStringLiteral("color: #9E9E9E;"));
        return;
    }

    // 部分覆盖的维度加后缀，提示结论置信度较低
    QString text = QString::fromStdString(item.state);
    if (item.coverage == "PARTIAL") {
        text += QStringLiteral(" (部分)");
    }
    state_label->setText(text);
    state_label->setStyleSheet(
        QStringLiteral("color: %1;").arg(state_color(QString::fromStdString(item.state))));
}

QString MainWindow::state_color(const QString& state) {
    // 取值依据 server/include/assurance/health_state.hpp:15-23
    if (state == QLatin1String("GOOD")) {
        return QStringLiteral("#2E7D32");      // 绿
    }
    if (state == QLatin1String("DEGRADED")) {
        return QStringLiteral("#F9A825");      // 黄
    }
    if (state == QLatin1String("BAD")) {
        return QStringLiteral("#C62828");      // 红
    }
    return QStringLiteral("#9E9E9E");          // 灰（UNKNOWN）
}

} // namespace dashboard
} // namespace weaknet

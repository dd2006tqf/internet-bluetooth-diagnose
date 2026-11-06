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
#include <QStringList>
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

void MainWindow::setInterface(const QString& interface_name) {
    // 历史查询的网卡过滤在后台线程里读取，这里同步进去
    interface_ = interface_name;
    fetcher_->setHistoryQuery(interface_name, kHistoryLimit);

    // 标题只用请求侧的网卡：查询全部网卡时序列里混着多个 iface，
    // 取数据里任意一条都会把标题写错。
    chart_->setTitle(interface_.isEmpty()
                         ? QStringLiteral("历史趋势（分数，全部网卡）")
                         : QStringLiteral("历史趋势（分数，%1）").arg(interface_));
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

    // 各维度状态汇总。左侧维度列表只能逐行看，这里给出整体分布，
    // 一眼看出「有几个 BAD」比逐行扫快得多。
    summary_label_ = new QLabel;
    top->addWidget(summary_label_);

    top->addStretch();

    // 评估档位：不同 profile 下同一维度的判定阈值不同，
    // 不标出来会让「同样是 GOOD」看起来可比，实际不可比。
    profile_label_ = new QLabel(QStringLiteral("--"));
    profile_label_->setStyleSheet(QStringLiteral("color: #666666;"));
    top->addWidget(profile_label_);

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
    // 标题由 setInterface() 统一设置（main.cpp 无条件调用）：
    // 这里不再给默认值，避免两处文案各自演化后对不上实际查询范围。
    splitter->addWidget(chart_);

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    root->addWidget(splitter, 1);

    // --- 底栏 ---
    // 每个 QLabel 只有一个所有者：写它的地方唯一，读的人随意。
    // 这样任一侧刷新都不会抹掉另一侧的内容。
    //
    // 归属：
    //   experience_label_          ← experience（等待评估 / 首要问题）
    //   experience_detail_label_   ← experience（警告，无警告时隐藏）
    //   diagnosis_label_           ← diagnosis （故障域 + 置信度 + 动作数）
    //   summary_template_label_    ← diagnosis （人读摘要）
    //   diagnosis_extra_label_     ← diagnosis （次要问题 / 排除原因 / 证据）
    experience_label_ = new QLabel(QStringLiteral("体验：等待数据…"));
    experience_label_->setWordWrap(true);
    root->addWidget(experience_label_);

    // 体验警告（experience 独占）：服务端主动标记的异常，红色单独一行，
    // 不与 experience_label_ 抢位置——两者会同时有内容
    experience_detail_label_ = new QLabel;
    experience_detail_label_->setWordWrap(true);
    experience_detail_label_->setStyleSheet(QStringLiteral("color: #C62828;"));
    experience_detail_label_->hide();
    root->addWidget(experience_detail_label_);

    diagnosis_label_ = new QLabel(QStringLiteral("诊断：等待数据…"));
    diagnosis_label_->setWordWrap(true);
    root->addWidget(diagnosis_label_);

    // 诊断的人读摘要（diagnosis 独占）：服务端给的是完整句子，
    // 比 primary_issue 这类机器 token 更适合直接展示给用户
    summary_template_label_ = new QLabel;
    summary_template_label_->setWordWrap(true);
    summary_template_label_->hide();
    root->addWidget(summary_template_label_);

    // 次要问题 / 已排除原因 / 证据（diagnosis 独占，默认折叠）
    diagnosis_extra_label_ = new QLabel;
    diagnosis_extra_label_->setWordWrap(true);
    diagnosis_extra_label_->setStyleSheet(QStringLiteral("color: #666666;"));
    diagnosis_extra_label_->hide();
    root->addWidget(diagnosis_extra_label_);

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
    apply_diagnosis(snap);
}

void MainWindow::apply_diagnosis(const DiagnosisSnapshot& snap) {
    if (!snap.valid) {
        diagnosis_label_->setText(QStringLiteral("诊断：数据无效"));
        summary_template_label_->hide();
        diagnosis_extra_label_->hide();
        return;
    }

    // 第一行：机器可判定的结论（故障域 + 置信度）
    // 注意 fault_domain == "NONE" 表示服务端明确判定「无需排查」
    // （见 diagnosis_engine.hpp:135 的 all_metrics_healthy 分支），
    // 不是「不知道」，所以照样展示，但把置信度放前面更醒目。
    QString text = QStringLiteral("诊断：故障域 %1 · 置信度 %2")
                       .arg(QString::fromStdString(snap.fault_domain),
                            QString::fromStdString(snap.confidence));

    if (!snap.actions.empty()) {
        // 动作只提示数量与最优先级别，不展示 action_id：
        // 看板不执行任何动作，真正执行必须走 weaknet_execute_action() 且需 root。
        int top_priority = snap.actions.front().priority;
        for (const ActionSuggestion& action : snap.actions) {
            top_priority = std::min(top_priority, action.priority);
        }
        text += QStringLiteral(" · 建议动作 %1 项（最高优先级 %2）")
                    .arg(static_cast<int>(snap.actions.size()))
                    .arg(top_priority);
    }
    diagnosis_label_->setText(text);

    // 第二行：人读摘要。default_summary_template 是完整句子，
    // 比 primary_issue 这类机器 token 更适合直接呈现给用户。
    //
    // primary_issue 作为兜底：服务端若没给 default_summary_template，
    // 这里就只剩 primary_issue 可选。丢掉它会让整行隐藏，而"没有摘要"
    // 和"没有诊断结论"在界面上是同一个样子，信息就白解析了。
    if (!snap.summary_template.empty()) {
        summary_template_label_->setText(
            QString::fromStdString(snap.summary_template));
        summary_template_label_->show();
    } else if (!snap.primary_issue.empty()) {
        // 加前缀标明这是机器标识而非完整句子，避免被当成结论直接引用
        summary_template_label_->setText(
            QStringLiteral("首要问题 %1")
                .arg(QString::fromStdString(snap.primary_issue)));
        summary_template_label_->show();
    } else {
        summary_template_label_->hide();
    }

    // 第三行：次要问题 / 已排除原因 / 证据 ID。
    // 默认折叠成一行灰色小字；证据 ID（形如 ev_radxa_..._s101_01）是机器标识，
    // 铺在界面上纯噪声，放进 tooltip 让需要排查的人能查到即可。
    QStringList extra;
    QStringList evidence_tooltip;
    if (!snap.secondary_issues.empty()) {
        QStringList items;
        for (const std::string& issue : snap.secondary_issues) {
            items.append(QString::fromStdString(issue));
        }
        extra.append(QStringLiteral("次要问题 %1 项").arg(items.size()));
        evidence_tooltip.append(QStringLiteral("次要问题：") +
                                items.join(QStringLiteral("、")));
    }
    if (!snap.excluded_causes.empty()) {
        QStringList items;
        for (const ExcludedCause& cause : snap.excluded_causes) {
            items.append(QStringLiteral("%1（%2）")
                             .arg(QString::fromStdString(cause.cause),
                                  QString::fromStdString(cause.reason)));
        }
        extra.append(QStringLiteral("已排除 %1 项").arg(items.size()));
        evidence_tooltip.append(QStringLiteral("已排除原因：") +
                                items.join(QStringLiteral("、")));
    }
    if (!snap.evidence_refs.empty()) {
        QStringList items;
        for (const std::string& ref : snap.evidence_refs) {
            items.append(QString::fromStdString(ref));
        }
        extra.append(QStringLiteral("证据 %1 条").arg(items.size()));
        evidence_tooltip.append(QStringLiteral("证据 ID：") +
                                items.join(QStringLiteral("\n")));
    }

    if (extra.isEmpty()) {
        diagnosis_extra_label_->hide();
        diagnosis_extra_label_->setToolTip(QString());
    } else {
        diagnosis_extra_label_->setText(extra.join(QStringLiteral("　")));
        diagnosis_extra_label_->setToolTip(evidence_tooltip.join(QStringLiteral("\n")));
        diagnosis_extra_label_->show();
    }
}

void MainWindow::onHistoryReady(const QByteArray& json) {
    const HistorySeries series =
        parse_history(std::string(json.constData(),
                                  static_cast<size_t>(json.size())));
    chart_->setSeries(series);
}

void MainWindow::onFetchFailed(const QString& stage, const QString& error) {
    // 只在状态栏提示，不弹窗：自动刷新时弹窗会烦人
    round_had_failure_ = true;
    status_bar_label_->setText(
        QStringLiteral("取数失败（%1）：%2").arg(stage, error));
}

void MainWindow::onRefreshFinished() {
    refresh_in_flight_ = false;
    // 按轮复位：本轮全部成功才回到「就绪」。
    // 之前用「状态栏文字是否以『取数失败』开头」判断，那是跨轮持久状态——
    // 只要历史上失败过一次，后面每轮成功也永远停在失败文案上。
    if (!round_had_failure_) {
        status_bar_label_->setText(QStringLiteral("就绪"));
    }
    round_had_failure_ = false;
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
        summary_label_->clear();
        profile_label_->setText(QStringLiteral("--"));
        experience_detail_label_->hide();
        // 不清空 summary_template_label_ / diagnosis_extra_label_ / diagnosis_label_：
        // 它们归 diagnosis 所有，experience 无权处置。反过来也一样，
        // 这样任一侧晚到或失败都不会抹掉另一侧已经显示的内容。
        experience_label_->setText(
            QStringLiteral("体验：等待评估（%1）")
                .arg(QString::fromStdString(snap.unavailable_reason)));
        return;
    }

    iface_label_->setText(QString::fromStdString(snap.interface_name));
    score_label_->setText(QString::number(snap.display_score, 'f', 1));

    // 整体分数按状态着色
    const QString color = state_color(QString::fromStdString(snap.overall_state));
    score_label_->setStyleSheet(QStringLiteral("color: %1;").arg(color));

    // 评估档位：同一维度在不同 profile 下的判定阈值不同，必须标出来
    profile_label_->setText(QStringLiteral("档位 %1 / 覆盖 %2")
                                .arg(QString::fromStdString(snap.assessment_profile),
                                     QString::fromStdString(snap.overall_coverage)));

    // 各维度状态汇总：一眼看出有几个 BAD / DEGRADED，不必逐行扫
    int healthy = 0;
    int degraded = 0;
    int failed = 0;
    int unknown = 0;
    snap.count_by_state(&healthy, &degraded, &failed, &unknown);
    summary_label_->setText(
        QStringLiteral("<span style=\"color:#2E7D32\">● %1</span> "
                       "<span style=\"color:#F9A825\">● %2</span> "
                       "<span style=\"color:#C62828\">● %3</span> "
                       "<span style=\"color:#9E9E9E\">● %4</span>")
            .arg(healthy)
            .arg(degraded)
            .arg(failed)
            .arg(unknown));

    // 服务端给出的警告直接展示：这是它主动标记的异常，不该被界面吞掉
    if (!snap.warnings.empty()) {
        QStringList parts;
        parts.reserve(static_cast<int>(snap.warnings.size()));
        for (const std::string& warning : snap.warnings) {
            parts.append(QString::fromStdString(warning));
        }
        experience_detail_label_->setText(
            QStringLiteral("⚠ %1").arg(parts.join(QStringLiteral(" · "))));
        experience_detail_label_->show();
    } else {
        experience_detail_label_->hide();
    }

    // 首要问题并入结论行：experience_label_ 此时已被本函数独占写入，
    // 不会再被 diagnosis 侧覆盖，所以不需要第二个标签。
    QString text = QStringLiteral("体验：%1")
                       .arg(QString::fromStdString(snap.overall_state));
    if (!snap.primary_issue.empty()) {
        text += QStringLiteral(" · 首要问题 %1")
                    .arg(QString::fromStdString(snap.primary_issue));
    }
    experience_label_->setText(text);

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

    // reason 是服务端给出的人可读 token（如 jitter_excessive、no_dns_observations），
    // 它解释了「为什么是这个状态」。用 tooltip 挂在标签上，不占版面但可查。
    const QString reason = QString::fromStdString(item.reason);
    state_label->setToolTip(reason.isEmpty()
                                ? QString()
                                : QStringLiteral("%1：%2")
                                      .arg(QString::fromStdString(item.key), reason));

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

/**
 * @file main_window.cpp
 * @brief MainWindow 实现
 */

#include "main_window.hpp"

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QScroller>
#include <QSplitter>
#include <QStatusBar>
#include <QStorageInfo>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

#include "data/dashboard_data.hpp"
#include "data_fetch_thread.hpp"
#include "line_chart_widget.hpp"
#include "theme.hpp"
#include "video_fetch_thread.hpp"
#include "video_frame_widget.hpp"

namespace weaknet {
namespace dashboard {

namespace {

/**
 * @brief 自动刷新间隔
 *
 * 历史数据落库周期是 5 秒（`server/src/server.cpp` 的
 * `start_history_persistence_thread()` 硬编码，与 config.yaml 的
 * 各监控器 `interval_ms` 不是同一件事）。
 *
 * 这里取 **10 秒**而不是贴着 5 秒：一次刷新要打三次 D-Bus（experience /
 * diagnosis / history），24h→13h 满载时 history 单次就返回近 10000 行 JSON。
 * 每 5 秒重复拉一遍满载数据，换来的是「图上一分钟多两个点」——用户看不出差别，
 * 但 D-Bus 与 SQLite 的负载翻倍。10 秒是「比落库周期慢一档」的折中。
 */
constexpr int kAutoRefreshMs = 10000;

/// 截图导出的文件名前缀（后面接时间戳）
constexpr const char* kScreenshotPrefix = "weaknet-dashboard-";

/// 没有 U 盘时的兜底目录
///
/// 板端 rootfs 是只读挂载的（OTA 需要原子切换），所以**不能**写 `/` 或
/// 可执行文件所在目录。`/tmp` 挂在 tmpfs 上一定可写，但掉电即失。
/// 因此它是兜底而不是默认：状态栏会明确写出实际路径，让用户知道
/// 「这张图现在只在内存里」。
constexpr const char* kScreenshotFallbackDir = "/tmp";

/**
 * @brief 判断某个路径是不是一个真实可写的挂载点
 *
 * ## 为什么不能只看 `QStorageInfo::isReady()`
 *
 * `QStorageInfo` 对一个**普通目录**也会返回 `isReady() == true`，
 * 此时它的 `rootPath()` 是所属文件系统的挂载点（通常是 `/`）。
 * 实测（本机 `/media/ubuntu`、`/media/nomachine` 都只是 rootfs 上的
 * 空目录，没有挂载任何东西）：
 *
 * ```
 * /media/ubuntu   valid=1 ready=1 ro=0 root=/        fs=ext4
 * /dev/shm        valid=1 ready=1 ro=0 root=/dev/shm fs=tmpfs
 * ```
 *
 * 只看 isReady 会把 `/media/<user>` 这种空壳当成 U 盘，于是截图被写进
 * rootfs——而板端 rootfs 是为 OTA 原子切换准备的只读分区，写在那里
 * 既不持久（下次 OTA 就没了），也可能直接失败。
 *
 * 正确判据：**`rootPath()` 必须等于候选路径本身**，才说明它是挂载点。
 *
 * 另外排除 proc / sysfs / tmpfs 等伪文件系统：它们同样是「挂载点」，
 * 但不是 U 盘，不该被选为落盘位置。
 */
bool is_real_writable_mount(const QString& path) {
    if (path.isEmpty()) {
        return false;
    }

    QStorageInfo info(path);
    if (!info.isValid() || !info.isReady() || info.isReadOnly()) {
        return false;
    }

    // 必须本身是挂载点，而不是「位于某个挂载点之下」
    const QString root = info.rootPath();
    if (root.isEmpty()) {
        return false;
    }
    const QString normalized =
        QDir::cleanPath(QFileInfo(root).canonicalFilePath());
    const QString wanted = QDir::cleanPath(QFileInfo(path).canonicalFilePath());
    if (normalized.isEmpty() || wanted.isEmpty() || normalized != wanted) {
        return false;
    }

    // 排除伪文件系统：U 盘是块设备，不会是这些
    const QByteArray fs = info.fileSystemType().toLower();
    static const char* kPseudoFs[] = {"proc", "sysfs", "tmpfs", "devtmpfs",
                                      "devpts", "cgroup", "cgroup2", "overlay",
                                      "squashfs", "ramfs", "none"};
    for (const char* pseudo : kPseudoFs) {
        if (fs == pseudo) {
            return false;
        }
    }

    // 还要真能写：挂载点存在不代表当前用户有权限
    return QFileInfo(path).isWritable();
}

/**
 * @brief 找一个已挂载的 U 盘目录
 *
 * 板端 U 盘由 udisks/udev 自动挂到 `/media/<user>/<label>`（新版）
 * 或 `/media/<label>`（老版），两种都扫。
 *
 * 这里**只判断「是不是一个已就绪的挂载点」，不做写探测**：真的要写的时候
 * 由 `export_screenshot()` 直接尝试保存，写失败再兜底——那才是唯一可靠的
 * 判据，而「先建个临时文件试试」会在 U 盘根目录留下垃圾，也可能在只读
 * 介质上报错刷日志。
 *
 * @return 可用目录；没有则返回空 QString（由调用方决定兜底到哪）
 */
QString find_usb_dir() {
    QDir media(QStringLiteral("/media"));
    if (!media.exists()) {
        return QString();
    }

    // 先 `/media/<user>/<label>` 再 `/media/<user>`：前者是新版 udisks
    // 的布局。顺序保证有真实挂载时不会先命中后者那个空壳中间层。
    QStringList candidates;
    const QStringList users =
        media.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString& user : users) {
        QDir user_dir(media.filePath(user));
        const QStringList labels =
            user_dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString& label : labels) {
            candidates.append(user_dir.filePath(label));
        }
        candidates.append(media.filePath(user));
    }

    for (const QString& path : candidates) {
        if (is_real_writable_mount(path)) {
            return path;
        }
    }
    return QString();
}

/**
 * @brief 创建一行「名称 + 状态点 + 状态文本」
 *
 * @param name        维度中文名
 * @param state_label 输出：状态标签控件
 * @param dot_label   输出：状态圆点控件（跟随状态着色）
 * @return 该行的布局
 *
 * 布局用固定列宽而非 stretch：14 行对齐后能当表格读，
 * 不会因为某个维度名长一点就把状态列推歪。
 */
QWidget* make_status_row(const QString& name, QLabel** state_label,
                         QLabel** dot_label) {
    auto* row = new QWidget;
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(8);

    auto* name_label = new QLabel(name);
    name_label->setMinimumWidth(84);
    name_label->setObjectName(QStringLiteral("dim_name"));

    auto* dot = new QLabel(QStringLiteral("●"));
    dot->setObjectName(QStringLiteral("state_dot"));
    dot->setFixedWidth(14);
    dot->setAlignment(Qt::AlignCenter);

    auto* state = new QLabel(QStringLiteral("--"));
    state->setObjectName(QStringLiteral("state_text"));

    layout->addWidget(name_label);
    layout->addWidget(dot);
    layout->addWidget(state);
    layout->addStretch();

    *state_label = state;
    *dot_label = dot;
    return row;
}

} // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("WeakNet 诊断看板"));
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

    // ---- 摄像头取帧 ----
    //
    // 与上面的趋势取数**完全独立**的一个线程。理由见 video_fetch_thread.hpp：
    // 趋势几秒一次、视频 10fps，混在一个线程里画面会一顿一顿。
    //
    // 这里在构造时就建线程但不 start()——与 fetcher_ 一致，start() 时才真正跑。
    // 元类型注册必须在任何 connect 之前：queued connection 需要它。
    qRegisterMetaType<FetchedFrame>("weaknet::dashboard::FetchedFrame");
    video_fetcher_ = new VideoFetchThread(this);
    connect(video_fetcher_, &VideoFetchThread::frameReady,
            this, &MainWindow::onVideoFrame);
    connect(video_fetcher_, &VideoFetchThread::frameUnavailable,
            this, &MainWindow::onVideoUnavailable);
    connect(video_fetcher_, &VideoFetchThread::fetchFailed,
            this, &MainWindow::onVideoFetchFailed);
}

MainWindow::~MainWindow() {
    // 先停取帧线程再让 QObject 析构：QThread 在运行中被销毁会 abort
    // （"QThread: Destroyed while thread is still running"）。
    //
    // 这里显式 stop() 而不是写在 VideoFetchThread 的析构里：析构顺序由
    // Qt 父子关系决定，而 stop() 需要等当前这轮 D-Bus 调用返回（最长 5s），
    // 在窗口析构里做这件事语义最清楚。
    if (video_fetcher_) {
        video_fetcher_->stop();
    }
}

void MainWindow::start() {
    fetcher_->start();
    fetcher_->requestRefresh();
    refresh_in_flight_ = true;

    // 视频取帧独立启动。摄像头不在位是正常形态：不动声色地显示
    // 「暂无画面」，不要在这条路径上弹错误。
    if (video_fetcher_) {
        video_fetcher_->start();
    }
}

void MainWindow::setInterface(const QString& interface_name) {
    // 历史查询的网卡过滤在后台线程里读取，这里同步进去。
    // 时间窗取 current_window_（不是常量）：本方法在 start() 前调用一次，
    // 之后用户切窗口由 onWindowSelected() 再同步一次。
    interface_ = interface_name;
    fetcher_->setHistoryQuery(interface_name, current_window_);

    // 标题只用请求侧的网卡：查询全部网卡时序列里混着多个 iface，
    // 取数据里任意一条都会把标题写错。
    //
    // 指标名与时间窗也一并交给 refresh_chart_title()，不再在这里硬编码
    // 「分数」二字——否则切到 RSSI 之后标题还停留在「分数」。
    refresh_chart_title();
}

void MainWindow::build_ui() {
    // 趋势图控件最先创建：顶部的展示态控制区（指标下拉框）在构造时就会
    // 连上 currentIndexChanged，而那个槽要调 chart_->setMetric()。
    // 控件指针先于所有可能回调它的 connect 就绪，才不用在槽里到处判空。
    chart_ = new LineChartWidget;

    auto* central = new QWidget;
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(12, 8, 12, 6);
    root->setSpacing(6);

    // --- 顶栏 ---
    //
    // 改进前是一个扁平 QHBoxLayout：刷新按钮、网卡名、分数、状态汇总、
    // 档位、时间戳全部挤在同一行同一种字重里，看不出哪一项是主信息。
    //
    // 现在分三层视觉层级：
    //   左：操作（刷新）+ 网卡名（等宽，避免 wlan0/wlan1 混淆）
    //   中：分数（26pt 大字，整体状态色）+ 状态汇总色块
    //   右：元信息（档位、时间戳，弱化小字）
    auto* top = new QHBoxLayout;
    top->setSpacing(10);

    refresh_button_ = new QPushButton(QStringLiteral("刷新"));
    refresh_button_->setCursor(Qt::PointingHandCursor);
    connect(refresh_button_, &QPushButton::clicked,
            this, &MainWindow::onRefreshClicked);
    top->addWidget(refresh_button_, 0, Qt::AlignVCenter);

    iface_label_ = new QLabel(QStringLiteral("--"));
    iface_label_->setObjectName(QStringLiteral("iface_name"));
    top->addWidget(iface_label_, 0, Qt::AlignVCenter);

    // 分数单独一列、右对齐：数字宽度会变（9.0 / 100.0），右对齐不会抖动
    score_label_ = new QLabel(QStringLiteral("--"));
    score_label_->setObjectName(QStringLiteral("score_value"));
    score_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    score_label_->setMinimumWidth(96);
    top->addWidget(score_label_, 0, Qt::AlignVCenter);

    // 各维度状态汇总。左侧维度列表只能逐行看，这里给出整体分布，
    // 一眼看出「有几个 BAD」比逐行扫快得多。
    summary_label_ = new QLabel;
    top->addWidget(summary_label_, 0, Qt::AlignVCenter);

    top->addStretch();

    // 右侧元信息竖排：档位在上、时间戳在下，避免横向挤占主信息宽度
    auto* meta = new QVBoxLayout;
    meta->setSpacing(0);

    // 评估档位：不同 profile 下同一维度的判定阈值不同，
    // 不标出来会让「同样是 GOOD」看起来不可比。
    profile_label_ = new QLabel(QStringLiteral("--"));
    profile_label_->setObjectName(QStringLiteral("meta_info"));
    profile_label_->setAlignment(Qt::AlignRight);
    meta->addWidget(profile_label_);

    timestamp_label_ = new QLabel(QStringLiteral("--"));
    timestamp_label_->setObjectName(QStringLiteral("meta_info"));
    timestamp_label_->setAlignment(Qt::AlignRight);
    meta->addWidget(timestamp_label_);

    top->addLayout(meta);

    root->addLayout(top);

    // --- 展示态控制行 ---
    //
    // 单独一行而不是塞进顶栏：顶栏已经有刷新、网卡名、分数、状态汇总、
    // 档位、时间戳六组信息，1024 宽的 7 寸屏上再挤进一个下拉框和四个按钮，
    // 分数就会被压到看不清——而分数是这块屏上最该一眼读到的东西。
    //
    // 这一行离主区更近，语义上也更贴切：它控制的就是下面这张图。
    auto* controls = new QHBoxLayout;
    controls->setSpacing(8);
    build_view_controls(controls);
    controls->addStretch();
    root->addLayout(controls);

    // --- 主区：左（画面 + 维度列表）/ 右趋势图 ---
    auto* splitter = new QSplitter(Qt::Horizontal);

    // 左侧：摄像头画面框在上，两个状态分组在下
    //
    // 画面放最左上是刻意的：这块屏最终要挂在现场设备旁边，使用者先看
    // 「摄像头拍到什么」，再看「网络好不好」。反过来（先看十几行状态）
    // 会让人每天多扫十几行才能确认画面还活着。
    auto* left = new QWidget;
    auto* left_layout = new QVBoxLayout(left);
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(6);

    // 画面框：不设固定高度，让 splitter 的拉伸因子决定。
    // 但给它一个最小高度，否则小屏上会被状态列表挤成一条线。
    video_view_ = new VideoFrameWidget;
    video_view_->setMinimumHeight(150);
    left_layout->addWidget(video_view_, 0);

    auto* net_group = new QGroupBox(QStringLiteral("网络健康"));
    auto* net_layout = new QVBoxLayout(net_group);
    net_layout->addWidget(make_status_row(QStringLiteral("IP 可达"),
                                          &ip_reachability_label_,
                                          &ip_reachability_dot_));
    net_layout->addWidget(make_status_row(QStringLiteral("响应性"),
                                          &responsiveness_label_,
                                          &responsiveness_dot_));
    net_layout->addWidget(make_status_row(QStringLiteral("可靠性"),
                                          &reliability_label_,
                                          &reliability_dot_));
    net_layout->addWidget(make_status_row(QStringLiteral("RF 健康"),
                                          &rf_health_label_, &rf_health_dot_));
    net_layout->addStretch();
    left_layout->addWidget(net_group);

    auto* svc_group = new QGroupBox(QStringLiteral("服务健康"));
    auto* svc_layout = new QVBoxLayout(svc_group);
    svc_layout->addWidget(make_status_row(QStringLiteral("DNS"), &dns_label_,
                                          &dns_dot_));
    svc_layout->addWidget(make_status_row(QStringLiteral("TCP 连接"),
                                          &tcp_connect_label_,
                                          &tcp_connect_dot_));
    svc_layout->addWidget(make_status_row(QStringLiteral("TCP RTT"),
                                          &tcp_rtt_label_, &tcp_rtt_dot_));
    svc_layout->addWidget(make_status_row(QStringLiteral("HTTP 访问"),
                                          &http_access_label_,
                                          &http_access_dot_));
    svc_layout->addWidget(make_status_row(QStringLiteral("Portal 检测"),
                                          &captive_portal_label_,
                                          &captive_portal_dot_));
    svc_layout->addWidget(make_status_row(QStringLiteral("主动 DNS"),
                                          &active_dns_label_, &active_dns_dot_));
    svc_layout->addWidget(make_status_row(QStringLiteral("主动 TCP"),
                                          &active_tcp_label_, &active_tcp_dot_));
    svc_layout->addWidget(make_status_row(QStringLiteral("主动 HTTPS"),
                                          &active_https_label_,
                                          &active_https_dot_));
    svc_layout->addWidget(make_status_row(QStringLiteral("主动 Portal"),
                                          &active_portal_label_,
                                          &active_portal_dot_));
    svc_layout->addStretch();
    left_layout->addWidget(svc_group);

    splitter->addWidget(left);

    // 右侧：趋势图
    //
    // 控件已在 build_ui() 开头创建（展示态控制区的槽需要它先存在），
    // 这里只负责挂进布局。标题由 setInterface() 统一设置（main.cpp
    // 无条件调用）：这里不给默认值，避免两处文案各自演化后对不上实际查询范围。
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
    experience_label_->setObjectName(QStringLiteral("primary_text"));
    experience_label_->setWordWrap(true);
    root->addWidget(experience_label_);

    // 体验警告（experience 独占）：服务端主动标记的异常，红色单独一行，
    // 不与 experience_label_ 抢位置——两者会同时有内容
    experience_detail_label_ = new QLabel;
    experience_detail_label_->setObjectName(QStringLiteral("warn_text"));
    experience_detail_label_->setWordWrap(true);
    experience_detail_label_->hide();
    root->addWidget(experience_detail_label_);

    diagnosis_label_ = new QLabel(QStringLiteral("诊断：等待数据…"));
    diagnosis_label_->setObjectName(QStringLiteral("primary_text"));
    diagnosis_label_->setWordWrap(true);
    root->addWidget(diagnosis_label_);

    // 诊断的人读摘要（diagnosis 独占）：服务端给的是完整句子，
    // 比 primary_issue 这类机器 token 更适合直接展示给用户
    summary_template_label_ = new QLabel;
    summary_template_label_->setObjectName(QStringLiteral("primary_text"));
    summary_template_label_->setWordWrap(true);
    summary_template_label_->hide();
    root->addWidget(summary_template_label_);

    // 次要问题 / 已排除原因 / 证据（diagnosis 独占，默认折叠）
    diagnosis_extra_label_ = new QLabel;
    diagnosis_extra_label_->setObjectName(QStringLiteral("detail_text"));
    diagnosis_extra_label_->setWordWrap(true);
    diagnosis_extra_label_->hide();
    root->addWidget(diagnosis_extra_label_);

    // --- 整页滚动（小屏 + 手指划动）---
    //
    // ## 为什么整页滚而不是压缩内容
    //
    // 这套布局的最小高度实测 661px（见 README「布局高度」一节），而现场
    // 最便宜、最容易买到的 7 寸屏是 1024x600——差 61px。两条路可选：
    // 删掉底栏内容去迁就屏幕，或让整页可滚、信息一条不少。选后者：
    // 底栏那几行是产品定位里明确的「把数据变成结论」，删掉就只剩数字。
    //
    // ## setWidgetResizable(true) 的作用
    //
    // 让内容宽度恒等于视口宽度（不产生横向滚动），高度取内容自身的
    // sizeHint：比视口高就滚，比视口矮就撑满。关掉它的话滚动区会按内容
    // 原始尺寸显示，宽屏两侧各留一条空白，视觉上像没铺满。
    //
    // ## 横向永久关闭
    //
    // 看板是竖着排的一列，横向本就没有溢出。留着横向条只会在触摸时
    // 误触发左右晃，还白占 14px 高度。
    //
    // ## 为什么是 TouchGesture，不是 LeftMouseButtonGesture
    //
    // 现场用手指划屏。MouseButton 手势会把「点按钮」和「拖页面」混成
    // 同一件事：按下按钮后手指一动就变成拖页面，按钮永远点不动。
    // TouchGesture 只吃触摸事件，鼠标点击与按钮行为完全不受影响。
    //
    // ## 手势必须抓在 viewport 上
    //
    // 内容 widget 盖在 QScrollArea 之上，触摸事件落不到 QScrollArea 本身。
    // 抓 viewport 才能收到内容区上的触摸。
    auto* scroll = new QScrollArea;
    scroll->setWidget(central);
    scroll->setWidgetResizable(true);
    // 去掉默认的 StyledPanel 边框：内容自己已有卡片样式，外面再套一圈
    // 灰线会显得像嵌在另一个容器里。
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    QScroller::grabGesture(scroll->viewport(), QScroller::TouchGesture);

    setCentralWidget(scroll);

    status_bar_label_ = new QLabel(QStringLiteral("就绪"));
    statusBar()->addWidget(status_bar_label_);

    // --- 默认尺寸：宽度写死，高度由内容推导 ---
    //
    // 宽度 1024 是产品语义（7 寸屏的原生宽度），不随内容变化。
    //
    // 高度**不能写死**。原先写 `resize(1024, 720)`，理由是「内容 + 状态栏
    // 正好凑满 720」。那是一个只在某个主题/字体组合下成立的巧合：内容高度
    // 取决于主题 QSS 字号、字体度量、以及那些「有数据才 show()」的行，任何
    // 一项变化都会让这个数对不上，而失败表现只是「桌面上多一条滚动条」，
    // 很容易被当成正常。改成按内容实测高度反推，关系对任何字体度量都成立。
    //
    // 三处必须注意的实现细节：
    //
    // 1. **取 `scroll->widget()` 的 sizeHint，不是 `scroll` 自己的。**
    //    `QScrollArea::sizeHint()` 有独立的上限逻辑，与内容高度不是同一回事。
    //    踩过一次：写成 `centralWidget()->sizeHint()`，窗口高度纹丝不动。
    // 2. **必须在 `addWidget(status_bar_label_)` 之后算。** 状态栏没内容时
    //    `sizeHint()` 偏小，chrome 会算少。
    // 3. **测量前必须先应用主题**。主题的 QSS 会改字号，未应用主题时内容
    //    高度与应用后差十几像素（实测 690 vs 674），用错基准会把「够不够放」
    //    判反。`main.cpp` 是先 `apply_theme()` 再构造窗口，顺序不能倒。
    //
    // `chrome`（窗口高 − 视口高）恒等于状态栏高度，600/700/720/740/760/800
    // 六个高度逐一验证过。不留安全余量——多留几像素会把「刚好放满」变成
    // 「略有余量」，下次对齐时反而误以为差值是布局问题。
    //
    // 下限仍取 720：内容比 720 还矮时（比如将来精简了底栏），不该缩成
    // 一个小窗口，720p 是这块屏的基准分辨率。
    //
    // 边界：这只保证**空态**放得下。真实数据到达后，警告行 / 人读摘要 /
    // 次要问题等隐藏行会显示出来，内容比空态更高（板端实测 674 → 770），
    // 默认窗口会出现滚动条。那是内容量决定的，不是这里的推导漏了——
    // 消掉它只能砍信息或把窗口顶出 720p 屏。整页滚动的意义正在于此。
    const int content_h = scroll->widget()->sizeHint().height();
    const int chrome_h = statusBar()->sizeHint().height();
    resize(1024, qMax(720, content_h + chrome_h));
}

// ---------------------------------------------------------------------------
// 展示态控制（现场交互终端白名单的三项）
// ---------------------------------------------------------------------------

void MainWindow::build_view_controls(QHBoxLayout* controls) {
    // --- 指标选择器 ---
    //
    // 五个指标全部来自 `network_history` 表的真实列，服务端一次查询就
    // 把它们一起返回（见 `queryHistory()` 的 SELECT 列表），所以**切指标
    // 不需要重新取数**：`LineChartWidget` 里已经缓存着完整的原始序列，
    // `setMetric()` 只是换个字段重画。这一点决定了下面 onMetricSelected
    // 里没有 `requestRefresh()`——多打一次 D-Bus 只会让界面闪一下。
    auto* metric_caption = new QLabel(QStringLiteral("指标"));
    metric_caption->setObjectName(QStringLiteral("meta_info"));
    controls->addWidget(metric_caption, 0, Qt::AlignVCenter);

    metric_combo_ = new QComboBox;
    const HistoryMetric kMetrics[] = {
        HistoryMetric::Score,  HistoryMetric::Rtt, HistoryMetric::Jitter,
        HistoryMetric::Rssi,   HistoryMetric::TcpLoss,
    };
    for (HistoryMetric metric : kMetrics) {
        // 带上单位，避免把「RTT 20」读成「20 分」这类跨量纲误读
        QString text = QString::fromUtf8(history_metric_label(metric));
        const char* unit = history_metric_unit(metric);
        if (unit != nullptr && unit[0] != '\0') {
            text += QStringLiteral(" (") + QString::fromUtf8(unit) +
                    QStringLiteral(")");
        }
        // 存 enum 的整数值而不是靠下标对齐：以后若调整枚举顺序，
        // 下标会静默错位，存值则直接跟着枚举走
        metric_combo_->addItem(text, static_cast<int>(metric));
    }
    // 与 current_metric_ 对齐，而不是硬写 0：默认值只在一个地方定义
    metric_combo_->setCurrentIndex(
        metric_combo_->findData(static_cast<int>(current_metric_)));
    // 用显式 static_cast 取重载，不用 QOverload：后者要求 Qt 5.7+ 且在
    // 不同 Qt 版本里的可见性依赖间接包含的头文件。这里只有一个 int 重载，
    // 写全类型最稳。
    connect(metric_combo_,
            static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
            this, &MainWindow::onMetricSelected);
    controls->addWidget(metric_combo_, 0, Qt::AlignVCenter);

    controls->addSpacing(16);

    // --- 时间窗按钮 ---
    //
    // 三个互斥按钮而不是下拉框：现场只有三个固定窗口，摆出来一眼可见、
    // 一次点击就切；下拉框要点两下且收起后看不出当前选的是哪个。
    //
    // 最大窗口是 13 小时而不是 24 小时：服务端 `kMaxHistoryLimit = 10000`，
    // 落库周期 5 秒 → 最多覆盖 50000 秒 ≈ 13.89 小时。24h 需要 17280 个点，
    // 超出上限 73%，会被**静默钳制**（板端实测确认），结果是「标签写 24h、
    // 图里只有 13.9h」。7 天更不可能。
    // 详见 `data_source.hpp` 的表格与 `dashboard_data.hpp` 的窗口定义注释。
    auto* window_caption = new QLabel(QStringLiteral("时间窗"));
    window_caption->setObjectName(QStringLiteral("meta_info"));
    controls->addWidget(window_caption, 0, Qt::AlignVCenter);

    struct WindowSpec {
        QPushButton** slot;
        HistoryWindow window;
        const char* text;
        const char* tip;
    };
    const WindowSpec specs[] = {
        {&window_1h_button_, HistoryWindow::Last1h, "1 小时",
         "最近 1 小时（约 360 个采样点）"},
        {&window_6h_button_, HistoryWindow::Last6h, "6 小时",
         "最近 6 小时（约 2160 个采样点）"},
        {&window_13h_button_, HistoryWindow::Last13h, "13 小时",
         "最近 13 小时（约 9360 个采样点，服务端上限内最优覆盖）"},
    };
    for (const WindowSpec& spec : specs) {
        auto* button = new QPushButton(QString::fromUtf8(spec.text));
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(QString::fromUtf8(spec.tip));
        button->setChecked(spec.window == current_window_);
        const int id = static_cast<int>(spec.window);
        connect(button, &QPushButton::clicked, this, [this, id]() {
            onWindowSelected(id);
        });
        *spec.slot = button;
        controls->addWidget(button, 0, Qt::AlignVCenter);
    }

    controls->addSpacing(16);

    // --- 截图导出 ---
    export_button_ = new QPushButton(QStringLiteral("导出截图"));
    export_button_->setCursor(Qt::PointingHandCursor);
    export_button_->setToolTip(
        QStringLiteral("把当前画面存成 PNG：优先写 U 盘，没有 U 盘时写 /tmp"));
    connect(export_button_, &QPushButton::clicked,
            this, &MainWindow::onExportClicked);
    controls->addWidget(export_button_, 0, Qt::AlignVCenter);
}

void MainWindow::onMetricSelected(int index) {
    if (index < 0 || metric_combo_ == nullptr) {
        return;
    }
    const HistoryMetric metric =
        static_cast<HistoryMetric>(metric_combo_->itemData(index).toInt());
    if (metric == current_metric_) {
        return;
    }
    current_metric_ = metric;

    // 只换视图，不重置时间窗、不重新取数：
    //   1. 时间窗是用户上一次的选择，切指标不该把它悄悄改回 1 小时
    //   2. 五个指标在同一条记录里，序列已经在内存中
    chart_->setMetric(metric);
    refresh_chart_title();
}

void MainWindow::onWindowSelected(int window_id) {
    const HistoryWindow window = static_cast<HistoryWindow>(window_id);

    // 先把三个按钮的选中态统一回写到 current_window_。
    // 可勾选按钮被点一下会自己翻转状态，用户点「已经选中的那个」会把它
    // 取消勾选，界面上就出现「三个都不亮」——此时必须复位。
    const auto sync_buttons = [this]() {
        if (window_1h_button_ != nullptr) {
            window_1h_button_->setChecked(current_window_ == HistoryWindow::Last1h);
        }
        if (window_6h_button_ != nullptr) {
            window_6h_button_->setChecked(current_window_ == HistoryWindow::Last6h);
        }
        if (window_13h_button_ != nullptr) {
            window_13h_button_->setChecked(current_window_ == HistoryWindow::Last13h);
        }
    };

    if (window == current_window_) {
        sync_buttons();
        return;
    }

    current_window_ = window;
    sync_buttons();

    // 只换查询范围，不重置指标——用户选 RSSI 之后想看的仍是 RSSI。
    // 网卡沿用 interface_（它在 setInterface() 里定下，之后的切换不该动它）。
    fetcher_->setHistoryQuery(interface_, current_window_);
    refresh_chart_title();

    // 时间窗变了必须重新取数：内存里只有上一窗口的数据，
    // 13h 视图拿 1h 的 720 个点画出来只是「最近一小时被拉宽」，是假的。
    //
    // 不判断 refresh_in_flight_：requestRefresh() 只是置一个标志位，
    // 正在进行的那一轮结束后会被立即消费，最多多跑一轮，不会堆积。
    refresh_in_flight_ = true;
    status_bar_label_->setText(QStringLiteral("切换时间窗，刷新中…"));
    fetcher_->requestRefresh();
}

void MainWindow::refresh_chart_title() {
    if (chart_ == nullptr) {
        return;
    }

    // 这里**只写网卡与时间窗**，不写指标名。
    // 指标名与单位由 `LineChartWidget::paint()` 自己在标题后面拼接
    // （见该文件），这样 `setMetric()` 之后标题不可能停留在旧指标上。
    // 两处都拼会得到「· 综合分 · 综合分」。
    //
    // 拆分顺序按「越靠左越先回答现场最常问的问题」：
    //   图是什么 → 哪个网卡 → 多长时间 → 哪个指标（由控件补）
    const QString iface =
        interface_.isEmpty() ? QStringLiteral("全部网卡") : interface_;
    chart_->setTitle(QStringLiteral("历史趋势 · %1 · %2")
                         .arg(iface,
                              QString::fromUtf8(history_window_label(current_window_))));
}

QString MainWindow::export_screenshot() {
    const QString filename =
        QString::fromLatin1(kScreenshotPrefix) +
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")) +
        QStringLiteral(".png");

    // 先找目标目录再抓图：grab() 会触发一次完整重绘，是这里最贵的动作。
    const QString usb = find_usb_dir();

    const QPixmap shot = grab();
    if (shot.isNull()) {
        return QString();
    }

    if (!usb.isEmpty()) {
        const QString path = QDir(usb).filePath(filename);
        if (shot.save(path, "PNG")) {
            return path;
        }
        // U 盘在、但写不进去（只读挂载 / 权限 / 刚好被拔）。
        // 不在这里 return：兜底继续走，失败必须由调用方如实报告。
    }

    const QString fallback =
        QDir(QString::fromLatin1(kScreenshotFallbackDir)).filePath(filename);
    if (shot.save(fallback, "PNG")) {
        return fallback;
    }
    return QString();
}

void MainWindow::onExportClicked() {
    const QString path = export_screenshot();
    if (path.isEmpty()) {
        status_bar_label_->setText(
            QStringLiteral("导出截图失败：U 盘与 %1 均无法写入")
                .arg(QString::fromLatin1(kScreenshotFallbackDir)));
        return;
    }

    // 必须说出**实际**路径，不能笼统写「已保存」：
    // 没有 U 盘时图落在 /tmp（tmpfs），重启或掉电就没了，
    // 现场会以为证据还在。这条提示是唯一能让人知道这件事的地方。
    const QString fallback_prefix =
        QString::fromLatin1(kScreenshotFallbackDir) + QLatin1Char('/');
    if (path.startsWith(fallback_prefix)) {
        status_bar_label_->setText(
            QStringLiteral("截图已存 %1（未检测到 U 盘，临时目录，重启即失）")
                .arg(path));
    } else {
        status_bar_label_->setText(QStringLiteral("截图已存 %1").arg(path));
    }
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
// 摄像头画面
//
// 三个槽对应三种**语义完全不同**的情形，必须分开处理：
//
//   onVideoFrame       拿到真实画面          → 画出来
//   onVideoUnavailable 服务端正常、只是没帧  → 「暂无画面」，不是错误
//   onVideoFetchFailed 调用本身失败          → 说明原因（但也不弹窗）
//
// 把后两者混成「显示错误」会让「摄像头还没启动」看起来像故障——
// 而摄像头不在位在本项目里是**正常形态**（见 server.cpp 的接线注释）。
// ---------------------------------------------------------------------------

void MainWindow::onVideoFrame(const weaknet::dashboard::FetchedFrame& frame) {
    if (!video_view_) return;

    // 跨线程传过来的是取帧线程的副本，这里再拷一次进 VideoFrame。
    // QByteArray 是隐式共享的，这一步是 O(1)（引用计数 +1），不是 3MB 拷贝。
    VideoFrame vf;
    vf.nv12 = frame.nv12;
    vf.width = frame.width;
    vf.height = frame.height;
    vf.stride = frame.stride;
    vf.uv_offset = frame.uv_offset;
    vf.fourcc = frame.fourcc;
    vf.sequence = frame.sequence;
    vf.timestamp_ns = frame.timestamp_ns;
    video_view_->setFrame(vf);
}

void MainWindow::onVideoUnavailable() {
    if (video_view_) {
        video_view_->clearFrame(QStringLiteral("暂无画面"));
    }
}

void MainWindow::onVideoFetchFailed(const QString& error) {
    if (video_view_) {
        // 把原因画在框里而不是状态栏：画面框自己说明为什么没画面，
        // 比让人去别处找线索直接得多。
        video_view_->clearFrame(QStringLiteral("取帧失败：%1").arg(error));
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
        QStringLiteral("<span style=\"color:%1\">● %2</span> "
                       "<span style=\"color:%3\">● %4</span> "
                       "<span style=\"color:%5\">● %6</span> "
                       "<span style=\"color:%7\">● %8</span>")
            .arg(QString::fromLatin1(ThemeColors::kGood))
            .arg(healthy)
            .arg(QString::fromLatin1(ThemeColors::kDegraded))
            .arg(degraded)
            .arg(QString::fromLatin1(ThemeColors::kBad))
            .arg(failed)
            .arg(QString::fromLatin1(ThemeColors::kUnknown))
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

    update_status_label(snap.ip_reachability, ip_reachability_label_,
                        ip_reachability_dot_);
    update_status_label(snap.responsiveness, responsiveness_label_,
                        responsiveness_dot_);
    update_status_label(snap.reliability, reliability_label_, reliability_dot_);
    update_status_label(snap.rf_health, rf_health_label_, rf_health_dot_);

    update_status_label(snap.dns_service, dns_label_, dns_dot_);
    update_status_label(snap.tcp_connect, tcp_connect_label_, tcp_connect_dot_);
    update_status_label(snap.tcp_rtt, tcp_rtt_label_, tcp_rtt_dot_);
    update_status_label(snap.http_access, http_access_label_, http_access_dot_);
    update_status_label(snap.captive_portal, captive_portal_label_,
                        captive_portal_dot_);
    update_status_label(snap.active_dns, active_dns_label_, active_dns_dot_);
    update_status_label(snap.active_tcp, active_tcp_label_, active_tcp_dot_);
    update_status_label(snap.active_https, active_https_label_,
                        active_https_dot_);
    update_status_label(snap.active_portal, active_portal_label_,
                        active_portal_dot_);
}

void MainWindow::update_status_label(const SleItem& item, QLabel* state_label,
                                     QLabel* dot_label) {
    if (!state_label) {
        return;
    }

    // reason 是服务端给出的人可读 token（如 jitter_excessive、no_dns_observations），
    // 它解释了「为什么是这个状态」。用 tooltip 挂在标签上，不占版面但可查。
    const QString reason = QString::fromStdString(item.reason);
    const QString tip = reason.isEmpty()
                            ? QString()
                            : QStringLiteral("%1：%2")
                                  .arg(QString::fromStdString(item.key), reason);
    state_label->setToolTip(tip);
    if (dot_label) {
        dot_label->setToolTip(tip);
    }

    // 圆点与文字同色：改进前圆点恒为默认灰，看得出「有个点」但看不出状态，
    // 状态只能靠右侧文字颜色辨认，14 行扫下来很容易漏。
    auto paint_dot = [dot_label](const QString& color) {
        if (dot_label) {
            dot_label->setStyleSheet(QStringLiteral("color: %1;").arg(color));
        }
    };

    // 不适用的维度（如有线链路下的 RF 健康）显式标注，不显示 GOOD/BAD
    if (!item.applicable()) {
        const QString grey = QString::fromLatin1(ThemeColors::kUnknown);
        state_label->setText(QStringLiteral("不适用"));
        state_label->setStyleSheet(QStringLiteral("color: %1;").arg(grey));
        paint_dot(grey);
        return;
    }

    if (!item.has_value()) {
        const QString grey = QString::fromLatin1(ThemeColors::kUnknown);
        state_label->setText(QStringLiteral("未知"));
        state_label->setStyleSheet(QStringLiteral("color: %1;").arg(grey));
        paint_dot(grey);
        return;
    }

    // 部分覆盖的维度加后缀，提示结论置信度较低
    QString text = QString::fromStdString(item.state);
    if (item.coverage == "PARTIAL") {
        text += QStringLiteral(" (部分)");
    }
    const QString color = state_color(QString::fromStdString(item.state));
    state_label->setText(text);
    state_label->setStyleSheet(QStringLiteral("color: %1;").arg(color));
    paint_dot(color);
}

QString MainWindow::state_color(const QString& state) {
    // 取值依据 server/include/assurance/health_state.hpp:15-23
    // 色值统一从 ThemeColors 取，不在这里重写一遍：同一个绿色曾在
    // state_color()、summary_label_ 的 html、折线图默认色三处各写一遍。
    if (state == QLatin1String("GOOD")) {
        return QString::fromLatin1(ThemeColors::kGood);
    }
    if (state == QLatin1String("DEGRADED")) {
        return QString::fromLatin1(ThemeColors::kDegraded);
    }
    if (state == QLatin1String("BAD")) {
        return QString::fromLatin1(ThemeColors::kBad);
    }
    return QString::fromLatin1(ThemeColors::kUnknown);
}

} // namespace dashboard
} // namespace weaknet

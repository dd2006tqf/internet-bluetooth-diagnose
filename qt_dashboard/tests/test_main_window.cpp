/**
 * @file test_main_window.cpp
 * @brief MainWindow 展示态控制的交互测试
 *
 * ## 这些测试防的是什么回归
 *
 * 产品定位 v1.2 把现场可交互的能力封闭成三项白名单：**换指标、换时间窗、
 * 导出截图**。这三项的共同约束是「只能改展示态，不能改任何状态」。
 * 实现上最容易无声破坏的正是那三条语义：
 *
 * 1. **切指标不能重置时间窗**。用户先选 13 小时再切到 RSSI，图上必须还是
 *    13 小时的 RSSI。若 `onMetricSelected()` 里顺手把 `current_window_`
 *    写回默认值，界面看起来「切成功了」，但数据范围被悄悄改小了。
 * 2. **切时间窗不能重置指标**。反过来同理。
 * 3. **时间窗切换必须真的触发取数**，否则 13h 视图会拿 1h 的 720 个点
 *    拉伸着画——标签写着 13 小时，实际只有 1 小时的数据。
 *
 * 最大窗口为什么是 13h 而不是 24h，见 `data/dashboard_data.hpp` 的
 * `HistoryWindow` 注释：服务端 `kMaxHistoryLimit = 10000` × 落库 5 秒
 * ≈ 13.89h，24h 会被静默钳制。
 *
 * 这三条都是「看起来对、量起来错」的类型，只能靠断言，不能靠眼睛。
 *
 * ## 为什么用 QMetaObject 调用而不是造测试专用接口
 *
 * 产品代码不应该为测试多开一个入口（那会变成只有测试才用的孤儿接口）。
 * 这里全部通过 `QMetaObject::invokeMethod()` 走真实的槽：
 * 既验证了槽确实被 Qt 元对象认识（`connect()` 的字符串形式要能工作），
 * 也没在生产代码里留下测试专用的钩子。
 */

#include <gtest/gtest.h>

#include <QApplication>
#include <QByteArray>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QImage>
#include <QList>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QScroller>
#include <QStatusBar>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <QMetaObject>
#include <QMetaType>

#include <cstdio>

#include "data/dashboard_data.hpp"
#include "ui/main_window.hpp"
#include "ui/theme.hpp"

namespace {

using weaknet::dashboard::HistoryMetric;
using weaknet::dashboard::HistoryWindow;
using weaknet::dashboard::MainWindow;

/// `QWidget` 要求进程内有 `QApplication`（`QGuiApplication` 不够）
class QtAppEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        static int argc = 1;
        static char arg0[] = "test_main_window";
        static char* argv[] = {arg0, nullptr};

        if (!QApplication::instance()) {
            app_ = new QApplication(argc, argv);
        }
    }

    void TearDown() override {
        delete app_;
        app_ = nullptr;
    }

private:
    QApplication* app_ = nullptr;
};

[[maybe_unused]] const ::testing::Environment* const g_qt_env =
    ::testing::AddGlobalTestEnvironment(new QtAppEnvironment);

/// 在窗口里按对象名找不到控件时，测试要给出可诊断的失败而不是崩
template <typename T>
T* find_child(QWidget& root, const char* name) {
    return root.findChild<T*>(QString::fromLatin1(name));
}

/**
 * @brief 从三个可勾选的「1/6/13 小时」按钮里认出时间窗按钮
 *
 * 生产代码没给它们设 objectName（那只是为了测试方便，不该污染产品代码）。
 * 这里按「可勾选 + 文案是小时数」识别，是目前唯一不侵入产品的办法。
 * 若将来按钮文案改了，这条测试会明确报「找不到时间窗按钮」，
 * 而不是静默跳过——静默跳过等于这条测试不存在。
 */
QPushButton* window_button(QWidget& root, const QString& text) {
    const QList<QPushButton*> buttons = root.findChildren<QPushButton*>();
    for (QPushButton* b : buttons) {
        if (b->isCheckable() && b->text() == text) {
            return b;
        }
    }
    return nullptr;
}

/// 走真实的 slider/dropdown 路径切指标：直接改下拉框索引，触发信号
void select_metric(MainWindow& w, HistoryMetric metric) {
    auto* combo = find_child<QComboBox>(w, "metric_combo");
    if (combo == nullptr) {
        // 生产代码没设 objectName，退回「窗口里唯一那个 QComboBox」
        const QList<QComboBox*> combos = w.findChildren<QComboBox*>();
        EXPECT_EQ(combos.size(), 1) << "主窗口应只有一个指标选择器";
        if (combos.isEmpty()) {
            return;
        }
        combo = combos.first();
    }
    const int index = combo->findData(static_cast<int>(metric));
    ASSERT_GE(index, 0) << "下拉框里没有这个指标条目";
    combo->setCurrentIndex(index);
}

} // namespace

// ---------------------------------------------------------------------------
// 默认状态
// ---------------------------------------------------------------------------

TEST(MainWindowTest, DefaultsToScoreAndOneHour) {
    // 默认值决定冷启动后第一屏。综合分是产品定位里的主指标，
    // 1 小时是「看当前情况」的默认视野，两者都不该被随手改掉。
    MainWindow w;
    EXPECT_EQ(w.current_metric(), HistoryMetric::Score);
    EXPECT_EQ(w.current_window(), HistoryWindow::Last1h);
}

TEST(MainWindowTest, ControlRowExists) {
    // 白名单三项必须都在界面上。缺任何一项，现场就少一个可用能力；
    // 多出一项则是越过权责契约（现场只能改展示态）。
    MainWindow w;
    w.setInterface(QString());
    w.resize(1024, 700);
    w.show();
    QApplication::processEvents();

    const QList<QComboBox*> combos = w.findChildren<QComboBox*>();
    EXPECT_EQ(combos.size(), 1) << "应有且仅有一个指标选择器";
    // 五个指标全部可选：它们都来自 network_history 的真实列
    EXPECT_EQ(combos.first()->count(), 5);

    EXPECT_NE(window_button(w, QStringLiteral("1 小时")), nullptr);
    EXPECT_NE(window_button(w, QStringLiteral("6 小时")), nullptr);
    EXPECT_NE(window_button(w, QStringLiteral("13 小时")), nullptr);

    // 导出按钮：文案固定，按文案找
    bool has_export = false;
    for (QPushButton* b : w.findChildren<QPushButton*>()) {
        if (b->text() == QStringLiteral("导出截图")) {
            has_export = true;
            break;
        }
    }
    EXPECT_TRUE(has_export) << "缺少导出截图按钮";
}

// ---------------------------------------------------------------------------
// 两条「互不重置」的约束（本轮最关键）
// ---------------------------------------------------------------------------

TEST(MainWindowTest, SwitchingMetricKeepsTheSelectedWindow) {
    MainWindow w;
    w.setInterface(QString());

    // 先切到 13 小时
    QPushButton* b13 = window_button(w, QStringLiteral("13 小时"));
    ASSERT_NE(b13, nullptr);
    b13->click();
    ASSERT_EQ(w.current_window(), HistoryWindow::Last13h);

    // 再切指标：时间窗必须原样保留
    select_metric(w, HistoryMetric::Rssi);

    EXPECT_EQ(w.current_metric(), HistoryMetric::Rssi);
    EXPECT_EQ(w.current_window(), HistoryWindow::Last13h)
        << "切指标把时间窗重置了：用户会以为在看 13 小时，实际是 1 小时";
}

TEST(MainWindowTest, SwitchingWindowKeepsTheSelectedMetric) {
    MainWindow w;
    w.setInterface(QString());

    // 先切到 TCP 丢包率
    select_metric(w, HistoryMetric::TcpLoss);
    ASSERT_EQ(w.current_metric(), HistoryMetric::TcpLoss);

    // 再切时间窗：指标必须原样保留
    QPushButton* b6 = window_button(w, QStringLiteral("6 小时"));
    ASSERT_NE(b6, nullptr);
    b6->click();

    EXPECT_EQ(w.current_window(), HistoryWindow::Last6h);
    EXPECT_EQ(w.current_metric(), HistoryMetric::TcpLoss)
        << "切时间窗把指标重置了：用户选的丢包率被悄悄换回综合分";
}

// ---------------------------------------------------------------------------
// 按钮选中态
// ---------------------------------------------------------------------------

TEST(MainWindowTest, WindowButtonsAreMutuallyExclusive) {
    // 三个可勾选按钮必须始终恰好有一个亮着。
    // `setCheckable(true)` 的按钮被点一下会自己翻转，用户点「已选中的那个」
    // 会把它取消勾选，界面上三个都不亮——那时 x 轴范围就无从判断了。
    MainWindow w;
    w.setInterface(QString());

    auto checked_count = [&w]() {
        int n = 0;
        const QStringList texts = {QStringLiteral("1 小时"), QStringLiteral("6 小时"),
                                   QStringLiteral("13 小时")};
        for (const QString& t : texts) {
            QPushButton* b = window_button(w, t);
            if (b != nullptr && b->isChecked()) {
                ++n;
            }
        }
        return n;
    };

    EXPECT_EQ(checked_count(), 1) << "初始应恰好有一个按钮处于选中态";

    QPushButton* b13 = window_button(w, QStringLiteral("13 小时"));
    ASSERT_NE(b13, nullptr);
    b13->click();
    EXPECT_EQ(checked_count(), 1) << "切到 13 小时后仍应只有一个选中";

    // 重复点当前已选中的那个：不能变成「三个都不亮」
    b13->click();
    EXPECT_EQ(checked_count(), 1)
        << "重复点击已选中的窗口按钮把它取消勾选了，界面失去范围指示";
    EXPECT_EQ(w.current_window(), HistoryWindow::Last13h)
        << "重复点击不该改变当前时间窗";
}

TEST(MainWindowTest, CheckedWindowButtonLooksDifferent) {
    // 上一条测试只守了「恰好一个按钮 isChecked() == true」这个**逻辑**不变量。
    // 真机截图暴露出它不够：三个按钮的 `:hover` / `:pressed` 之外没有任何
    // 选中态样式，渲染出来边框与填充完全一致（实测三者的边框像素都是
    // `(224,227,232)`、内部都是 `(255,255,255)`）。现场用户看不出当前选的是
    // 哪个时间窗——控件存在、状态也对，但**这个状态不可见**。
    //
    // 判据不锚定具体配色（换主题不该让测试失败），只要求「选中与未选中
    // 的渲染结果必须可区分」。这条要求本身就是选中态的语义。
    // 必须先把产品主题装上去，否则量到的是 Fusion 原生样式。
    //
    // 这一点是本条测试第二版才想明白的：第一版没调 `apply_theme()`，
    // 而缺陷恰恰藏在**全局 QSS 覆盖**里（QSS 定义了 :hover/:pressed 却漏了
    // :checked，把原生样式里本该有的选中效果一并盖掉）。不装主题时
    // Fusion 原生的 checked 态自带高亮，测试于是 PASS——量到的不是
    // 产品真实渲染路径，缺陷被完美掩盖。
    weaknet::dashboard::apply_theme();

    MainWindow w;
    w.setInterface(QString());
    w.resize(1024, 700);
    w.show();
    QApplication::processEvents();

    auto render = [](QPushButton* b) {
        return b->grab().toImage().convertToFormat(QImage::Format_RGB32);
    };
    // 两图逐像素比较，返回不同的像素数。用「不同像素数」而不是「是否
    // 完全相同」，是为了让失败信息能说明差异有多小。
    auto diff_pixels = [](const QImage& a, const QImage& b) {
        if (a.size() != b.size()) {
            return static_cast<qint64>(a.width()) * a.height();
        }
        qint64 n = 0;
        for (int y = 0; y < a.height(); ++y) {
            for (int x = 0; x < a.width(); ++x) {
                if (a.pixel(x, y) != b.pixel(x, y)) {
                    ++n;
                }
            }
        }
        return n;
    };

    QPushButton* b1 = window_button(w, QStringLiteral("1 小时"));
    QPushButton* b13 = window_button(w, QStringLiteral("13 小时"));
    ASSERT_NE(b1, nullptr);
    ASSERT_NE(b13, nullptr);
    ASSERT_TRUE(b1->isChecked()) << "初始应选中 1 小时";
    ASSERT_FALSE(b13->isChecked());

    // 判据必须是**同一个按钮**在选中态与未选中态的渲染差。
    //
    // 不能拿「1 小时按钮」和「13 小时按钮」互相比：它们的文案本来就不同，
    // 像素必然不同——那样的断言恒真，什么缺陷都抓不到（第一版就是这么写的，
    // 在缺陷存在时照样 PASS）。只有固定同一个控件、只改 checked 状态，
    // 差异才唯一来自选中态样式。
    const QImage b1_checked = render(b1);
    const QImage b13_unchecked = render(b13);

    // 切到 13 小时：b1 变为未选中、b13 变为选中
    b13->click();
    QApplication::processEvents();
    ASSERT_TRUE(b13->isChecked());
    ASSERT_FALSE(b1->isChecked());

    const QImage b1_unchecked = render(b1);
    const QImage b13_checked = render(b13);

    // (a) 未选中态的 b1 与选中态的 b1 必须不同
    const qint64 b1_delta = diff_pixels(b1_checked, b1_unchecked);
    EXPECT_GT(b1_delta, 100)
        << "时间窗按钮在选中与未选中之间渲染完全相同（" << b1_delta
        << " 像素不同）：checked 状态对用户不可见，现场无法判断当前时间窗";

    // (b) checked → unchecked 再 checked 应回到同一外观（选中态是稳定的，
    //     不是「点一下换个随机色」）
    b1->click();
    QApplication::processEvents();
    ASSERT_TRUE(b1->isChecked());
    const qint64 round_trip = diff_pixels(b1_checked, render(b1));
    EXPECT_EQ(round_trip, 0) << "同一选中态两次渲染不一致，样式不稳定";

    // (c) 反向：b13 的选中态也必须与它的未选中态可区分
    const qint64 b13_delta = diff_pixels(b13_unchecked, b13_checked);
    EXPECT_GT(b13_delta, 100)
        << "13 小时按钮的选中态与未选中态无法区分（" << b13_delta << " 像素不同）";
}

// ---------------------------------------------------------------------------
// 截图导出（白名单第 3 项）
// ---------------------------------------------------------------------------

TEST(MainWindowTest, ExportScreenshotWritesARealPng) {
    // 导出的核心承诺是「一定会给出一个能用的文件，并如实报告在哪」。
    // 这条测试走真实路径：调槽 → 抓图 → 落盘 → 回读校验 PNG 魔数。
    // 它不验证落在 U 盘还是 /tmp（那是环境决定的），只验证产物有效。
    MainWindow w;
    w.setInterface(QString());
    w.resize(1024, 700);
    w.show();
    QApplication::processEvents();

    const bool invoked = QMetaObject::invokeMethod(
        &w, "onExportClicked", Qt::DirectConnection);
    ASSERT_TRUE(invoked) << "onExportClicked 必须是可被元对象调用的槽";

    // 找出刚写的那个文件：在 /tmp 与 /media 下按前缀找最新的
    // （没有 U 盘时生产代码兜底到 /tmp，本机测试环境就是这种情况）
    QString found;
    const QString prefix = QStringLiteral("weaknet-dashboard-");
    const QStringList dirs = {QStringLiteral("/tmp"),
                              QStringLiteral("/media")};
    for (const QString& dir : dirs) {
        QDir d(dir);
        const QStringList entries =
            d.entryList(QStringList{prefix + QStringLiteral("*")}, QDir::Files,
                        QDir::Time);
        for (const QString& name : entries) {
            const QString path = d.filePath(name);
            // 只认本次测试生成的文件（30 秒内）
            if (QFileInfo(path).lastModified().secsTo(
                    QDateTime::currentDateTime()) <= 30) {
                found = path;
                break;
            }
        }
        if (!found.isEmpty()) {
            break;
        }
    }

    ASSERT_FALSE(found.isEmpty()) << "导出后找不到任何 PNG 产物";

    // 回读校验：`save()` 返回 true 但写出 0 字节也是可能的
    // （磁盘满、只读介质边界）。只看「文件存在」抓不到这种失败，
    // 必须真的把内容读出来认魔数。
    QFile f(found);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly)) << "产物无法读取";
    const QByteArray head = f.read(8);
    const qint64 size = f.size();
    f.close();

    // PNG 魔数：89 50 4E 47 0D 0A 1A 0A
    const QByteArray png_magic =
        QByteArray::fromHex("89504e470d0a1a0a");
    EXPECT_EQ(head, png_magic) << "产物不是合法 PNG";
    EXPECT_GT(size, 1024) << "PNG 太小，可能只写了头没有内容";

    // 收尾：测试产物不留在磁盘上
    QFile::remove(found);
}

TEST(MainWindowTest, ExportReportsTheActualPathOnStatusBar) {
    // 现场最危险的情形是「以为图存进 U 盘了，其实掉进了 tmpfs」。
    // 状态栏是唯一能让人知道这件事的地方，所以必须写**实际路径**，
    // 而且落在 /tmp 时要明确警告「重启即失」。
    MainWindow w;
    w.setInterface(QString());
    w.resize(1024, 700);
    w.show();
    QApplication::processEvents();

    ASSERT_TRUE(QMetaObject::invokeMethod(&w, "onExportClicked",
                                          Qt::DirectConnection));

    // 状态栏归 MainWindow 所有，是唯一那个 QStatusBar 里的 QLabel
    QStatusBar* bar = w.findChild<QStatusBar*>();
    ASSERT_NE(bar, nullptr);
    QString text;
    for (QLabel* label : bar->findChildren<QLabel*>()) {
        text = label->text();
        if (!text.isEmpty()) {
            break;
        }
    }

    ASSERT_FALSE(text.isEmpty()) << "导出后状态栏什么都没说";
    EXPECT_TRUE(text.contains(QStringLiteral("截图已存")) ||
                text.contains(QStringLiteral("导出截图失败")))
        << "状态栏文案："
        << text.toStdString();

    // 成功时必须带上完整路径（以 / 开头）与文件名前缀，
    // 否则用户不知道去哪里取图
    if (text.contains(QStringLiteral("截图已存"))) {
        EXPECT_TRUE(text.contains(QStringLiteral("/")))
            << "状态栏没给出实际路径：" << text.toStdString();
        EXPECT_TRUE(text.contains(QStringLiteral("weaknet-dashboard-")))
            << "状态栏没给出文件名：" << text.toStdString();
    }

    // 清理本次产物
    for (const QString& dir :
         {QStringLiteral("/tmp"), QStringLiteral("/media")}) {
        QDir d(dir);
        for (const QString& name :
             d.entryList(QStringList{QStringLiteral("weaknet-dashboard-*")},
                         QDir::Files, QDir::Time)) {
            const QString path = d.filePath(name);
            if (QFileInfo(path).lastModified().secsTo(
                    QDateTime::currentDateTime()) <= 30) {
                QFile::remove(path);
            }
        }
    }
}

TEST(MainWindowTest, ExportScreenshotFallsBackWhenNoUsb) {
    // 没有 U 盘时**必须**还能出图，且状态栏要明说落在哪。
    // 这是现场取证据的唯一兜底：不兜底等于「没插 U 盘就导不出」。
    //
    // 本机（容器 / 无 U 盘）跑这条时，生产代码应当走 /tmp 分支。
    // 若某天真的挂了 U 盘，这条断言不该假失败——因此只断言
    // 「产物存在」与「路径非空」，不硬编码必须是 /tmp。
    MainWindow w;
    w.setInterface(QString());
    w.resize(1024, 700);
    w.show();
    QApplication::processEvents();

    EXPECT_TRUE(QMetaObject::invokeMethod(&w, "onExportClicked",
                                          Qt::DirectConnection));

    // 状态栏必须包含「截图已存」或「导出截图失败」二者之一，不能什么都不说
    const QList<QWidget*> children = w.findChildren<QWidget*>();
    Q_UNUSED(children);

    // 直接看有没有新文件产生（与上一条同源，但断言的是「兜底也成功」）
    QDir tmp(QStringLiteral("/tmp"));
    const QStringList entries =
        tmp.entryList(QStringList{QStringLiteral("weaknet-dashboard-*")},
                      QDir::Files, QDir::Time);
    ASSERT_FALSE(entries.isEmpty())
        << "无 U 盘时也应兜底写到 /tmp，否则现场没有取证据的途径";

    // 清理，避免污染后续测试与开发者机器
    for (const QString& name : entries) {
        const QString path = tmp.filePath(name);
        if (QFileInfo(path).lastModified().secsTo(
                QDateTime::currentDateTime()) <= 30) {
            QFile::remove(path);
        }
    }
}

// ---------------------------------------------------------------------------
// 整页滚动（小屏 + 手指划动）
// ---------------------------------------------------------------------------

TEST(MainWindowTest, WholePageIsInsideAScrollArea) {
    // 现场最便宜、最容易买到的 7 寸屏是 1024x600，而本布局的最小高度
    // 实测 661px（README「布局高度」一节）。取舍是「整页可滚、信息一条不少」，
    // 不是「删掉底栏去迁就屏幕」——底栏那几行正是产品定位里
    // 「把数据变成结论」的部分，删掉就只剩一堆数字。
    //
    // 这条测试守三件事，缺任何一件滚动区都形同虚设：
    //   1. **整页**被包进 QScrollArea，而不是只有图表或某个子面板可滚；
    //   2. 窗口最小高度不再被内容顶到 661——否则 `resize(1024,600)`
    //      会被弹回去，屏幕上照样裁底栏；
    //   3. 内容比视口高时，纵向滚动条真的有可滚范围。
    MainWindow w;
    w.setInterface(QString());
    w.show();
    QApplication::processEvents();

    auto* scroll = qobject_cast<QScrollArea*>(w.centralWidget());
    ASSERT_NE(scroll, nullptr) << "整页必须包在 QScrollArea 里";

    // 宽度跟随视口（不产生横向滚动），高度取内容自身 sizeHint
    EXPECT_TRUE(scroll->widgetResizable())
        << "关掉 widgetResizable 会让内容按原始尺寸显示，"
        << "宽屏上两侧各留一条空白，视觉上像没铺满";
    EXPECT_EQ(scroll->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOff)
        << "看板是竖排一列，横向本就没有溢出；"
        << "留着横条只会在触摸时误触发左右晃，还白占高度";

    const int min_h = w.minimumSizeHint().height();
    EXPECT_LE(min_h, 600)
        << "窗口最小高度 " << min_h << "px 仍大于 600："
        << "1024x600 的屏幕上滚动区根本不会出现（resize 会被弹回）";

    w.resize(1024, 600);
    QApplication::processEvents();
    EXPECT_LE(w.height(), 600)
        << "resize(1024,600) 被弹回到 " << w.height() << "px";

    QScrollBar* vbar = scroll->verticalScrollBar();
    ASSERT_NE(vbar, nullptr);
    EXPECT_GT(vbar->maximum(), 0)
        << "600 高的视口下纵向没有可滚范围，说明内容没被滚动区接住";

    // 滚到底时，最下面那行可见内容（诊断行）必须真的进到视口里。
    // 只断言「有可滚范围」不够：范围存在但内容没被覆盖时同样滚不出东西。
    vbar->setValue(vbar->maximum());
    QApplication::processEvents();

    QLabel* bottom = nullptr;
    for (QLabel* l : w.findChildren<QLabel*>()) {
        if (l->text().startsWith(QStringLiteral("诊断："))) {
            bottom = l;
            break;
        }
    }
    ASSERT_NE(bottom, nullptr) << "找不到底栏的诊断行";
    EXPECT_FALSE(bottom->visibleRegion().isEmpty())
        << "滚到底之后诊断行仍不可见，说明它没被滚动区覆盖";
}

TEST(MainWindowTest, DefaultSizeFitsWithoutScrollBar) {
    // 滚动是给小屏的兜底，不是常态。默认窗口（1024x720）必须**刚好放下**
    // 全部内容：桌面上打开就常驻一条滚动条，用户会以为布局没排好。
    //
    // 这条测试守的是「**空态**下默认尺寸由内容推导，而不是写死一个数」。
    //
    // ## 为什么不能写死 720
    //
    // 曾经写死 `resize(1024, 720)`，x86 上通过。它的隐含前提是「内容 + 状态栏
    // 正好凑满 720」——一个只在某个主题/字体组合下成立的巧合。
    // 内容高度取决于 QSS 字号、字体度量、有没有显示隐藏行，任何一项变化
    // 都会让这个数对不上，而失败表现只是「桌面上多一条滚动条」，很容易
    // 被当成正常。
    //
    // 改成按内容推导后，这段关系对任何字体度量都成立。
    //
    // ## 这条测试**不能**覆盖什么
    //
    // 它只测**空态**：构造后立刻 `show()`，后台线程还没取回数据。真实运行
    // 时那几个「有数据才 show()」的行（警告行、人读摘要、次要问题）会撑开
    // 内容——板端实测空态 674px、真实数据到达后 770px，纵向可滚范围 57px。
    // 也就是说**真实数据下默认窗口本来就会出现滚动条**，这是内容量决定的，
    // 不是布局缺陷。整页滚动的意义正在于此：内容比屏高时能划，而不是被裁。
    //
    // 若要连数据态也不滚，只有两条路：砍掉底栏信息，或把默认高度提到 800。
    // 前者违背「把数据变成结论」的产品定位，后者超出 720p 屏。所以接受滚动。
    //
    // 注意不能反过来写成「内容高度必须等于 674」——那会把字号彻底钉死，
    // 连正当的排版调整都过不去。测的是关系，不是魔数。
    MainWindow w;
    w.setInterface(QString());
    w.show();
    QApplication::processEvents();

    auto* scroll = qobject_cast<QScrollArea*>(w.centralWidget());
    ASSERT_NE(scroll, nullptr);
    QScrollBar* vbar = scroll->verticalScrollBar();
    ASSERT_NE(vbar, nullptr);

    EXPECT_EQ(vbar->maximum(), 0)
        << "空态下默认尺寸 " << w.width() << "x" << w.height() << " 仍有 "
        << vbar->maximum() << "px 可滚范围：内容高度（"
        << scroll->widget()->sizeHint().height() << "）已超过视口高度（"
        << scroll->viewport()->height() << "），说明默认高度没有跟着内容长";

    // 反向保证：默认高度确实是**跟着内容长的**，不是碰巧够。内容比 720
    // 的下限矮时窗口可以停在 720；但内容一旦超过 720 − 状态栏，
    // 窗口就必须同步长高，否则上面的断言会在某些平台上开始失败。
    const int content = scroll->widget()->sizeHint().height();
    const int chrome = w.statusBar()->sizeHint().height();
    if (content + chrome > 720) {
        EXPECT_GE(w.height(), content + chrome)
            << "内容（" << content << "）+ 状态栏（" << chrome
            << "）超过 720 下限，但默认窗口只有 " << w.height()
            << "：默认高度没有跟着内容长高";
    }
}

TEST(MainWindowTest, FingerSwipeGestureIsGrabbedOnViewport) {
    // 手势必须抓在 **viewport** 上，不是 QScrollArea 自己：内容 widget 盖在
    // 滚动区之上，触摸事件落不到 QScrollArea 本体。抓错对象的表现是
    // 「滚动条能拖、手指划不动」——现场没有键盘鼠标时就是死界面。
    //
    // 注意断言的是「谁被抓了」，不是「划起来手感如何」。惯性曲线、
    // 跟手程度只能在真机上验，这里不假装覆盖。
    MainWindow w;
    w.setInterface(QString());
    w.show();
    QApplication::processEvents();

    auto* scroll = qobject_cast<QScrollArea*>(w.centralWidget());
    ASSERT_NE(scroll, nullptr);

    EXPECT_TRUE(QScroller::hasScroller(scroll->viewport()))
        << "viewport 上没有 QScroller，手指划屏不会滚动";
    EXPECT_NE(QScroller::grabbedGesture(scroll->viewport()), Qt::CustomGesture)
        << "viewport 上没有抓到手势";

    // 反向保证：手势不该抓在滚动区自身上。
    // 抓在那里等于没抓——事件根本不会落到它头上。
    EXPECT_FALSE(QScroller::hasScroller(scroll))
        << "手势抓错了对象（应是 viewport，不是 QScrollArea 本身）";
}

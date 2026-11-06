/**
 * @file test_line_chart_widget.cpp
 * @brief LineChartWidget 的像素级测试
 *
 * ## 为什么这些测试存在
 *
 * `LineChartWidget::paint(QPainter*, QRect)` 之所以与 `paintEvent()` 分离、
 * 之所以不读 `this->width()`，**唯一动机就是能在没有显示器的环境里对
 * 任意 QPainter 做像素断言**。如果一直没有这样的测试，那个分离设计本身
 * 就是一个只有测试才会用的孤儿接口。
 *
 * 这里就是那个消费者：把控件画到 `QImage`，然后对像素做可复算的断言。
 * 不需要 HDMI、不需要显示窗口、不需要开发板。
 *
 * ## 判据为什么这么写（踩过的坑）
 *
 * 上一轮用一次性探针验证 `legacy` 断开连线时，第一版判据「中间列无绿色」
 * 假失败过：它把 Y 轴刻度文字的抗锯齿边缘也算进去了（`0x88` 混白后
 * 恰好落在 `0x9E`）。
 *
 * 因此这里遵守两条规则：
 *
 * 1. **只统计绘图区内部**，不含坐标轴文字所在的行列；
 * 2. **每条断言都配对照组**，同一进程内只改一个变量跑两次。
 *    单点断言没有对照组时，恒真条件也会「通过」。
 */

#include <gtest/gtest.h>

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QPainter>
#include <QString>

#include <cstdlib>
#include <string>

#include "data/dashboard_data.hpp"
#include "ui/line_chart_widget.hpp"

namespace {

using weaknet::dashboard::HistoryPoint;
using weaknet::dashboard::HistorySeries;
using weaknet::dashboard::LineChartWidget;

/**
 * @brief 全局 QApplication
 *
 * `QWidget` 的构造要求进程里先有 `QApplication`（`QGuiApplication` 不够：
 * 会报 "QWidget: Cannot create a QWidget without QApplication"）。
 * 用 gtest 的全局环境在第一个测试之前建、最后一个测试之后销毁，
 * 避免在每个测试里重复构造（Qt 不允许）。
 *
 * 注意必须用 `QApplication` 而不是 `QGuiApplication`：这条坑上一轮已经踩过。
 */
class QtAppEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        // 保留 argc/argv 的引用语义：QApplication 要求它们活到析构。
        // 这里给一个只含程序名的静态 argv 即可。
        static int argc = 1;
        static char arg0[] = "test_line_chart_widget";
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

// 注册：gtest_main 会自动接管这些环境
[[maybe_unused]] const ::testing::Environment* const g_qt_env =
    ::testing::AddGlobalTestEnvironment(new QtAppEnvironment);

/// 与 line_chart_widget.cpp 的常量保持一致（测试要能独立算出绘图区）
constexpr int kMarginLeft = 44;
constexpr int kMarginRight = 12;
constexpr int kMarginTop = 28;
constexpr int kMarginBottom = 24;

/// 画布尺寸：足够大，保证绘图区是一个有意义的矩形
constexpr int kWidth = 400;
constexpr int kHeight = 200;

/// 折线颜色（line_chart_widget.cpp 的 line_color_ 默认值 #2E7D32）
constexpr int kLineR = 0x2E;
constexpr int kLineG = 0x7D;
constexpr int kLineB = 0x32;

/// legacy 空心点颜色 #9E9E9E
constexpr int kLegacyGray = 0x9E;

/// 绘图区（与 LineChartWidget::paint() 内的公式一致）
QRect plot_rect() {
    return QRect(kMarginLeft, kMarginTop, kWidth - kMarginLeft - kMarginRight,
                 kHeight - kMarginTop - kMarginBottom);
}

/// 构造一个 n 点、等比递增的序列
HistorySeries make_series(int n, bool all_legacy = false) {
    HistorySeries series;
    series.points.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        HistoryPoint p;
        p.ts = "2026-11-06 12:00:0" + std::to_string(i);
        p.score = 20.0 + static_cast<double>(i) * 5.0;
        p.legacy = all_legacy;
        series.points.push_back(std::move(p));
    }
    return series;
}

/// 把控件画到固定尺寸的 QImage 上
QImage render(LineChartWidget& chart, const QString& title = QString()) {
    QImage img(kWidth, kHeight, QImage::Format_RGB32);
    img.fill(Qt::white);
    chart.setTitle(title);

    QPainter painter(&img);
    painter.setRenderHint(QPainter::Antialiasing, true);
    chart.paint(&painter, img.rect());
    painter.end();
    return img;
}

/// 两条 RGB 是否接近（容忍抗锯齿带来的通道偏差）
bool close_to(int r, int g, int b, int tr, int tg, int tb, int tol = 28) {
    return std::abs(r - tr) <= tol && std::abs(g - tg) <= tol &&
           std::abs(b - tb) <= tol;
}

bool is_line_pixel(const QColor& c) {
    return close_to(c.red(), c.green(), c.blue(), kLineR, kLineG, kLineB);
}

bool is_legacy_pixel(const QColor& c) {
    return close_to(c.red(), c.green(), c.blue(),
                    kLegacyGray, kLegacyGray, kLegacyGray);
}

/// 统计绘图区内部满足条件的像素数。**刻意排除坐标轴文字区域**
int count_in_plot(const QImage& img, bool want_line, bool want_gray) {
    const QRect plot = plot_rect();
    int count = 0;
    for (int y = plot.top() + 1; y <= plot.bottom() - 1; ++y) {
        for (int x = plot.left() + 1; x <= plot.right() - 1; ++x) {
            const QColor c = img.pixelColor(x, y);
            if (want_line && is_line_pixel(c)) {
                ++count;
            }
            if (want_gray && is_legacy_pixel(c)) {
                ++count;
            }
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// 基础渲染
// ---------------------------------------------------------------------------

TEST(LineChartWidgetTest, EmptySeriesShowsNoLine) {
    LineChartWidget chart;
    const QImage img = render(chart);

    // 没有数据时必须画提示文字，且绝不能画折线
    EXPECT_EQ(count_in_plot(img, /*want_line=*/true, /*want_gray=*/false), 0)
        << "空序列不应画出折线";
}

TEST(LineChartWidgetTest, NonEmptySeriesDrawsLine) {
    LineChartWidget chart;
    chart.setSeries(make_series(10));
    const QImage img = render(chart);

    // 对照组：非空序列必须真的画出折线像素
    EXPECT_GT(count_in_plot(img, /*want_line=*/true, /*want_gray=*/false), 0)
        << "非空序列必须画出折线";
}

// ---------------------------------------------------------------------------
// legacy 断开连线（最关键的一条）
// ---------------------------------------------------------------------------

TEST(LineChartWidgetTest, LegacyBreaksTheLineInTheMiddle) {
    // 三个点，中间那个标为 legacy（v1 旧模型）。
    //
    // 对照设计：唯一变量是中间点的 legacy 标志，其余完全相同。
    //   - 非 legacy：路径连续穿过中间列 → 中间列有折线像素
    //   - legacy：  路径在中间断开 → 中间列没有折线像素
    //
    // 三点水平等距，中间点正好在绘图区水平中心。
    const QRect plot = plot_rect();
    const int mid_x = plot.left() + plot.width() / 2;
    constexpr int kMidTol = 2;  // 折线宽 2px + 圆点半径 2.5px，±2 足够

    auto count_line_near_mid_column = [&](const QImage& img) {
        int count = 0;
        for (int y = plot.top() + 1; y <= plot.bottom() - 1; ++y) {
            for (int dx = -kMidTol; dx <= kMidTol; ++dx) {
                const int x = mid_x + dx;
                if (x <= plot.left() || x >= plot.right()) {
                    continue;
                }
                if (is_line_pixel(img.pixelColor(x, y))) {
                    ++count;
                }
            }
        }
        return count;
    };

    // --- 对照组：全部 v2，连线穿过中间列 ---
    LineChartWidget control;
    control.setSeries(make_series(3, /*all_legacy=*/false));
    const QImage control_img = render(control);

    // --- 实验组：中间点 legacy，连线断开 ---
    HistorySeries mixed = make_series(3, /*all_legacy=*/false);
    mixed.points[1].legacy = true;

    LineChartWidget subject;
    subject.setSeries(mixed);
    const QImage subject_img = render(subject);

    const int control_mid = count_line_near_mid_column(control_img);
    const int subject_mid = count_line_near_mid_column(subject_img);

    EXPECT_GT(control_mid, 0)
        << "对照组：三个 v2 点应连成一条穿过中间列的线";
    EXPECT_EQ(subject_mid, 0)
        << "中间点标为 legacy 后，连线必须在该处断开";

    // 两侧的线还在：证明只在 legacy 处断开，而不是整条线都没画
    EXPECT_GT(count_in_plot(subject_img, /*want_line=*/true, /*want_gray=*/false), 0)
        << "legacy 只断开它自己那一段，其余线段必须仍然存在";
}

TEST(LineChartWidgetTest, LegacyPointsAreStillMarked) {
    // legacy 点不在折线上。若再不给标记就等于从图上凭空消失，
    // 比画错更糟——用户会以为那几个采样根本不存在。
    LineChartWidget chart;
    chart.setSeries(make_series(3, /*all_legacy=*/true));
    const QImage img = render(chart);

    // 全 legacy 时不应有任何折线
    EXPECT_EQ(count_in_plot(img, /*want_line=*/true, /*want_gray=*/false), 0)
        << "全 legacy 序列不应画出折线";

    // 但必须有灰色标记
    EXPECT_GT(count_in_plot(img, /*want_line=*/false, /*want_gray=*/true), 0)
        << "legacy 点必须留下灰色空心标记，不能静默消失";
}

// ---------------------------------------------------------------------------
// 几何与坐标变换
// ---------------------------------------------------------------------------

TEST(LineChartWidgetTest, SinglePointRendersWithoutDivisionByZero) {
    // n == 1 时 x_ratio 被特意设为 0.5，避免除零。
    // 这类边界不值得为它崩一次界面。
    LineChartWidget chart;
    chart.setSeries(make_series(1));
    const QImage img = render(chart);

    EXPECT_GT(count_in_plot(img, /*want_line=*/true, /*want_gray=*/false), 0)
        << "单点也必须画出来（画成圆点，不画线）";
}

TEST(LineChartWidgetTest, FlatSeriesDoesNotCollapseToEdge) {
    // 全等值序列：hi == lo，y_ratio 会退化成 0.5。
    // 若没有 span 兜底，取值会落到绘图区顶部或底部，看起来像"贴顶"。
    HistorySeries flat;
    for (int i = 0; i < 5; ++i) {
        HistoryPoint p;
        p.ts = "2026-11-06 12:00:0" + std::to_string(i);
        p.score = 55.0;
        flat.points.push_back(std::move(p));
    }

    LineChartWidget chart;
    chart.setSeries(flat);
    const QImage img = render(chart);

    // 折线应出现在绘图区的垂直中部，而不是最上一行或最下一行
    const QRect plot = plot_rect();
    const int mid_y = plot.top() + plot.height() / 2;
    constexpr int kTol = 6;

    int near_mid = 0;
    for (int y = mid_y - kTol; y <= mid_y + kTol; ++y) {
        for (int x = plot.left() + 1; x <= plot.right() - 1; ++x) {
            if (is_line_pixel(img.pixelColor(x, y))) {
                ++near_mid;
            }
        }
    }
    EXPECT_GT(near_mid, 0) << "全等值序列应画在垂直中部";
}

TEST(LineChartWidgetTest, LegacyVectorStaysInSyncWithScores) {
    // legacy_ 与 scores_ 必须等长：paint() 里用 legacy_.at(i) 索引，
    // 长度不一致会越界。连续换两次数据是这个不变量的最容易破的时刻。
    LineChartWidget chart;
    chart.setSeries(make_series(10));
    chart.setSeries(make_series(3, /*all_legacy=*/true));
    const QImage img = render(chart);

    // 换过数据后不应崩，且只能看到 3 个 legacy 点、没有折线
    EXPECT_EQ(count_in_plot(img, /*want_line=*/true, /*want_gray=*/false), 0);
    EXPECT_GT(count_in_plot(img, /*want_line=*/false, /*want_gray=*/true), 0);
}

TEST(LineChartWidgetTest, DegenerateAreaIsIgnored) {
    // paint() 对空矩形必须安全返回，不能崩。
    // paintEvent 在窗口最小化等情况下可能给出极小的 rect。
    LineChartWidget chart;
    chart.setSeries(make_series(5));

    QImage img(10, 10, QImage::Format_RGB32);
    img.fill(Qt::white);
    QPainter painter(&img);
    chart.paint(&painter, QRect(0, 0, 0, 0));
    chart.paint(&painter, QRect(0, 0, 5, 5));   // 比边距还小
    painter.end();

    SUCCEED();  // 没崩就算通过
}

} // namespace

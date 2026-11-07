/**
 * @file line_chart_widget.cpp
 * @brief LineChartWidget 实现
 */

#include "line_chart_widget.hpp"

#include <QFontMetrics>
#include <QLinearGradient>
#include <QPainter>
#include <QPaintEvent>
#include <QPainterPath>
#include <QPair>
#include <QPen>

#include <algorithm>
#include <limits>

#include "theme.hpp"

namespace weaknet {
namespace dashboard {

namespace {

/// 绘图区相对控件边缘的留白（为坐标轴标签留空间）
constexpr int kMarginLeft = 44;
constexpr int kMarginRight = 12;
constexpr int kMarginTop = 28;
constexpr int kMarginBottom = 24;

/// 网格线数量
constexpr int kGridLines = 4;

/// 折线下方的渐变填充顶端不透明度（0-255）
///
/// 刻意压得很低：这块填充只是给曲线一个「体量感」，不能抢走线本身的注意力，
/// 也不能深到被像素测试当成折线。alpha=56 时混白后是 (217,231,218)，
/// 与折线色 (46,125,50) 的红通道差 171，远超测试的 28 容差。
constexpr int kAreaFillAlpha = 56;

QColor theme_color(const char* hex) {
    return QColor(QString::fromLatin1(hex));
}

} // namespace

LineChartWidget::LineChartWidget(QWidget* parent) : QWidget(parent) {
    // 让控件至少在布局里有合理的最小尺寸
    setMinimumSize(240, 140);
}

void LineChartWidget::setSeries(const HistorySeries& series) {
    scores_.clear();
    labels_.clear();
    legacy_.clear();
    scores_.reserve(static_cast<int>(series.points.size()));
    legacy_.reserve(static_cast<int>(series.points.size()));
    for (const HistoryPoint& p : series.points) {
        scores_.push_back(p.score);
        labels_.push_back(QString::fromStdString(p.ts));
        legacy_.push_back(p.legacy);
    }
    update();
}

void LineChartWidget::setTitle(const QString& title) {
    title_ = title;
    update();
}

void LineChartWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    paint(&painter, rect());
}

void LineChartWidget::paint(QPainter* painter, const QRect& area) const {
    if (!painter || area.width() <= 0 || area.height() <= 0) {
        return;
    }

    painter->save();

    // --- 标题 ---
    if (!title_.isEmpty()) {
        QFont f = painter->font();
        f.setBold(true);
        painter->setFont(f);
        painter->setPen(theme_color(ThemeColors::kTextPrimary));
        painter->drawText(QRect(area.left() + kMarginLeft, area.top() + 4,
                                area.width() - kMarginLeft - kMarginRight, kMarginTop - 6),
                          Qt::AlignLeft | Qt::AlignVCenter, title_);
        f.setBold(false);
        painter->setFont(f);
    }

    // --- 绘图区 ---
    QRect plot(area.left() + kMarginLeft,
               area.top() + kMarginTop,
               area.width() - kMarginLeft - kMarginRight,
               area.height() - kMarginTop - kMarginBottom);

    if (plot.width() <= 1 || plot.height() <= 1) {
        painter->restore();
        return;
    }

    // --- Y 轴范围 ---
    // 按数据 min/max 自适应，上下各留 10% 边距；全等值时给最小跨度避免除零。
    // 注意：小范围波动会被放大到撑满全高（如 20↔55 会铺满整个绘图区）。
    double lo = 0.0;
    double hi = 100.0;
    if (!scores_.isEmpty()) {
        lo = *std::min_element(scores_.constBegin(), scores_.constEnd());
        hi = *std::max_element(scores_.constBegin(), scores_.constEnd());
        const double span = hi - lo;
        if (span < 1e-6) {
            lo -= 5.0;
            hi += 5.0;
        } else {
            const double pad = span * 0.1;
            lo -= pad;
            hi += pad;
        }
    }

    // --- 网格 + Y 轴刻度 ---
    painter->setPen(QPen(theme_color(ThemeColors::kGrid), 1, Qt::SolidLine));
    QFont small = painter->font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() - 1.0));
    painter->setFont(small);

    for (int i = 0; i <= kGridLines; ++i) {
        const double ratio = static_cast<double>(i) / kGridLines;
        const int y = plot.bottom() - static_cast<int>(ratio * plot.height());

        painter->setPen(QPen(theme_color(ThemeColors::kGrid), 1, Qt::SolidLine));
        painter->drawLine(plot.left(), y, plot.right(), y);

        const double value = lo + ratio * (hi - lo);
        painter->setPen(theme_color(ThemeColors::kTextMuted));
        painter->drawText(QRect(area.left(), y - 8, kMarginLeft - 6, 16),
                          Qt::AlignRight | Qt::AlignVCenter,
                          QString::number(value, 'f', 0));
    }

    // --- 数据为空时的提示 ---
    if (scores_.isEmpty()) {
        painter->setPen(theme_color(ThemeColors::kTextMuted));
        painter->drawText(plot, Qt::AlignCenter, QStringLiteral("暂无历史数据"));
        painter->restore();
        return;
    }

    // --- 折线 ---
    // 单点时画一个点，避免除零
    const int n = scores_.size();

    // 折线色从主题取：改进前这里是一个可写成员 `line_color_`，
    // 但 `setLineColor()` 作为孤儿接口被删掉后再没有人改它，
    // 留着成员只是把「常量」伪装成「状态」。现在直接取语义色。
    const QColor line_color = theme_color(ThemeColors::kGood);

    // 先把所有点换算成屏幕坐标，连线与画点共用同一份，避免两处公式漂移
    QVector<QPointF> pts;
    pts.reserve(n);
    for (int i = 0; i < n; ++i) {
        const double x_ratio =
            (n == 1) ? 0.5 : static_cast<double>(i) / static_cast<double>(n - 1);
        const double y_ratio = (hi - lo) < 1e-9 ? 0.5 : (scores_[i] - lo) / (hi - lo);
        pts.push_back(QPointF(plot.left() + x_ratio * plot.width(),
                              plot.bottom() - y_ratio * plot.height()));
    }

    // 只连 v2 点；遇到 legacy 点就断开。
    // legacy 是 v1 旧评分模型的分数（`data_version < 2`），与 v2 不同量纲，
    // 把两者连成一条线会读出一段根本不存在的趋势。
    //
    // 同时记下每个连续段的起止下标：下面的面积填充要按段闭合，
    // 跨段填充会在 legacy 处画出一块凭空多出来的三角。
    QPainterPath path;
    QVector<QPair<int, int>> runs;
    int run_begin = -1;
    for (int i = 0; i < n; ++i) {
        if (legacy_.at(i)) {
            if (run_begin >= 0) {
                runs.push_back(qMakePair(run_begin, i - 1));
                run_begin = -1;
            }
            continue;
        }
        if (run_begin < 0) {
            run_begin = i;
            path.moveTo(pts[i]);
        } else {
            path.lineTo(pts[i]);
        }
    }
    if (run_begin >= 0) {
        runs.push_back(qMakePair(run_begin, n - 1));
    }

    // --- 曲线下方的面积填充 ---
    //
    // 先填后画线，否则填充会盖住折线。透明度很低（见 kAreaFillAlpha），
    // 只给曲线一个「体量感」，让整体水平一眼可见，不抢折线本身。
    QLinearGradient fill_grad(0, plot.top(), 0, plot.bottom());
    QColor fill_top = line_color;
    fill_top.setAlpha(kAreaFillAlpha);
    QColor fill_bottom = line_color;
    fill_bottom.setAlpha(0);
    fill_grad.setColorAt(0.0, fill_top);
    fill_grad.setColorAt(1.0, fill_bottom);

    painter->save();
    painter->setClipRect(plot);
    painter->setPen(Qt::NoPen);
    painter->setBrush(fill_grad);
    for (const QPair<int, int>& run : runs) {
        // 单点没有面积，闭合后会得到一条零宽竖线，跳过
        if (run.second <= run.first) {
            continue;
        }
        QPainterPath area;
        area.moveTo(pts[run.first].x(), plot.bottom());
        for (int i = run.first; i <= run.second; ++i) {
            area.lineTo(pts[i]);
        }
        area.lineTo(pts[run.second].x(), plot.bottom());
        area.closeSubpath();
        painter->drawPath(area);
    }
    painter->restore();

    painter->setPen(QPen(line_color, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(path);

    // 数据点少于 30 个时补圆点，便于看清具体采样
    if (n <= 30) {
        painter->setBrush(line_color);
        painter->setPen(Qt::NoPen);
        for (int i = 0; i < n; ++i) {
            if (!legacy_.at(i)) {
                painter->drawEllipse(pts[i], 2.5, 2.5);
            }
        }
    }

    // legacy 点**始终**标记（灰色空心）：它们不在折线上，若再不给标记就等于
    // 从图上凭空消失，比画错更糟。空心也让「不是同一条趋势」一眼可辨。
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(theme_color(ThemeColors::kUnknown), 1.5));
    for (int i = 0; i < n; ++i) {
        if (legacy_.at(i)) {
            painter->drawEllipse(pts[i], 2.5, 2.5);
        }
    }

    // --- X 轴首尾时间标签 ---
    if (!labels_.isEmpty()) {
        painter->setPen(theme_color(ThemeColors::kTextMuted));
        painter->drawText(QRect(plot.left(), plot.bottom() + 4,
                                plot.width() / 2, kMarginBottom - 4),
                          Qt::AlignLeft | Qt::AlignTop, labels_.first());
        if (n > 1) {
            painter->drawText(QRect(plot.left() + plot.width() / 2, plot.bottom() + 4,
                                    plot.width() / 2, kMarginBottom - 4),
                              Qt::AlignRight | Qt::AlignTop, labels_.last());
        }
    }

    painter->restore();
}

} // namespace dashboard
} // namespace weaknet

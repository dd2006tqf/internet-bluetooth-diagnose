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
#include <vector>

#include "chart_geometry.hpp"
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

/// 点数不超过这个值时补圆点，便于看清具体采样
constexpr int kDotThreshold = 30;

QColor theme_color(const char* hex) {
    return QColor(QString::fromLatin1(hex));
}

/// 把 Qt 矩形转换成几何层用的纯数值矩形
PlotRect to_plot_rect(const QRect& plot) {
    PlotRect out;
    out.left = plot.left();
    out.top = plot.top();
    out.width = plot.width();
    out.height = plot.height();
    return out;
}

} // namespace

LineChartWidget::LineChartWidget(QWidget* parent) : QWidget(parent) {
    // 让控件至少在布局里有合理的最小尺寸
    setMinimumSize(240, 140);
}

void LineChartWidget::setSeries(const HistorySeries& series) {
    series_ = series;
    rebuild_from_series();
    update();
}

void LineChartWidget::setMetric(HistoryMetric metric) {
    if (metric_ == metric) {
        return;
    }
    metric_ = metric;
    rebuild_from_series();
    update();
}

void LineChartWidget::rebuild_from_series() {
    points_.clear();
    labels_.clear();
    const int n = static_cast<int>(series_.points.size());
    points_.reserve(n);
    labels_.reserve(n);

    for (const HistoryPoint& p : series_.points) {
        PlotPoint point;
        point.value = p.value(metric_);

        // 未测量：服务端 null → 哨兵值。必须显式标出来，
        // 否则 0 与「没测到」在图上完全一样。
        point.absent = is_history_metric_absent(metric_, point.value);

        // legacy 只对 Score 有意义：它标的是「评分模型是旧的」。
        // RSSI / RTT 这类原始测量值不受评分模型换代影响，不该被断开。
        point.break_line = (metric_ == HistoryMetric::Score) && p.legacy;

        points_.push_back(point);
        labels_.push_back(QString::fromStdString(p.ts));
    }
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
    //
    // 指标名与单位由本控件拼进标题，而不是让调用方传完整字符串：
    // 那样 `setMetric()` 之后标题会停留在旧指标上，图和标题各说一套。
    // 现场终端最不能出的错就是「图上画的是 RSSI，标题写着综合分」。
    if (!title_.isEmpty()) {
        QString title = title_;
        title += QStringLiteral(" · ");
        title += QString::fromUtf8(history_metric_label(metric_));
        const char* unit = history_metric_unit(metric_);
        if (unit && unit[0] != '\0') {
            title += QStringLiteral(" (") + QString::fromUtf8(unit) +
                     QStringLiteral(")");
        }

        QFont f = painter->font();
        f.setBold(true);
        painter->setFont(f);
        painter->setPen(theme_color(ThemeColors::kTextPrimary));
        painter->drawText(QRect(area.left() + kMarginLeft, area.top() + 4,
                                area.width() - kMarginLeft - kMarginRight,
                                kMarginTop - 6),
                          Qt::AlignLeft | Qt::AlignVCenter, title);
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
    //
    // 只喂**有效值**给轴计算：未测量点的哨兵（-1 / -1000）若混进去，
    // RSSI 的轴会被拉到 -1000，真实数据全挤成一条贴顶的线。
    std::vector<double> valid_values;
    valid_values.reserve(static_cast<size_t>(points_.size()));
    for (const PlotPoint& p : points_) {
        if (!p.absent) {
            valid_values.push_back(p.value);
        }
    }

    const AxisRange range = compute_axis_range(valid_values, metric_);
    const PlotRect plot_rect = to_plot_rect(plot);

    // --- 网格 + Y 轴刻度 ---
    painter->setPen(QPen(theme_color(ThemeColors::kGrid), 1, Qt::SolidLine));
    QFont small = painter->font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() - 1.0));
    painter->setFont(small);

    const int decimals = axis_label_decimals(metric_);
    for (int i = 0; i <= kGridLines; ++i) {
        const double ratio = static_cast<double>(i) / kGridLines;
        const int y = plot.bottom() - static_cast<int>(ratio * plot.height());

        painter->setPen(QPen(theme_color(ThemeColors::kGrid), 1, Qt::SolidLine));
        painter->drawLine(plot.left(), y, plot.right(), y);

        const double value = range.lo + ratio * (range.hi - range.lo);
        painter->setPen(theme_color(ThemeColors::kTextMuted));
        painter->drawText(QRect(area.left(), y - 8, kMarginLeft - 6, 16),
                          Qt::AlignRight | Qt::AlignVCenter,
                          QString::number(value, 'f', decimals));
    }

    // --- 数据为空时的提示 ---
    //
    // 两种「空」要分开说，否则用户会去查服务端，而实际上只是这个指标
    // 从来没被采集过（如没有射频接口时的 RSSI）。
    if (points_.isEmpty() || valid_values.empty()) {
        painter->setPen(theme_color(ThemeColors::kTextMuted));
        const QString message =
            points_.isEmpty()
                ? QStringLiteral("暂无历史数据")
                : QStringLiteral("该指标无历史数据（%1 个采样点均未测量）")
                      .arg(static_cast<int>(points_.size()));
        painter->drawText(plot, Qt::AlignCenter, message);
        painter->restore();
        return;
    }

    // --- 折线 ---
    const int n = points_.size();

    // 折线色从主题取：改进前这里是一个可写成员 `line_color_`，
    // 但 `setLineColor()` 作为孤儿接口被删掉后再没有人改它，
    // 留着成员只是把「常量」伪装成「状态」。现在直接取语义色。
    const QColor line_color = theme_color(ThemeColors::kGood);

    // 先把所有点换算成屏幕坐标，连线与画点共用同一份，避免两处公式漂移。
    // 公式本身在 chart_geometry.cpp 里，可脱离 Qt 单测。
    QVector<QPointF> pts;
    pts.reserve(n);
    for (int i = 0; i < n; ++i) {
        const ChartPoint mapped = map_point(static_cast<size_t>(i),
                                            static_cast<size_t>(n),
                                            points_[i].value, range, plot_rect);
        pts.push_back(QPointF(mapped.x, mapped.y));
    }

    // 遇到断点就断开。两种断点：
    //   1. `break_line`（legacy 旧评分模型点）——分数与 v2 不同量纲，
    //      连成一条线会读出一段根本不存在的趋势
    //   2. `absent`（该指标未测量）——没有值可画，连过去就是凭空造数
    //
    // 同时记下每个连续段的起止下标：下面的面积填充要按段闭合，
    // 跨段填充会在断点处画出一块凭空多出来的三角。
    QPainterPath path;
    QVector<QPair<int, int>> runs;
    int run_begin = -1;
    for (int i = 0; i < n; ++i) {
        if (points_[i].absent || points_[i].break_line) {
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
        QPainterPath area_path;
        area_path.moveTo(pts[run.first].x(), plot.bottom());
        for (int i = run.first; i <= run.second; ++i) {
            area_path.lineTo(pts[i]);
        }
        area_path.lineTo(pts[run.second].x(), plot.bottom());
        area_path.closeSubpath();
        painter->drawPath(area_path);
    }
    painter->restore();

    painter->setPen(QPen(line_color, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(path);

    // 数据点少于阈值时补圆点，便于看清具体采样
    if (n <= kDotThreshold) {
        painter->setBrush(line_color);
        painter->setPen(Qt::NoPen);
        for (int i = 0; i < n; ++i) {
            if (!points_[i].absent && !points_[i].break_line) {
                painter->drawEllipse(pts[i], 2.5, 2.5);
            }
        }
    }

    // legacy 点**始终**标记（灰色空心）：它们不在折线上，若再不给标记就等于
    // 从图上凭空消失，比画错更糟。空心也让「不是同一条趋势」一眼可辨。
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(theme_color(ThemeColors::kUnknown), 1.5));
    for (int i = 0; i < n; ++i) {
        if (points_[i].break_line) {
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

/**
 * @file line_chart_widget.cpp
 * @brief LineChartWidget 实现
 */

#include "line_chart_widget.hpp"

#include <QFontMetrics>
#include <QPainter>
#include <QPaintEvent>
#include <QPainterPath>
#include <QPen>

#include <algorithm>
#include <limits>

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
        painter->setPen(QColor(0x33, 0x33, 0x33));
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
    painter->setPen(QPen(QColor(0xE0, 0xE0, 0xE0), 1, Qt::SolidLine));
    QFont small = painter->font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() - 1.0));
    painter->setFont(small);

    for (int i = 0; i <= kGridLines; ++i) {
        const double ratio = static_cast<double>(i) / kGridLines;
        const int y = plot.bottom() - static_cast<int>(ratio * plot.height());

        painter->setPen(QPen(QColor(0xE0, 0xE0, 0xE0), 1, Qt::SolidLine));
        painter->drawLine(plot.left(), y, plot.right(), y);

        const double value = lo + ratio * (hi - lo);
        painter->setPen(QColor(0x88, 0x88, 0x88));
        painter->drawText(QRect(area.left(), y - 8, kMarginLeft - 6, 16),
                          Qt::AlignRight | Qt::AlignVCenter,
                          QString::number(value, 'f', 0));
    }

    // --- 数据为空时的提示 ---
    if (scores_.isEmpty()) {
        painter->setPen(QColor(0x99, 0x99, 0x99));
        painter->drawText(plot, Qt::AlignCenter, QStringLiteral("暂无历史数据"));
        painter->restore();
        return;
    }

    // --- 折线 ---
    // 单点时画一个点，避免除零
    const int n = scores_.size();

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
    QPainterPath path;
    bool pen_down = false;
    for (int i = 0; i < n; ++i) {
        if (legacy_.at(i)) {
            pen_down = false;
            continue;
        }
        if (pen_down) {
            path.lineTo(pts[i]);
        } else {
            path.moveTo(pts[i]);
            pen_down = true;
        }
    }

    painter->setPen(QPen(line_color_, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(path);

    // 数据点少于 30 个时补圆点，便于看清具体采样
    if (n <= 30) {
        painter->setBrush(line_color_);
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
    painter->setPen(QPen(QColor(0x9E, 0x9E, 0x9E), 1.5));
    for (int i = 0; i < n; ++i) {
        if (legacy_.at(i)) {
            painter->drawEllipse(pts[i], 2.5, 2.5);
        }
    }

    // --- X 轴首尾时间标签 ---
    if (!labels_.isEmpty()) {
        painter->setPen(QColor(0x88, 0x88, 0x88));
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

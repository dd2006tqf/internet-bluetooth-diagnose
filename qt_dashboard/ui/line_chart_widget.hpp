/**
 * @file line_chart_widget.hpp
 * @brief QPainter 自绘折线图
 *
 * ## 为什么不用 Qt Charts
 *
 * 1. 板端**没装** `libqt5charts5`（实测 `dpkg -l | grep -i chart` 为空）
 * 2. `QChart` 在 Mesa 软件渲染下帧率差
 * 3. 自绘可以 `render()` 到 `QImage` 做像素级断言，**不需要显示器**
 *
 * 第 3 点是关键：开发板目前没接 HDMI 显示器，如果图表只能在屏幕上验证，
 * 这条路在硬件到手前就是死的。渲染到 QImage 让 UI 层也能在 x86 上单测。
 *
 * ## 绘制与控件分离
 *
 * `paint(QPainter*, QRect)` 是纯绘制函数，接收 QPainter 和矩形区域，
 * 不读 `this->width()`。`paintEvent()` 只是把 `rect()` 转给它。
 *
 * 这样测试可以：
 *
 * ```cpp
 * QImage img(400, 200, QImage::Format_RGB32);
 * img.fill(Qt::white);
 * QPainter p(&img);
 * chart.paint(&p, img.rect());
 * p.end();
 * // 断言 img 里出现了曲线颜色的像素
 * ```
 */

#ifndef WEAKNET_QT_DASHBOARD_LINE_CHART_WIDGET_HPP
#define WEAKNET_QT_DASHBOARD_LINE_CHART_WIDGET_HPP

#include <QColor>
#include <QString>
#include <QVector>
#include <QWidget>

#include "data/dashboard_data.hpp"

class QPainter;

namespace weaknet {
namespace dashboard {

/**
 * @brief 折线图控件
 *
 * 显示 `HistorySeries` 的分数随时间变化。
 *
 * 不持有数据的所有权，`setSeries()` 拷贝一份 QVector。
 */
class LineChartWidget : public QWidget {
    Q_OBJECT

public:
    explicit LineChartWidget(QWidget* parent = nullptr);

    /**
     * @brief 设置要绘制的数据
     *
     * 传入前应保证点已按时间正序排列（`parse_history()` 已做翻转）。
     *
     * `legacy` 数据点（v1 旧评分模型）会画成灰色空心点，且**不与相邻点连线**：
     * 旧模型与新模型的分数不在同一个量纲上，连成一条线会读出不存在的趋势。
     */
    void setSeries(const HistorySeries& series);

    void setTitle(const QString& title);

    /**
     * @brief 纯绘制函数
     *
     * 不依赖控件几何，可直接对任意 QPainter 调用。
     */
    void paint(QPainter* painter, const QRect& area) const;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QVector<double> scores_;
    QVector<QString> labels_;
    /// 与 scores_ 等长：该点是否为 v1 旧评分模型的数据
    QVector<bool> legacy_;
    QColor line_color_{0x2E, 0x7D, 0x32};
    QString title_;
};

} // namespace dashboard
} // namespace weaknet

#endif // WEAKNET_QT_DASHBOARD_LINE_CHART_WIDGET_HPP

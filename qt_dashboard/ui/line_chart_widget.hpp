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
 * 第 3 点是关键：在显示器到手前，如果图表只能在屏幕上验证，这条路就是死的。
 * 渲染到 QImage 让 UI 层也能在 x86 上单测。
 *
 * ## 绘制与控件分离
 *
 * `paint(QPainter*, QRect)` 是纯绘制函数，接收 QPainter 和矩形区域，
 * 不读 `this->width()`。`paintEvent()` 只是把 `rect()` 转给它。
 *
 * 这样测试可以对任意画布做像素断言，`tests/test_line_chart_widget.cpp`
 * 就是这么用的。
 *
 * **这个测试必须存在**。那个分离设计唯一动机就是可被像素测试；
 * 没有测试，它就是一个只有测试才会用的孤儿接口。
 *
 * ## 坐标计算为什么不在本文件
 *
 * 轴范围与坐标映射挪到了 `chart_geometry.{hpp,cpp}`（不依赖 Qt）。
 * 原因见那个文件的头注：算得对不对是纯算术，应该能直接断言，
 * 不该只能靠数像素去间接反推。
 */

#ifndef WEAKNET_QT_DASHBOARD_LINE_CHART_WIDGET_HPP
#define WEAKNET_QT_DASHBOARD_LINE_CHART_WIDGET_HPP

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
 * 显示 `HistorySeries` 的某个指标（默认为综合分）随时间变化。
 *
 * 不持有数据的所有权，`setSeries()` 拷贝一份。
 */
class LineChartWidget : public QWidget {
    Q_OBJECT

public:
    explicit LineChartWidget(QWidget* parent = nullptr);

    /**
     * @brief 设置要绘制的数据（保持当前指标）
     *
     * 传入前应保证点已按时间正序排列（`parse_history()` 已做翻转）。
     *
     * `legacy` 数据点（v1 旧评分模型）会画成灰色空心点，且**不与相邻点连线**：
     * 旧模型与新模型的分数不在同一个量纲上，连成一条线会读出不存在的趋势。
     *
     * 该语义**只在 Score 指标下有意义**：`legacy` 标记的是「评分模型是旧的」，
     * 它不说明 RSSI 这类原始测量值不可比。所以切到非 Score 指标时，
     * 所有点按正常点处理（见 `setMetric()`）。
     */
    void setSeries(const HistorySeries& series);

    /**
     * @brief 切换绘制的指标
     *
     * 只重算视图，不重新取数：原始序列已经缓存着。
     * 调用方（MainWindow）随后会触发一次刷新以取回对应指标的历史，
     * 但即便刷新失败，已有数据也能正确重画。
     */
    void setMetric(HistoryMetric metric);

    // 这里刻意**没有** `metric()` 只读访问器：当前没有任何消费者
    // （MainWindow 自己持有 current_metric_，测试断言的是那个）。
    // 而且它会遮蔽 QWidget::metric(PaintDeviceMetric) 这个虚函数，
    // 编译期警告 -Woverloaded-virtual，运行期谁写 `chart->metric(...)`
    // 都可能悄悄调到错的重载。要加访问器时请叫 currentMetric()。

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
    /**
     * @brief 一个待绘制的点
     *
     * ## 为什么把三个状态捏在一个结构里
     *
     * 改进前是三个平行 QVector（scores_ / labels_ / legacy_），要靠
     * `setSeries()` 里手写 push_back 保持等长，当时甚至有测试专门守这条
     * （`LegacyVectorStaysInSyncWithScores`，现已改名为
     * `RepeatedSetSeriesKeepsVectorsConsistent`）——需要写测试守的不变量，
     * 就说明设计上有可以误用的缝。
     *
     * 现在一个点就是一条记录，**结构上不可能不同步**。
     */
    struct PlotPoint {
        /// 指标值（absent 时无意义）
        double value = 0.0;
        /// 未测量：不画点、断开连线
        bool absent = false;
        /// 应断开连线但在图上留标记（legacy 旧模型点）
        bool break_line = false;
    };

    /// 用当前 metric_ 从 series_ 重算 points_ / labels_
    void rebuild_from_series();

    /// 原始序列（切换指标时不重新取数，只重算视图）
    HistorySeries series_;

    HistoryMetric metric_ = HistoryMetric::Score;

    /// 与原始序列**等长同序**：未测量点保留为 absent 记录，
    /// 不删除。删掉会让后面所有点的 x 坐标前移，把时间轴压扁成假趋势。
    QVector<PlotPoint> points_;
    QVector<QString> labels_;

    QString title_;
};

} // namespace dashboard
} // namespace weaknet

#endif // WEAKNET_QT_DASHBOARD_LINE_CHART_WIDGET_HPP

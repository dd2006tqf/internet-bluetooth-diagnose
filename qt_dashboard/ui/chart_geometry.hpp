/**
 * @file chart_geometry.hpp
 * @brief 折线图的坐标几何计算（**不依赖 Qt**）
 *
 * ## 为什么从 LineChartWidget 里拆出来
 *
 * 折线图的正确性有两类，混在一个函数里就都没法单独验证：
 *
 * | 关注点 | 出错的后果 | 能否脱离显示器验证 |
 * | --- | --- | --- |
 * | 像素画得对不对 | 颜色/线宽/抗锯齿 | 只能渲染成 QImage 看像素 |
 * | 坐标算得对不对 | 点画到错误高度、轴范围错、除零 | **纯算术，能直接断言** |
 *
 * `LineChartWidget::paint()` 把两者搅在一起，于是「RTT=150 应该画在轴中间」
 * 这种判断只能靠数像素来间接推断——一旦失败，你分不清是算错了还是画错了。
 *
 * 拆出本文件后：
 *   - 轴范围、坐标映射、刻度精度 → 纯函数单测，0 依赖
 *   - 颜色、线宽、抗锯齿 → 仍走 QImage 像素测试
 *
 * 本文件不 include 任何 Qt 头，因此可以在没有 Qt 开发包的机器上编译测试。
 */

#ifndef WEAKNET_QT_DASHBOARD_CHART_GEOMETRY_HPP
#define WEAKNET_QT_DASHBOARD_CHART_GEOMETRY_HPP

#include <cstddef>
#include <vector>

#include "data/dashboard_data.hpp"

namespace weaknet {
namespace dashboard {

/**
 * @brief 一个数据点映射到屏幕后的坐标
 *
 * 用 double 而不是 int：像素级取整由绘制端做，几何层保留精度，
 * 否则连续缩放时会累积误差。
 */
struct ChartPoint {
    double x = 0.0;
    double y = 0.0;
};

/**
 * @brief Y 轴取值范围
 */
struct AxisRange {
    double lo = 0.0;
    double hi = 1.0;

    /// 跨度是否足够大到能做除法（退化时调用方应把 y_ratio 定为 0.5）
    bool has_span() const { return (hi - lo) > 1e-9; }
};

/**
 * @brief 绘图区矩形（纯数值版 QRect）
 *
 * 刻意不直接用 QRect：本文件要能在无 Qt 的机器上编译。
 * `y` 轴原点在**上方**（与屏幕坐标一致），所以 bottom() 是较大值。
 */
struct PlotRect {
    double left = 0.0;
    double top = 0.0;
    double width = 0.0;
    double height = 0.0;

    double bottom() const { return top + height; }
    double right() const { return left + width; }

    /// 尺寸是否够画（比 margin 还小就没法画）
    bool usable() const { return width > 1.0 && height > 1.0; }
};

/**
 * @brief 计算某指标在给定数据下的 Y 轴范围
 *
 * ## 两种模式，不能混用
 *
 * **自适应**（只用于 Score）：分数的量纲本身就是归一化的 0-100，看的是
 * 相对起伏，按数据 min/max 铺满绘图区是有意义的。
 *
 * **固定基准**（Rtt / Jitter / Rssi / TcpLoss）：这些指标有绝对含义。
 * 若也按数据自适应，RSSI 从 -45 变到 -50（工程上都属「信号很好」）
 * 会被放成满屏断崖，让现场人员误判成严重劣化。
 *
 * ## 数据超出固定范围时怎么办
 *
 * **向外扩展，不裁剪**。沿用固定基准会让「RTT 突然飙到 800ms」这种
 * 最该被看见的情况被切掉一半。所以固定范围只是**下限基准**：
 * 数据跑出去了，轴就跟着长，但不会为了迁就小幅波动而缩短。
 *
 * @param values 有效值序列（**调用方必须已剔除未测量哨兵**）
 * @param metric 指标类型，决定是否使用固定基准
 */
AxisRange compute_axis_range(const std::vector<double>& values,
                             HistoryMetric metric);

/**
 * @brief 把第 index 个点映射为屏幕坐标
 *
 * @param index 点下标（0 <= index < count）
 * @param count 总点数。count == 1 时 x 取绘图区水平中心（避免除零）
 * @param value 该点的指标值
 * @param range Y 轴范围
 * @param plot  绘图区
 */
ChartPoint map_point(std::size_t index, std::size_t count, double value,
                     const AxisRange& range, const PlotRect& plot);

/**
 * @brief Y 轴刻度标签应保留的小数位
 *
 * 分数与 RSSI 用整数（0 位）够读；抖动与丢包率常在小数位上有意义
 * （0.5ms / 0.75%），取 0 位会把它们全读成 0。
 */
int axis_label_decimals(HistoryMetric metric);

} // namespace dashboard
} // namespace weaknet

#endif // WEAKNET_QT_DASHBOARD_CHART_GEOMETRY_HPP

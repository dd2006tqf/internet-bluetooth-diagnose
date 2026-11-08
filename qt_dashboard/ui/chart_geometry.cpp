/**
 * @file chart_geometry.cpp
 * @brief 折线图坐标几何实现
 */

#include "chart_geometry.hpp"

#include <algorithm>

namespace weaknet {
namespace dashboard {

AxisRange compute_axis_range(const std::vector<double>& values,
                             HistoryMetric metric) {
    const HistoryRange preset = history_metric_range(metric);

    if (values.empty()) {
        // 没有数据时给一个中性范围，避免下游除零。
        // 有固定基准的指标直接用基准，图表的轴看起来才是稳定的；
        // 否则空图与有数据图的轴会来回跳。
        AxisRange empty;
        if (preset.fixed) {
            empty.lo = preset.lo;
            empty.hi = preset.hi;
        } else {
            empty.lo = 0.0;
            empty.hi = 100.0;
        }
        return empty;
    }

    const double data_lo = *std::min_element(values.begin(), values.end());
    const double data_hi = *std::max_element(values.begin(), values.end());

    AxisRange range;

    if (preset.fixed) {
        // 固定基准是**下限**：数据在范围内就钉死基准（波动看起来是真实幅度），
        // 超出范围就向外长（不裁剪掉最该被看见的极值）。
        range.lo = std::min(preset.lo, data_lo);
        range.hi = std::max(preset.hi, data_hi);

        // 外侧再留一点边距，避免极值点正好贴在边框上、被裁掉半个圆点
        const double span = range.hi - range.lo;
        if (span > 1e-6) {
            const double pad = span * 0.02;
            range.lo -= pad;
            range.hi += pad;
        }
        return range;
    }

    // 自适应模式：上下各留 10% 边距；全等值时给最小跨度，否则退化成一条
    // 贴在顶部或底部的线（y_ratio 恒为 0 或 1）。
    //
    // 这里的 ±5 与改进前的 LineChartWidget 行为一致，不要改：
    // 真机实测的 60 个点分数全是 55.0，改了这个值那个场景就贴边了。
    const double span = data_hi - data_lo;
    if (span < 1e-6) {
        range.lo = data_lo - 5.0;
        range.hi = data_hi + 5.0;
    } else {
        const double pad = span * 0.1;
        range.lo = data_lo - pad;
        range.hi = data_hi + pad;
    }
    return range;
}

ChartPoint map_point(std::size_t index, std::size_t count, double value,
                     const AxisRange& range, const PlotRect& plot) {
    ChartPoint out;

    // --- X ---
    // 单点时放正中：i/(n-1) 在 n==1 时分母为零。
    // 这不是"理论上可能"，是真会发生的：服务端刚建库只有一个采样点时必然出现。
    if (count <= 1) {
        out.x = plot.left + plot.width * 0.5;
    } else {
        const double ratio =
            static_cast<double>(index) / static_cast<double>(count - 1);
        out.x = plot.left + ratio * plot.width;
    }

    // --- Y ---
    // 屏幕坐标 y 向下增长，所以用 bottom() 减。
    // 轴退化成单值时固定画在中线，不去赌除零后的结果。
    if (!range.has_span()) {
        out.y = plot.top + plot.height * 0.5;
        return out;
    }
    const double y_ratio = (value - range.lo) / (range.hi - range.lo);
    out.y = plot.bottom() - y_ratio * plot.height;
    return out;
}

int axis_label_decimals(HistoryMetric metric) {
    switch (metric) {
        case HistoryMetric::Score:
            // 0-100 的整数刻度，小数位纯噪声
            return 0;
        case HistoryMetric::Rtt:
            // RTT 常以整数毫秒采样，但轴范围可能被极值拉到很大，
            // 0 位足够，且不会让标签变宽挤掉绘图区
            return 0;
        case HistoryMetric::Rssi:
            // dBm 本身就是整数语义
            return 0;
        case HistoryMetric::Jitter:
            // 抖动常在 0.5 ~ 5ms 量级，0 位会把 0.5 读成 0 或 1
            return 1;
        case HistoryMetric::TcpLoss:
            // 丢包率在健康态是 0.xx%，0 位会让整条轴都是 0
            return 1;
    }
    return 0;
}

} // namespace dashboard
} // namespace weaknet

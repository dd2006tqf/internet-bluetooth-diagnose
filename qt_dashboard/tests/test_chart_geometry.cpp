/**
 * @file test_chart_geometry.cpp
 * @brief 折线图坐标几何的单元测试（不需要 Qt、不需要显示器）
 *
 * ## 为什么这个测试值得单独存在
 *
 * 折线图有两类错误，混在一起就无法定位：
 *
 *   - **算错了**：RTT=150 本该画在轴中间，结果画到了顶部
 *   - **画错了**：坐标是对的，但线没画出来 / 颜色不对
 *
 * `tests/test_line_chart_widget.cpp` 的像素测试只能回答「图上有没有绿点」，
 * 它分不清上面两种。本文件把「算」这一半单独钉死，让像素测试只需负责「画」。
 *
 * 另外，本文件**不需要 Qt**，因此在只有 `qtbase5-dev-tools`（moc）
 * 而没有 `qtbase5-dev` 的机器上也能编译并运行——本机就是这种情况。
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "data/dashboard_data.hpp"
#include "ui/chart_geometry.hpp"

namespace {

using weaknet::dashboard::AxisRange;
using weaknet::dashboard::ChartPoint;
using weaknet::dashboard::compute_axis_range;
using weaknet::dashboard::HistoryMetric;
using weaknet::dashboard::map_point;
using weaknet::dashboard::PlotRect;

/// 一个典型的绘图区：400x200 的画布扣掉边距后的样子
PlotRect sample_plot() {
    PlotRect p;
    p.left = 44.0;
    p.top = 28.0;
    p.width = 344.0;
    p.height = 148.0;
    return p;
}

// ---------------------------------------------------------------------------
// 坐标映射 —— 最基础也最容易错的四条
// ---------------------------------------------------------------------------

TEST(ChartGeometryTest, FirstAndLastPointHitPlotEdges) {
    const PlotRect plot = sample_plot();
    const AxisRange range{0.0, 100.0};

    // 第一个点在左边界，最后一个在右边界（3 个点：0 / 1 / 2）
    const ChartPoint first = map_point(0, 3, 50.0, range, plot);
    const ChartPoint last = map_point(2, 3, 50.0, range, plot);

    EXPECT_DOUBLE_EQ(first.x, plot.left);
    EXPECT_DOUBLE_EQ(last.x, plot.right());
}

TEST(ChartGeometryTest, ValueMapsToVerticalPosition) {
    const PlotRect plot = sample_plot();
    const AxisRange range{0.0, 100.0};

    // 轴底 → 绘图区底；轴顶 → 绘图区顶（屏幕 y 向下增长）
    const ChartPoint at_lo = map_point(0, 1, 0.0, range, plot);
    const ChartPoint at_hi = map_point(0, 1, 100.0, range, plot);
    const ChartPoint at_mid = map_point(0, 1, 50.0, range, plot);

    EXPECT_DOUBLE_EQ(at_lo.y, plot.bottom());
    EXPECT_DOUBLE_EQ(at_hi.y, plot.top);
    EXPECT_DOUBLE_EQ(at_mid.y, plot.top + plot.height * 0.5);
}

TEST(ChartGeometryTest, SinglePointIsCenteredHorizontally) {
    // n == 1 时 i/(n-1) 分母为零。这不是理论问题：
    // 服务端刚建库、只有一个采样点时必然出现。
    const PlotRect plot = sample_plot();
    const ChartPoint p = map_point(0, 1, 50.0, AxisRange{0.0, 100.0}, plot);

    EXPECT_DOUBLE_EQ(p.x, plot.left + plot.width * 0.5);
}

TEST(ChartGeometryTest, DegenerateRangeDoesNotDivideByZero) {
    // hi == lo（全等值序列）时 y_ratio 会除零。必须落回中线，
    // 而不是产生 inf / nan 把整条路径画飞到屏幕外。
    const PlotRect plot = sample_plot();
    const ChartPoint p = map_point(0, 1, 55.0, AxisRange{55.0, 55.0}, plot);

    EXPECT_DOUBLE_EQ(p.y, plot.top + plot.height * 0.5);
    EXPECT_TRUE(std::isfinite(p.y));
}

// ---------------------------------------------------------------------------
// 轴范围 —— 固定基准 vs 自适应
// ---------------------------------------------------------------------------

TEST(ChartGeometryTest, ScoreAxisIsAdaptiveAroundData) {
    // 分数是归一化量纲，按 min/max 铺满才有意义
    const std::vector<double> values{20.0, 40.0, 60.0};
    const AxisRange range = compute_axis_range(values, HistoryMetric::Score);

    // 上下各留 10% 边距：(60-20)*0.1 = 4
    EXPECT_DOUBLE_EQ(range.lo, 16.0);
    EXPECT_DOUBLE_EQ(range.hi, 64.0);
}

TEST(ChartGeometryTest, FlatScoreSeriesGetsMinimumSpan) {
    // 真机实测 60 个点分数全是 55.0（kBoardRealHistory）。
    // 若没有最小跨度兜底，整条线会贴在绘图区边沿。
    const std::vector<double> values(5, 55.0);
    const AxisRange range = compute_axis_range(values, HistoryMetric::Score);

    EXPECT_DOUBLE_EQ(range.lo, 50.0);
    EXPECT_DOUBLE_EQ(range.hi, 60.0);
    EXPECT_TRUE(range.has_span());
}

TEST(ChartGeometryTest, RssiAxisIsFixedNotAdaptive) {
    // RSSI 从 -45 变到 -50，工程上都属「信号很好」。
    // 若自适应，这点差别会被放大成满屏断崖，让现场误判成严重劣化。
    const std::vector<double> values{-45.0, -46.0, -47.0, -50.0};
    const AxisRange range = compute_axis_range(values, HistoryMetric::Rssi);

    // 轴必须仍是 -100 ~ -30 这个物理量级，而不是 -50 到 -45
    EXPECT_LT(range.lo, -90.0);
    EXPECT_GT(range.hi, -40.0);

    // 数据只占了轴的一小段：这才是「波动看起来是它本来的大小」
    const double data_span_ratio =
        (50.0 - 45.0) / (range.hi - range.lo);
    EXPECT_LT(data_span_ratio, 0.3);
}

TEST(ChartGeometryTest, FixedAxisGrowsWhenDataExceedsRange) {
    // 固定基准是**下限**不是硬裁剪：RTT 飙到 800ms 是最该被看见的情况，
    // 若按 300 截断，图上会显示成「贴着顶」，看不出到底飙到多少。
    const std::vector<double> values{25.0, 800.0};
    const AxisRange range = compute_axis_range(values, HistoryMetric::Rtt);

    EXPECT_GT(range.hi, 800.0) << "极值不得被裁掉";
}

TEST(ChartGeometryTest, EmptyValuesFallBackToSaneRange) {
    const AxisRange score = compute_axis_range({}, HistoryMetric::Score);
    EXPECT_LT(score.lo, score.hi);

    const AxisRange rssi = compute_axis_range({}, HistoryMetric::Rssi);
    // 空数据时也要用固定基准，否则轴会在「有数据 / 无数据」之间来回跳
    EXPECT_LT(rssi.lo, -90.0);
    EXPECT_GT(rssi.hi, -40.0);
}

// ---------------------------------------------------------------------------
// 刻度精度 —— 决定 Y 轴标签读不读得懂
// ---------------------------------------------------------------------------

TEST(ChartGeometryTest, LossAxisKeepsOneDecimal) {
    // 健康态的丢包率是 0.xx%，取 0 位会让整条轴每个标签都是 "0"
    EXPECT_EQ(weaknet::dashboard::axis_label_decimals(HistoryMetric::TcpLoss), 1);
    EXPECT_EQ(weaknet::dashboard::axis_label_decimals(HistoryMetric::Jitter), 1);
}

TEST(ChartGeometryTest, ScoreAndRssiUseIntegerLabels) {
    EXPECT_EQ(weaknet::dashboard::axis_label_decimals(HistoryMetric::Score), 0);
    EXPECT_EQ(weaknet::dashboard::axis_label_decimals(HistoryMetric::Rssi), 0);
}

// ---------------------------------------------------------------------------
// 交叉验证：几何层与数据层的哨兵语义必须一致
// ---------------------------------------------------------------------------

TEST(ChartGeometryTest, RssiFixedRangeCoversRealBoardValue) {
    // 真机样本 RSSI 是 -47 dBm（kBoardRealHistory）。
    // 若固定范围装不下它，图上就会一片空白。
    const AxisRange range = compute_axis_range({-47.0}, HistoryMetric::Rssi);
    EXPECT_LE(range.lo, -47.0);
    EXPECT_GE(range.hi, -47.0);

    const PlotRect plot = sample_plot();
    const ChartPoint p = map_point(0, 1, -47.0, range, plot);
    // 必须落在绘图区内部，不能贴边被裁
    EXPECT_GT(p.y, plot.top);
    EXPECT_LT(p.y, plot.bottom());
}

} // namespace

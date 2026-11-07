/**
 * @file theme.hpp
 * @brief 看板视觉主题（配色、字号、QSS）
 *
 * ## 为什么需要单独的 theme
 *
 * 改进前的界面没有全局样式表，8 处 `setStyleSheet` 全是零散写颜色。
 * 这带来三个具体后果：
 *
 * 1. **配色散落**：状态色 `#2E7D32` / `#F9A825` / `#C62828` 在
 *    `main_window.cpp` 里出现 3 次（`state_color`、`summary_label_` 的 html、
 *    折线图默认色），改一处漏两处；
 * 2. **无主题层**：想换深色/浅色必须逐处改代码；
 * 3. **布局靠默认值**：`QGroupBox` 用系统默认边框，`QPushButton` 是系统
 *    原生样式，在 eglfs 直出时显得像未完成的调试界面。
 *
 * ## 与部件级 setStyleSheet 的关系（已实测）
 *
 * 本主题用 `qApp->setStyleSheet()` 设置全局规则，部件级
 * `setStyleSheet("color: ...")` 仍然生效且**不会重置**全局字号——
 * 实测两者笔画高度都是 22px（`/tmp/qsstest/t4.cpp`）。
 *
 * 因此 `MainWindow::update_status_label()` 里那套运行时按状态改色的逻辑
 * 可以原样保留，本文件只负责提供全局外观基线。
 *
 * ## 配色依据
 *
 * 状态色取自 `server/include/assurance/health_state.hpp:15-23` 的四个枚举，
 * 色值选的是 Material Design 的 700/800 色阶：在浅色背景上对比度足够
 * （GOOD `#2E7D32` 对白底的对比度约 5.1:1，满足 WCAG AA 的 4.5:1）。
 */

#ifndef WEAKNET_QT_DASHBOARD_THEME_HPP
#define WEAKNET_QT_DASHBOARD_THEME_HPP

#include <QString>

namespace weaknet {
namespace dashboard {

/**
 * @brief 语义色板
 *
 * 集中管理，避免同一个状态色在多个文件里各写一遍。
 * `LineChartWidget` 与 `MainWindow` 都从这里取。
 */
struct ThemeColors {
    /// GOOD —— Material Green 800
    static constexpr const char* kGood = "#2E7D32";
    /// DEGRADED —— 深琥珀（非 Material Amber 800）
    ///
    /// 原值 `#F9A825` 对白底只有 **1.97:1**，连 WCAG 1.4.11 的非文本 3:1 都不到。
    /// 这个颜色要用来画状态词和圆点，属于「必须能读出来」的内容，
    /// 不能只当装饰。选用 `#B26A00`（4.24:1），仍是明确的琥珀语义，
    /// 同时与 GOOD/BAD 的色阶重量接近，不会显得「黄色比绿红淡一档」。
    static constexpr const char* kDegraded = "#B26A00";
    /// BAD —— Material Red 800
    static constexpr const char* kBad = "#C62828";
    /// UNKNOWN / 不适用 —— Material Grey 500
    ///
    /// 对白底 2.68:1。**刻意保留**：这个色只用于「不适用 / 未知」这类
    /// 无信息量的占位，视觉上就该退到后面；把它提亮反而会让
    /// 「没有数据」看起来像「有一个状态」。
    static constexpr const char* kUnknown = "#9E9E9E";

    /// 主文字色
    static constexpr const char* kTextPrimary = "#212121";
    /// 次要文字色（提示、元信息）
    static constexpr const char* kTextSecondary = "#616161";
    /// 更弱的文字色（轴标签、占位）
    static constexpr const char* kTextMuted = "#9E9E9E";

    /// 页面底色
    static constexpr const char* kBackground = "#F5F6F8";
    /// 卡片 / 分组底色
    static constexpr const char* kSurface = "#FFFFFF";
    /// 分隔线
    static constexpr const char* kDivider = "#E0E3E8";
    /// 网格线
    static constexpr const char* kGrid = "#E8EBF0";
};

/**
 * @brief 应用全局样式表
 *
 * 必须在 `QApplication` 构造之后、`MainWindow` 构造之前调用。
 *
 * 用 `QStyleFactory::create("Fusion")` 做基底：eglfs/offscreen 下原生样式
 * 会退化得很难看，Fusion 是跨平台一致的，也是 Qt 官方推荐的 QSS 基底。
 *
 * @return 实际生效的样式名，便于日志确认
 */
QString apply_theme();

} // namespace dashboard
} // namespace weaknet

#endif // WEAKNET_QT_DASHBOARD_THEME_HPP

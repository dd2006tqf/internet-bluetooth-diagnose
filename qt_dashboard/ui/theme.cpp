/**
 * @file theme.cpp
 * @brief 看板视觉主题实现
 */

#include "theme.hpp"

#include <QApplication>
#include <QFont>
#include <QPalette>
#include <QStyleFactory>

namespace weaknet {
namespace dashboard {

namespace {

/**
 * @brief 构建全局 QSS
 *
 * 结构按「容器 → 文本 → 控件」分层，与 Qt 官方的层叠顺序一致：
 * 同等特异性下后写的规则覆盖先写的，因此交互态（`:hover` / `:pressed`）
 * 必须写在基础规则之后。
 *
 * 字体统一用 sans-serif + 明确字号：eglfs 直出时没有桌面环境的字体配置，
 * 不指定就会落到难以预测的默认值上。
 */
QString build_stylesheet() {
    return QStringLiteral(R"(
/* ---------- 全局基底 ---------- */
QWidget {
    font-family: "Noto Sans CJK SC", "Source Han Sans SC", "WenQuanYi Micro Hei", sans-serif;
    font-size: 10pt;
    color: %1;
}

QMainWindow, QDialog {
    background-color: %2;
}

/* ---------- 分组卡片 ---------- */
QGroupBox {
    background-color: %3;
    border: 1px solid %4;
    border-radius: 6px;
    margin-top: 14px;
    padding: 6px 10px 4px 10px;
    font-weight: bold;
    color: %5;
}

/* 标题压在边框上，背景要与卡片同色才能「切开」边框 */
QGroupBox::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    left: 12px;
    padding: 0 6px;
    background-color: %3;
    color: %5;
}

/* ---------- 文本 ---------- */
QLabel {
    background: transparent;
}

/* 状态文本用等宽字体：GOOD / DEGRADED / BAD 长度不同，
   等宽能让左对齐的列看起来整齐 */
QLabel#state_text {
    font-family: "DejaVu Sans Mono", "Noto Sans Mono", monospace;
    font-size: 9pt;
}

/* 整体分数：最重要的一个数字，给足视觉权重。
   20pt 而不是更大：主窗口最小高度是硬约束，7 寸屏常见 1024x600 / 720p，
   字号每涨 2pt 顶栏就多约 3px，26pt 会把最小高度顶到 728px（实测），
   720p 屏幕上窗口就放不下了。20pt 相对正文 10pt 仍是 2 倍，主次足够。 */
QLabel#score_value {
    font-size: 20pt;
    font-weight: bold;
}

/* 网卡名：等宽 + 加粗，避免 wlan0 / wlan1 混淆 */
QLabel#iface_name {
    font-family: "DejaVu Sans Mono", "Noto Sans Mono", monospace;
    font-size: 12pt;
    font-weight: bold;
}

/* 元信息（档位、时间戳）：弱化但不至于看不见 */
QLabel#meta_info {
    color: %6;
    font-size: 9pt;
}

/* 底栏的经验 / 诊断文本 */
QLabel#detail_text {
    color: %6;
    font-size: 9pt;
}

/* 底栏结论行：比明细更重要，用主文字色而非弱化色 */
QLabel#primary_text {
    color: %5;
    font-size: 10pt;
}

/* 服务端主动标记的警告：红色是语义不是装饰，与 BAD 状态同色系 */
QLabel#warn_text {
    color: %8;
    font-size: 10pt;
}

/* ---------- 按钮 ---------- */
QPushButton {
    background-color: %3;
    border: 1px solid %4;
    border-radius: 4px;
    padding: 5px 16px;
    color: %5;
}

QPushButton:hover {
    background-color: #EEF1F6;
    border-color: #C5CAD3;
}

QPushButton:pressed {
    background-color: #E2E6ED;
}

QPushButton:disabled {
    color: %7;
    background-color: #F0F1F3;
    border-color: %4;
}

/* ---------- 分隔器 ---------- */
QSplitter::handle {
    background-color: %2;
}

QSplitter::handle:horizontal {
    width: 8px;
}

/* ---------- 状态栏 ---------- */
QStatusBar {
    background-color: %3;
    border-top: 1px solid %4;
    color: %6;
    font-size: 9pt;
}

QStatusBar::item {
    border: none;
}

/* ---------- 提示气泡 ---------- */
QToolTip {
    background-color: #37474F;
    color: #FFFFFF;
    border: 1px solid #37474F;
    padding: 4px 6px;
}
)")
        .arg(QString::fromLatin1(ThemeColors::kTextPrimary))   // %1
        .arg(QString::fromLatin1(ThemeColors::kBackground))    // %2
        .arg(QString::fromLatin1(ThemeColors::kSurface))       // %3
        .arg(QString::fromLatin1(ThemeColors::kDivider))       // %4
        .arg(QString::fromLatin1(ThemeColors::kTextPrimary))   // %5
        .arg(QString::fromLatin1(ThemeColors::kTextSecondary)) // %6
        .arg(QString::fromLatin1(ThemeColors::kUnknown))       // %7
        .arg(QString::fromLatin1(ThemeColors::kBad));          // %8
}

/**
 * @brief 构建调色板
 *
 * QSS 覆盖不到的地方（原生对话框、某些子控件、QToolTip 之外的 popup）
 * 仍读 palette，因此两者要一致，否则会出现浅色界面上蹦出一块深色区域。
 *
 * 这里刻意保持**浅色**主题：看板多在室内/机柜旁使用，浅色在强光下可读性更好；
 * 深色更适合长时间盯屏。若将来要加深色，改这一处 + QSS 即可。
 */
QPalette build_palette() {
    QPalette p;
    p.setColor(QPalette::Window, QColor(ThemeColors::kBackground));
    p.setColor(QPalette::WindowText, QColor(ThemeColors::kTextPrimary));
    p.setColor(QPalette::Base, QColor(ThemeColors::kSurface));
    p.setColor(QPalette::AlternateBase, QColor(ThemeColors::kBackground));
    p.setColor(QPalette::Text, QColor(ThemeColors::kTextPrimary));
    p.setColor(QPalette::Button, QColor(ThemeColors::kSurface));
    p.setColor(QPalette::ButtonText, QColor(ThemeColors::kTextPrimary));
    p.setColor(QPalette::Highlight, QColor(0x1A, 0x73, 0xE8));
    p.setColor(QPalette::HighlightedText, QColor(Qt::white));
    p.setColor(QPalette::ToolTipBase, QColor(0x37, 0x47, 0x4F));
    p.setColor(QPalette::ToolTipText, QColor(Qt::white));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(ThemeColors::kUnknown));
    p.setColor(QPalette::Disabled, QPalette::ButtonText,
               QColor(ThemeColors::kUnknown));
    return p;
}

} // namespace

QString apply_theme() {
    QString style_name = QStringLiteral("Fusion");
    if (QStyle* fusion = QStyleFactory::create(style_name)) {
        QApplication::setStyle(fusion);
    } else {
        // 极端情况下 Fusion 不可用（静态构建裁剪掉），沿用当前样式但不中断，
        // 因为 QSS 仍会生效——只是某些控件细节会跟着平台样式走。
        style_name = QStringLiteral("(platform default)");
    }

    qApp->setPalette(build_palette());
    qApp->setStyleSheet(build_stylesheet());
    return style_name;
}

} // namespace dashboard
} // namespace weaknet

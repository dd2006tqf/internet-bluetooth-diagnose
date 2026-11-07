/**
 * @file test_theme.cpp
 * @brief 主题与状态着色的像素级测试
 *
 * ## 这些测试防的是什么回归
 *
 * 本轮改版把三处散落的颜色字面量收敛到 `ThemeColors`，并新增了
 * 「维度圆点跟随状态着色」这个行为。这两件事都可能被后续改动无声破坏：
 *
 * 1. `apply_theme()` 设置全局 QSS 后，`state_color()` 若退回字面量，
 *    颜色仍然「看起来对」，只有精确比色才能发现它不再来自主题；
 * 2. 圆点着色依赖 `make_status_row()` 把 dot 指针交出去。一旦
 *    `update_status_label()` 的第三个参数被漏传或传 null，圆点会静默
 *    退回默认灰——界面上仍有圆点，但失去语义。
 *
 * ## 为什么能脱离显示器
 *
 * 与 `test_line_chart_widget.cpp` 同一套路：`widget.grab()` 走完整
 * 绘制路径，产物是 `QPixmap`，可以逐像素断言。offscreen 平台即可。
 *
 * ## 判据为什么锚定精确色值
 *
 * 上一轮踩过「非白像素」这种宽松判据的坑（图表灰底被当成内容）。
 * 这里所有断言都要求**精确 RGB**：QSS/palette 设置的颜色不经抗锯齿混合
 * 的像素就是纯前景色，因此「存在该精确色」是有意义的强判据。
 */

#include <gtest/gtest.h>

#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QPixmap>
#include <QRect>
#include <QString>

#include <cmath>

#include "data/dashboard_data.hpp"
#include "ui/main_window.hpp"
#include "ui/theme.hpp"

namespace {

using weaknet::dashboard::MainWindow;
using weaknet::dashboard::ThemeColors;

/// `QWidget` 要求进程内有 `QApplication`（`QGuiApplication` 不够）
class QtAppEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        static int argc = 1;
        static char arg0[] = "test_theme";
        static char* argv[] = {arg0, nullptr};

        if (!QApplication::instance()) {
            app_ = new QApplication(argc, argv);
        }
    }

    void TearDown() override {
        delete app_;
        app_ = nullptr;
    }

private:
    QApplication* app_ = nullptr;
};

[[maybe_unused]] const ::testing::Environment* const g_qt_env =
    ::testing::AddGlobalTestEnvironment(new QtAppEnvironment);

/**
 * @brief 与服务端 `legacy_adapter.hpp` 输出形状一致的样本
 *
 * 刻意让 4 个维度各处于一种状态（GOOD / DEGRADED / BAD / NOT_APPLICABLE），
 * 这样一次渲染就能覆盖 `update_status_label()` 的全部分支。
 */
const char* kMixedSnapshot = R"({
  "schema_version":2,
  "interface":"wlan0",
  "assessment_profile":"FULL",
  "overall":{"state":"DEGRADED","coverage":"FULL_FOR_PROFILE","display_score":72.5},
  "network_health":{
    "ip_reachability":{"state":"GOOD","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"","evidence":[]},
    "responsiveness":{"state":"DEGRADED","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"rtt_high","evidence":[]},
    "reliability":{"state":"BAD","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"loss_high","evidence":[]},
    "rf_health":{"state":"UNKNOWN","coverage":"NONE","applicability":"NOT_APPLICABLE","reason":"wired_link","evidence":[]}
  },
  "service_health":{
    "dns":{"state":"GOOD","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"","evidence":[]},
    "tcp_connect":{"state":"GOOD","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"","evidence":[]},
    "tcp_rtt":{"state":"GOOD","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"","evidence":[]},
    "http_access":{"state":"GOOD","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"","evidence":[]},
    "captive_portal":{"state":"UNKNOWN","coverage":"NONE","applicability":"NOT_APPLICABLE","reason":"","evidence":[]},
    "active_dns":{"state":"GOOD","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"","evidence":[]},
    "active_tcp":{"state":"GOOD","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"","evidence":[]},
    "active_https":{"state":"GOOD","coverage":"FULL_FOR_PROFILE","applicability":"APPLICABLE","reason":"","evidence":[]},
    "active_portal":{"state":"UNKNOWN","coverage":"NONE","applicability":"NOT_APPLICABLE","reason":"","evidence":[]}
  },
  "warnings":[],
  "primary_issue":"rtt_high"
})";

QColor theme_qcolor(const char* hex) {
    return QColor(QString::fromLatin1(hex));
}

/// 统计整张图中出现该精确色的像素数
int count_exact(const QImage& img, const QColor& target) {
    const QRgb want = target.rgb();
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            if (img.pixel(x, y) == want) {
                ++n;
            }
        }
    }
    return n;
}

/// 只在指定矩形内统计精确色（用于把断言锚定到窗口的某一块）
int count_exact_in(const QImage& img, const QRect& box, const QColor& target) {
    const QRgb want = target.rgb();
    int n = 0;
    const QRect r = box.intersected(img.rect());
    for (int y = r.top(); y <= r.bottom(); ++y) {
        for (int x = r.left(); x <= r.right(); ++x) {
            if (img.pixel(x, y) == want) {
                ++n;
            }
        }
    }
    return n;
}

/// 渲染整个主窗口
QImage render_window(MainWindow& w) {
    w.setInterface(QString());
    w.resize(1024, 700);
    w.show();
    QApplication::processEvents();
    const QPixmap pm = w.grab();
    return pm.toImage().convertToFormat(QImage::Format_RGB32);
}

/// 通过真实信号槽喂入快照（走 QMetaObject，不新增测试专用接口）
void feed_experience(MainWindow& w, const char* json) {
    const bool ok = QMetaObject::invokeMethod(
        &w, "onExperienceReady", Qt::DirectConnection,
        Q_ARG(QByteArray, QByteArray(json)));
    ASSERT_TRUE(ok) << "onExperienceReady 必须是可被元对象调用的槽";
}

// ---------------------------------------------------------------------------
// 主题本身
// ---------------------------------------------------------------------------

TEST(ThemeTest, ApplyThemeReturnsFusionStyle) {
    // Fusion 是跨平台一致的基底样式。eglfs/offscreen 下原生样式会退化，
    // 返回 "(platform default)" 就意味着主题降级了，需要知道。
    const QString style = weaknet::dashboard::apply_theme();
    EXPECT_EQ(style, QStringLiteral("Fusion"))
        << "Fusion 不可用会退化到平台默认样式，QSS 细节将不受控";
}

TEST(ThemeTest, ThemeColorsAreDistinct) {
    // 四个状态色必须互不相同，否则界面上分不出状态。
    // 这不是形式主义：近似灰会让 DEGRADED 与 UNKNOWN 视觉上无法区分。
    const QColor good = theme_qcolor(ThemeColors::kGood);
    const QColor degraded = theme_qcolor(ThemeColors::kDegraded);
    const QColor bad = theme_qcolor(ThemeColors::kBad);
    const QColor unknown = theme_qcolor(ThemeColors::kUnknown);

    EXPECT_TRUE(good.isValid());
    EXPECT_TRUE(degraded.isValid());
    EXPECT_TRUE(bad.isValid());
    EXPECT_TRUE(unknown.isValid());

    EXPECT_NE(good, degraded);
    EXPECT_NE(good, bad);
    EXPECT_NE(good, unknown);
    EXPECT_NE(degraded, bad);
    EXPECT_NE(degraded, unknown);
    EXPECT_NE(bad, unknown);
}

TEST(ThemeTest, StatusColorsHaveEnoughContrastOnWhite) {
    // 状态色要能压在白卡片上读出来。WCAG AA 正文要求 4.5:1，
    // 这里按相对亮度公式独立复算，不依赖外部工具。
    auto relative_luminance = [](const QColor& c) {
        auto channel = [](double v) {
            const double s = v / 255.0;
            return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * channel(c.red()) + 0.7152 * channel(c.green()) +
               0.0722 * channel(c.blue());
    };

    const double white = 1.0;
    const QColor colors[] = {
        theme_qcolor(ThemeColors::kGood),
        theme_qcolor(ThemeColors::kDegraded),
        theme_qcolor(ThemeColors::kBad),
    };

    for (const QColor& c : colors) {
        const double l = relative_luminance(c);
        const double ratio = (white + 0.05) / (l + 0.05);
        // 3.0:1 是 WCAG 1.4.11 对「状态词、图标这类非文本内容」的门槛。
        // 这条判据是有效的：改版初版用的 Amber 800 (#F9A825) 实测只有
        // 1.97:1，被它当场拦下，随后换成 #B26A00 (4.24:1)。
        EXPECT_GT(ratio, 3.0)
            << c.name().toStdString() << " 对白底对比度 " << ratio << ":1 过低";
    }
}

TEST(ThemeTest, UnknownGreyStaysDeliberatelyDim) {
    // kUnknown 刻意不满足 3:1（实测 2.68:1），这是设计选择而非疏漏：
    // 它只用于「不适用 / 未知」这类无信息量的占位，视觉上就该退到后面。
    //
    // 但「刻意」不等于「随便」——它仍要有最低可辨识度（2.0:1），
    // 否则用户会以为那里是一块空白，而不是一个写着「不适用」的状态。
    // 这条测试把豁免的范围钉死：低于 2.0 就说明改错了。
    auto relative_luminance = [](const QColor& c) {
        auto channel = [](double v) {
            const double s = v / 255.0;
            return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * channel(c.red()) + 0.7152 * channel(c.green()) +
               0.0722 * channel(c.blue());
    };

    const QColor grey = theme_qcolor(ThemeColors::kUnknown);
    const double ratio = 1.05 / (relative_luminance(grey) + 0.05);

    EXPECT_GT(ratio, 2.0) << "占位灰太淡会读不出来";
    EXPECT_LT(ratio, 3.0)
        << "占位灰不该达到状态色门槛：那会让「没有数据」看起来像「有状态」";
}

// ---------------------------------------------------------------------------
// 全局 QSS 是否真的落到控件上
// ---------------------------------------------------------------------------

TEST(ThemeTest, WindowBackgroundUsesThemeBackground) {
    weaknet::dashboard::apply_theme();
    MainWindow w;
    const QImage img = render_window(w);

    const int bg = count_exact(img, theme_qcolor(ThemeColors::kBackground));
    const int total = img.width() * img.height();
    EXPECT_GT(bg, total / 4)
        << "页面底色应占画面主要面积；过少说明 QMainWindow 背景规则没生效";
}

TEST(ThemeTest, CardSurfaceIsWhite) {
    weaknet::dashboard::apply_theme();
    MainWindow w;
    const QImage img = render_window(w);

    const int white = count_exact(img, theme_qcolor(ThemeColors::kSurface));
    EXPECT_GT(white, 20000)
        << "两张卡片应有可观的白底面积；接近 0 说明 QGroupBox 规则没生效";
}

TEST(ThemeTest, DividerColorDrawnAsCardBorder) {
    // 卡片必须有 1px 边框，否则浅底上的白卡片几乎看不出边界。
    weaknet::dashboard::apply_theme();
    MainWindow w;
    const QImage img = render_window(w);

    const int divider = count_exact(img, theme_qcolor(ThemeColors::kDivider));
    EXPECT_GT(divider, 1000)
        << "分隔线色应出现在卡片边框上；过少说明边框没画出来";
}

// ---------------------------------------------------------------------------
// 状态着色（本轮新增行为：圆点跟随状态）
// ---------------------------------------------------------------------------

TEST(ThemeTest, StatusColorsAppearAfterFeedingSnapshot) {
    weaknet::dashboard::apply_theme();
    MainWindow w;
    render_window(w);

    // 渲染前不该有任何状态色（全是「--」占位）
    feed_experience(w, kMixedSnapshot);
    QApplication::processEvents();
    const QPixmap pm = w.grab();
    const QImage img = pm.toImage().convertToFormat(QImage::Format_RGB32);

    const int good = count_exact(img, theme_qcolor(ThemeColors::kGood));
    const int degraded = count_exact(img, theme_qcolor(ThemeColors::kDegraded));
    const int bad = count_exact(img, theme_qcolor(ThemeColors::kBad));
    const int unknown = count_exact(img, theme_qcolor(ThemeColors::kUnknown));

    EXPECT_GT(good, 0) << "样本里有 8 个 GOOD 维度，绿色必须出现";
    EXPECT_GT(degraded, 0) << "样本里有 1 个 DEGRADED 维度，琥珀色必须出现";
    EXPECT_GT(bad, 0) << "样本里有 1 个 BAD 维度，红色必须出现";
    EXPECT_GT(unknown, 0) << "样本里有 4 个 NOT_APPLICABLE 维度，灰色必须出现";
}

TEST(ThemeTest, DotsFollowTheirStateColor) {
    // 这条是本轮新增行为的关键断言：圆点必须与状态文字同色。
    //
    // 失效模式：`update_status_label()` 的 dot 参数漏传 → 圆点停在
    // QSS 的默认前景色（#212121）。此时状态文字仍然正确着色，
    // 单看「绿色存在」的断言照样通过，这个 bug 会完全无声。
    //
    // 因此锚定到「左侧维度列」的横向范围，统计其中的绿色像素：
    // 若圆点不着色，那里就只剩状态文字的绿，数量会显著掉一档。
    weaknet::dashboard::apply_theme();
    MainWindow w;
    render_window(w);
    feed_experience(w, kMixedSnapshot);
    QApplication::processEvents();
    const QPixmap pm = w.grab();
    const QImage img = pm.toImage().convertToFormat(QImage::Format_RGB32);

    // 左侧维度列在窗口左三分之一内（卡片 + 圆点 + 状态文字都在这里）
    const QRect left_column(0, 0, img.width() / 3, img.height());
    const int left_good =
        count_exact_in(img, left_column, theme_qcolor(ThemeColors::kGood));

    // 8 个 GOOD 维度 × （圆点 + 状态词）都要着色。
    // 阈值取 100：QSS 下 14px 的实心 ● 字形本身就有数十个纯色像素，
    // 加上 GOOD 四个字母的实心笔画，正常渲染远超这个数。
    EXPECT_GT(left_good, 100)
        << "左侧列的绿色像素过少：圆点可能没跟随状态着色（漏传 dot 参数）";
}

} // namespace

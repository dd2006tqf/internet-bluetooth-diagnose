/**
 * @file test_video_frame_widget.cpp
 * @brief 摄像头画面框的渲染语义测试
 *
 * ## 这个套件要钉住什么
 *
 * 不是「颜色对不对」——那是 `test_nv12_to_rgb` 的事。这里钉的是**画面框的
 * 显示语义**，四类问题：
 *
 *   1. 有帧时画出来的像素确实来自那帧（不是空白、不是旧帧）
 *   2. 没有帧时**不能保留上一帧**（否则看起来像实时画面，实际是静止的）
 *   3. 格式不认识时明确说明，而不是把未知格式当 NV12 画成噪点
 *   4. 源数据不完整时宁可显示"数据不完整"，也不画半帧
 *
 * 第 2 条是最容易在实现里写错的：一个想当然的实现会"最后一帧一直留着"，
 * 而现场看到静止画面的第一反应是"摄像头卡了"，不是"取不到帧了"。
 */

#include <gtest/gtest.h>

#include <QApplication>
#include <QImage>
#include <QPainter>

#include "ui/video_frame_widget.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

using weaknet::dashboard::VideoFrame;
using weaknet::dashboard::VideoFrameWidget;

/// 让 QApplication 在整轮测试里只初始化一次（offscreen 平台，无需显示器）
QApplication* app() {
    static int argc = 1;
    static char name[] = "test_video_frame_widget";
    static char* argv[] = {name, nullptr};
    static QApplication* instance = nullptr;
    if (!instance) {
        instance = new QApplication(argc, argv);
    }
    return instance;
}

constexpr uint32_t kFourccNv12 = 0x3231564E;

struct Nv12Frame {
    std::vector<uint8_t> bytes;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
};

/// 造一个纯色 NV12 帧（Y 全填 y，UV 全填 128/128 = 中性色度）
Nv12Frame makeSolidNv12(uint32_t w, uint32_t h, uint32_t stride, uint8_t y) {
    Nv12Frame f;
    f.width = w;
    f.height = h;
    f.stride = stride;
    f.bytes.assign(static_cast<size_t>(stride) * h * 3 / 2, 0);
    for (uint32_t row = 0; row < h; ++row) {
        std::fill_n(f.bytes.begin() + static_cast<size_t>(row) * stride, w, y);
    }
    uint8_t* uv = f.bytes.data() + static_cast<size_t>(stride) * h;
    const size_t uv_len = static_cast<size_t>(stride) * (h / 2);
    for (size_t i = 0; i + 1 < uv_len; i += 2) {
        uv[i] = 128;
        uv[i + 1] = 128;
    }
    return f;
}

VideoFrame toVideoFrame(const Nv12Frame& src) {
    VideoFrame vf;
    vf.nv12 = QByteArray(reinterpret_cast<const char*>(src.bytes.data()),
                         static_cast<int>(src.bytes.size()));
    vf.width = src.width;
    vf.height = src.height;
    vf.stride = src.stride;
    vf.fourcc = kFourccNv12;
    vf.sequence = 7;
    vf.timestamp_ns = 12345;
    return vf;
}

/// 把控件渲染进 QImage，便于逐像素断言
QImage renderWidget(VideoFrameWidget& w, int width = 160, int height = 120) {
    w.resize(width, height);
    QImage canvas(width, height, QImage::Format_RGB888);
    canvas.fill(Qt::magenta);   // 显眼底色：没被画到的地方一眼看出来
    QPainter p(&canvas);
    w.render(&p);
    p.end();
    return canvas;
}

// ---------------------------------------------------------------------------

TEST(VideoFrameWidgetTest, StartsWithoutFrameAndShowsPlaceholder) {
    app();
    VideoFrameWidget w;
    EXPECT_FALSE(w.hasFrame()) << "构造后不应有帧";
    EXPECT_EQ(w.sequence(), 0u);
    EXPECT_EQ(w.placeholderText(), QStringLiteral("暂无画面"));

    // 占位态必须真的画了东西：整块画布应当不再是最初的品红底色，
    // 否则说明 paintEvent 什么都没画（留白与"框没接上"无法区分）。
    const QImage img = renderWidget(w);
    EXPECT_NE(img.pixelColor(2, 2), QColor(Qt::magenta))
        << "占位态必须自己涂底色，不能留空";
}

TEST(VideoFrameWidgetTest, SetFrameRendersPixelsFromSource) {
    app();
    VideoFrameWidget w;

    // Y=235 是 limited range 的白点 → 转换后应接近 255
    const auto src = makeSolidNv12(64, 48, 64, 235);
    w.setFrame(toVideoFrame(src));

    ASSERT_TRUE(w.hasFrame());
    EXPECT_EQ(w.sequence(), 7u);

    const QImage img = renderWidget(w, 128, 96);
    const QColor c = img.pixelColor(64, 48);   // 中心，避开 letterbox 黑边
    EXPECT_GT(c.red(), 200) << "Y=235 应接近白色，实际 R=" << c.red();
    EXPECT_GT(c.green(), 200);
    EXPECT_GT(c.blue(), 200);
    EXPECT_LT(std::abs(c.red() - c.blue()), 6) << "中性色度应得灰色，不该偏色";
}

TEST(VideoFrameWidgetTest, LetterboxKeepsAspectRatioAndFillsBarsBlack) {
    app();
    VideoFrameWidget w;

    // 4:3 源（64x48）放进 160x60 的宽扁框：
    //   按高度贴合 → 画面 80x60，**左右**留黑边（不是上下）
    // 这里第一版把黑边位置写成了上下，跑出来 255 vs 40 才发现判据写反了。
    const auto src = makeSolidNv12(64, 48, 64, 235);
    w.setFrame(toVideoFrame(src));

    const QImage img = renderWidget(w, 160, 60);
    // 左边缘中央：应当是黑边
    const QColor bar = img.pixelColor(2, 30);
    EXPECT_LT(bar.red(), 40) << "等比缩放后左右应留黑边，不该拉伸变形";
    EXPECT_LT(bar.green(), 40);
    // 中心：应当是画面（亮色）
    const QColor mid = img.pixelColor(80, 30);
    EXPECT_GT(mid.red(), 200) << "画面区域应为亮色";

    // 反向用例：竖长的框里应当是上下留黑边。
    // 只测一个方向会漏掉「算反了也恰好对」的实现。
    const QImage tall = renderWidget(w, 60, 160);
    const QColor top_bar = tall.pixelColor(30, 2);
    EXPECT_LT(top_bar.red(), 40) << "竖长框里应上下留黑边";
    const QColor tall_mid = tall.pixelColor(30, 80);
    EXPECT_GT(tall_mid.red(), 200);
}

TEST(VideoFrameWidgetTest, ClearFrameDoesNotKeepPreviousImage) {
    app();
    VideoFrameWidget w;

    const auto src = makeSolidNv12(64, 48, 64, 235);
    w.setFrame(toVideoFrame(src));
    ASSERT_TRUE(w.hasFrame());

    w.clearFrame();
    EXPECT_FALSE(w.hasFrame())
        << "清空后不能还留着上一帧——静止画面会被误认为实时画面";

    const QImage img = renderWidget(w, 128, 96);
    const QColor c = img.pixelColor(64, 48);
    EXPECT_LT(c.red(), 100) << "清空后应显示深色占位，而非上一帧的亮画面";
}

TEST(VideoFrameWidgetTest, InvalidFrameClearsInsteadOfKeepingStaleImage) {
    app();
    VideoFrameWidget w;

    const auto src = makeSolidNv12(64, 48, 64, 235);
    w.setFrame(toVideoFrame(src));
    ASSERT_TRUE(w.hasFrame());

    VideoFrame empty;   // 语义等同"没有画面了"
    w.setFrame(empty);
    EXPECT_FALSE(w.hasFrame()) << "空帧必须清掉旧画面，不能静默忽略";
    EXPECT_EQ(w.placeholderText(), QStringLiteral("暂无画面"));
}

TEST(VideoFrameWidgetTest, UnknownFourccShowsExplicitMessage) {
    app();
    VideoFrameWidget w;

    const auto src = makeSolidNv12(64, 48, 64, 235);
    auto vf = toVideoFrame(src);
    vf.fourcc = 0x32315659;   // YV12，不是 NV12

    w.setFrame(vf);
    EXPECT_FALSE(w.hasFrame());
    EXPECT_EQ(w.placeholderText(), QStringLiteral("不支持的像素格式"))
        << "格式不认识时必须明说，不能当 NV12 硬画成噪点";
}

TEST(VideoFrameWidgetTest, TruncatedSourceIsRejectedNotHalfDrawn) {
    app();
    VideoFrameWidget w;

    auto src = makeSolidNv12(64, 48, 64, 235);
    auto vf = toVideoFrame(src);
    // 砍掉一半字节：中枢在"源比槽短"时会截断，面板必须识别出来
    vf.nv12.chop(vf.nv12.size() / 2);

    w.setFrame(vf);
    EXPECT_FALSE(w.hasFrame()) << "源数据不完整时应拒绝绘制，而不是画半帧（下半截会是垃圾）";
    EXPECT_EQ(w.placeholderText(), QStringLiteral("画面数据不完整"));
}

TEST(VideoFrameWidgetTest, StrideGreaterThanWidthIsHonored) {
    app();
    VideoFrameWidget w;

    // **stride > width 本身在板端并不成立**：实测真实 stride = width = 1640。
    // 但「stride != width」这条路径必须支持——其它驱动（以及 vivid）会填充。
    // 用 4x2 的小帧 + stride 8 让偏差放大到可断言的程度。
    const uint32_t w4 = 4, h4 = 2, stride8 = 8;
    std::vector<uint8_t> bytes(static_cast<size_t>(stride8) * h4 * 3 / 2, 0);
    // Y 平面填 235（亮），但每行只填前 4 个字节，后 4 个是 padding
    for (uint32_t row = 0; row < h4; ++row) {
        std::fill_n(bytes.begin() + static_cast<size_t>(row) * stride8, w4, 235);
    }
    // UV 平面（偏移 stride*h，不是 w*h）填中性
    uint8_t* uv = bytes.data() + static_cast<size_t>(stride8) * h4;
    for (size_t i = 0; i + 1 < static_cast<size_t>(stride8) * (h4 / 2); i += 2) {
        uv[i] = 128;
        uv[i + 1] = 128;
    }

    VideoFrame vf;
    vf.nv12 = QByteArray(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<int>(bytes.size()));
    vf.width = w4;
    vf.height = h4;
    vf.stride = stride8;
    vf.fourcc = kFourccNv12;
    vf.sequence = 1;

    w.setFrame(vf);
    ASSERT_TRUE(w.hasFrame()) << "stride > width 是板端常态，必须支持";

    const QImage img = renderWidget(w, 64, 32);
    const QColor c = img.pixelColor(32, 16);
    EXPECT_GT(c.red(), 200) << "UV 平面偏移若用 width*h 而非 stride*h，这里会偏色/变暗";
}

TEST(VideoFrameWidgetTest, UvOffsetWithAlignmentHoleReachesTheConverter) {
    app();
    VideoFrameWidget w;

    // ========================================================================
    // 守的是「uv_offset 从 VideoFrame 一路传到 nv12ToRgb888」
    // ========================================================================
    //
    // 板端 sunxi-vin 实测布局（2026-11-14，1640x1232 NV12）：
    //   Y   [0, 2020480)         stride=1640
    //   空洞 [2020480, 2030336)   9856 字节，全零
    //   UV  [2030336, 3040576)   stride=1640
    //
    // 这个用例把它等比缩小：UV 偏移严格大于 stride*height，中间是零空洞。
    // 若控件层把 uv_offset 丢了（或按 stride*height 重算），转换器会读到
    // 空洞里的 0 → U=V=0 → 严重偏色。用「中性 UV 应得灰」当判据。
    const uint32_t fw = 8, fh = 8, fstride = 8;
    const uint32_t hole = 32;
    const size_t y_bytes = static_cast<size_t>(fstride) * fh;
    const size_t uv_off = y_bytes + hole;
    const size_t uv_bytes = static_cast<size_t>(fstride) * (fh / 2);

    std::vector<uint8_t> bytes(uv_off + uv_bytes, 0);
    for (uint32_t row = 0; row < fh; ++row) {
        // Y=235（亮），让偏色在亮背景上一眼可见
        std::fill_n(bytes.begin() + static_cast<size_t>(row) * fstride, fw, 235);
    }
    for (uint32_t row = 0; row < fh / 2; ++row) {
        for (uint32_t col = 0; col + 1 < fw; col += 2) {
            bytes[uv_off + static_cast<size_t>(row) * fstride + col] = 128;
            bytes[uv_off + static_cast<size_t>(row) * fstride + col + 1] = 128;
        }
    }

    VideoFrame vf;
    vf.nv12 = QByteArray(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<int>(bytes.size()));
    vf.width = fw;
    vf.height = fh;
    vf.stride = fstride;
    vf.uv_offset = static_cast<uint32_t>(uv_off);
    vf.fourcc = kFourccNv12;
    vf.sequence = 9;

    ASSERT_GT(uv_off, y_bytes) << "本用例要求 UV 偏移严格大于 stride*height";
    w.setFrame(vf);
    ASSERT_TRUE(w.hasFrame()) << "带对齐空洞的帧必须能显示，不能因校验过严被拒";

    const QImage img = renderWidget(w, 64, 64);
    const QColor c = img.pixelColor(32, 32);
    EXPECT_GT(c.red(), 200) << "Y=235 应接近白色";
    EXPECT_LT(std::abs(c.red() - c.blue()), 8)
        << "三通道应接近相等；偏色说明 uv_offset 没传到转换器、UV 读到了空洞。"
        << " 实际 R=" << c.red() << " G=" << c.green() << " B=" << c.blue();
}

}  // namespace

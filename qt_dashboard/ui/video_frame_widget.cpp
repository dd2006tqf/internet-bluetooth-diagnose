/**
 * @file video_frame_widget.cpp
 * @brief 摄像头画面框实现
 */

#include "video_frame_widget.hpp"

#include "nv12_to_rgb.hpp"

#include <QPainter>
#include <QPaintEvent>

#include <vector>

namespace weaknet {
namespace dashboard {

namespace {

/// NV12 的 fourcc，小端打包的 "NV12"
constexpr uint32_t kFourccNv12 = 0x3231564E;

/// 没有帧时的占位底色。用深色而不是白/灰：
/// 深底上任何一点画面内容都不会被误认为"背景"。
constexpr int kPlaceholderBg = 0x21;   // #212121

}  // namespace

VideoFrameWidget::VideoFrameWidget(QWidget* parent) : QWidget(parent) {
    // 这个控件只负责画，不接收焦点：现场终端上焦点应该留给按钮。
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    clearFrame();
}

QSize VideoFrameWidget::sizeHint() const {
    // 4:3 是摄像头原生比例（1640×1232 正好是 4:3），
    // 按它给 hint，布局不会先给一个奇怪的比例再跳变。
    return QSize(320, 240);
}

QSize VideoFrameWidget::minimumSizeHint() const {
    // 再小就看不清了。小屏上让布局挤压其它控件，而不是把这个框压没。
    return QSize(160, 120);
}

void VideoFrameWidget::setFrame(const VideoFrame& frame) {
    // 「什么都没有」与「格式不认识」是两件不同的事，必须先分开。
    //
    // 空帧（nv12 为空、几何为 0）就是**没有画面**——这是摄像头刚启动时
    // 的正常形态，界面该说「暂无画面」。若落到下面那条 fourcc 判断上，
    // 空帧的 fourcc 也是 0，就会被报成「不支持的像素格式」，把一个正常
    // 状态说成故障。（本套件 InvalidFrameClearsInsteadOfKeepingStaleImage
    // 就是这样抓到的。）
    if (frame.nv12.isEmpty() || frame.width == 0 || frame.height == 0) {
        clearFrame();
        return;
    }

    if (frame.fourcc != kFourccNv12) {
        // 有数据但格式不认：把未知格式当 NV12 画出来会得到一片彩色噪点，
        // 看起来像"摄像头坏了"，实际是格式不匹配。明确说明。
        clearFrame(QStringLiteral("不支持的像素格式"));
        return;
    }

    if (!frame.valid()) {
        // 几何不成立（stride < width 等）
        clearFrame(QStringLiteral("画面数据不完整"));
        return;
    }

    const size_t out_size =
        static_cast<size_t>(frame.width) * frame.height * 3;
    std::vector<uint8_t> rgb(out_size);

    if (!nv12ToRgb888(frame.nv12.constData(),
                      static_cast<size_t>(frame.nv12.size()),
                      frame.width, frame.height, frame.stride,
                      frame.uv_offset, rgb.data(), rgb.size())) {
        // 转换失败通常是源长度不足（中枢截断或几何不一致）。
        // 明确报出来，别画半帧。
        clearFrame(QStringLiteral("画面数据不完整"));
        return;
    }

    // QImage 需要自己的内存，这里拷贝一份 RGB。
    //
    // 能不能零拷贝？可以（QImage 支持外部缓冲），但那样 QImage 的生命周期
    // 就依赖 `rgb` 这个局部 vector——一旦 setFrame 返回就是悬空指针。
    // 而 QImage 走的是 copy-on-write 隐式共享，赋值给成员不会真的复制，
    // 所以这里的拷贝只有一次，代价是每帧 ~6MB memcpy（1640*1232*3），
    // 在 A55 上约 3~5ms，10fps 下可以接受。
    image_ = QImage(rgb.data(), static_cast<int>(frame.width),
                    static_cast<int>(frame.height),
                    static_cast<int>(frame.width) * 3,
                    QImage::Format_RGB888)
                 .copy();
    sequence_ = frame.sequence;
    placeholder_.clear();
    update();
}

void VideoFrameWidget::clearFrame(const QString& reason) {
    image_ = QImage();
    sequence_ = 0;
    placeholder_ = reason.isEmpty() ? QStringLiteral("暂无画面") : reason;
    update();
}

void VideoFrameWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event);
    QPainter painter(this);

    if (image_.isNull()) {
        // 占位态：深底 + 居中说明。不留白——留白与"框没接上"无法区分。
        painter.fillRect(rect(), QColor(kPlaceholderBg, kPlaceholderBg, kPlaceholderBg));
        painter.setPen(QColor(0xB0, 0xB0, 0xB0));
        painter.drawText(rect(), Qt::AlignCenter, placeholder_);
        return;
    }

    paintImageLetterboxed();
}

void VideoFrameWidget::paintImageLetterboxed() {
    QPainter painter(this);

    // 先铺黑底：等比缩放会在两侧或上下留出空白，
    // 铺成灰色会让人以为画面被裁掉了。
    painter.fillRect(rect(), Qt::black);

    const QSize target = image_.size().scaled(size(), Qt::KeepAspectRatio);
    // 居中放置。整数除法会让奇偶相差 1px，居中比贴左上角好看得多。
    const int x = (width() - target.width()) / 2;
    const int y = (height() - target.height()) / 2;

    // 缩放用 SmoothTransformation 还是 FastTransformation？
    // 用 Fast（最近邻）作为默认：本板 A55 上 1640→320 的平滑缩放约 8ms，
    // 而显示尺寸通常已经很接近源尺寸（1024 宽屏上框约 600 宽）。
    // 平滑缩放在这个尺寸下肉眼差异很小，但 CPU 占用翻倍。
    painter.drawImage(QRect(x, y, target.width(), target.height()), image_);
}

}  // namespace dashboard
}  // namespace weaknet

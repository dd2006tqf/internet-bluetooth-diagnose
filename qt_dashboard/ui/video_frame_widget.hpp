/**
 * @file video_frame_widget.hpp
 * @brief 摄像头实时画面框
 *
 * ## 这个控件只做三件事
 *
 *   1. 接收一帧 NV12 像素（裸指针 + 几何）
 *   2. 用 `nv12ToRgb888()` 转成 RGB888
 *   3. 等比缩放后画在框里，保持宽高比（letterbox）
 *
 * 取帧、D-Bus、线程统统不在这里——那是 `VideoFetchThread` 的事。
 * 分开的理由与 `chart_geometry` / `nv12_to_rgb` 一致：**转换算错**与
 * **取帧链路错**必须能分别定位。本控件在没有 D-Bus 的机器上也能单测。
 *
 * ## 为什么不用 QImage 直接贴 NV12
 *
 * Qt 5.15 没有 NV12 的 `QVideoFrameFormat` 独立转换路径（`QVideoFrame`
 * 在 QtMultimedia 里，而 sysroot 没有 QtMultimedia——见 CMakeLists 注释）。
 * 也不打算为此引入 QtMultimedia：本板要的是「能看见画面」，不是播放器。
 *
 * ## 没有帧时画什么
 *
 * 画一块深色底 + 一行说明文字。**不要留白**：留白与「框没接上」无法区分，
 * 而这块屏的使用者需要一眼看出「是没画面，还是画面是黑的」。
 */

#ifndef WEAKNET_QT_DASHBOARD_VIDEO_FRAME_WIDGET_HPP
#define WEAKNET_QT_DASHBOARD_VIDEO_FRAME_WIDGET_HPP

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QWidget>

#include <cstdint>

namespace weaknet {
namespace dashboard {

/**
 * @brief 一帧画面 + 它的元信息
 *
 * 用朴素结构体而不是 QImage：QImage 已经是「转好的、可绘制的东西」，
 * 而这里要表达的是「刚拿到、还是 NV12 的原始帧」。混在一起会让
 * 「转换发生在哪一步」变得含糊。
 */
struct VideoFrame {
    /// NV12 原始字节（Y 平面 + UV 平面，按 stride 排布）
    QByteArray nv12;

    uint32_t width = 0;         ///< 可见宽度
    uint32_t height = 0;        ///< 可见高度
    uint32_t stride = 0;        ///< Y 平面一行的字节数（本板实测 1640，不是 2460）
    /// UV 平面相对帧起始的字节偏移；0 = 等于 stride*height（标准紧凑 NV12）
    ///
    /// 本板 sunxi-vin 实测真实值是 2030336，而 stride*height = 2020480，
    /// 中间 9856 字节是驱动预留的对齐空洞（全零）。若按 stride*height 定位
    /// UV，会把这块空洞当色度读（U=V=0），画面严重偏色且不报错。
    uint32_t uv_offset = 0;
    uint32_t fourcc = 0;        ///< V4L2 四字符码；只认 NV12
    uint64_t sequence = 0;      ///< 采集序号，用于判断是否是新帧
    uint64_t timestamp_ns = 0;  ///< 采集时间戳

    bool valid() const {
        return !nv12.isEmpty() && width > 0 && height > 0 && stride >= width;
    }
};

/**
 * @brief 摄像头画面框
 *
 * 用法：
 *
 * ```cpp
 * auto* view = new VideoFrameWidget;
 * // 后台线程取到帧之后（在主线程）：
 * view->setFrame(frame);
 * ```
 *
 * `setFrame()` 会立即做 NV12→RGB 转换（1640×1232 约 10~20ms on A55）。
 * 这在 10fps 刷新下够用；若将来要更高帧率，转换应挪到取帧线程。
 */
class VideoFrameWidget : public QWidget {
    Q_OBJECT

public:
    explicit VideoFrameWidget(QWidget* parent = nullptr);

    /**
     * @brief 交给本控件一帧新画面
     *
     * 尺寸或格式变化时会重新分配内部缓冲。传入 `valid()==false` 的帧
     * 等同于 `clearFrame()`：显示「暂无画面」而不是保留上一帧——
     * 保留旧画面会让人以为画面是实时的。
     */
    void setFrame(const VideoFrame& frame);

    /// 清空画面，显示占位提示
    void clearFrame(const QString& reason = QString());

    /// 当前是否已有一帧可显示的画面（测试与状态查询用）
    bool hasFrame() const { return !image_.isNull(); }

    /// 当前帧的采集序号（没帧时为 0）
    uint64_t sequence() const { return sequence_; }

    /// 占位提示文字（没帧时显示的那行）
    QString placeholderText() const { return placeholder_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    /// 把 image_ 按保持宽高比的方式画进 rect（letterbox）
    void paintImageLetterboxed();

    QImage image_;              ///< 已转换的 RGB888 帧（可能为空）
    uint64_t sequence_ = 0;
    QString placeholder_;       ///< 没帧时显示的文字
};

}  // namespace dashboard
}  // namespace weaknet

#endif  // WEAKNET_QT_DASHBOARD_VIDEO_FRAME_WIDGET_HPP

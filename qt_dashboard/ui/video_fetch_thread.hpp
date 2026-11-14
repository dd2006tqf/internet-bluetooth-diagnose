/**
 * @file video_fetch_thread.hpp
 * @brief 后台取帧线程
 *
 * ## 为什么必须单独一个线程
 *
 * `weaknet_get_video_frame()` 内部走 D-Bus 同步调用。服务端忙、总线排队、
 * 或中枢正被别的消费者占着时，单次调用可能阻塞。放在主线程就是画面卡住、
 * 整个面板跟着僵。
 *
 * ## 为什么不复用 DataFetchThread
 *
 * 两者节奏完全不同：`DataFetchThread` 是**用户点一次刷新取一次**（几秒一次），
 * 视频要的是**持续 10fps**。混在一个线程里，取历史（可能几秒）会把取帧饿死，
 * 画面就一顿一顿的。
 *
 * ## 为什么永远只持有一帧
 *
 * 每次取帧成功后立刻 `weaknet_release_video_frame()` 归还上一帧的槽位。
 * 中枢的槽位只有 3 个，且「槽位没归还就不覆写」——持有多帧会让中枢
 * 很快无槽可写。服务端为此还做了兜底（同一 sender 再取自动归还上一次），
 * 但那是防呆，不是让我们可以偷懒的理由。
 */

#ifndef WEAKNET_QT_DASHBOARD_VIDEO_FETCH_THREAD_HPP
#define WEAKNET_QT_DASHBOARD_VIDEO_FETCH_THREAD_HPP

#include <QByteArray>
#include <QMetaType>
#include <QThread>

#include <cstdint>

namespace weaknet {
namespace dashboard {

/**
 * @brief 一帧从对端取回的画面（跨线程传递用）
 *
 * 单独定义而不复用 UI 层的 `VideoFrame`：取帧线程不应该依赖 Qt Widgets，
 * 否则在没有 Qt 开发包的机器上连线程层都编不了。
 */
struct FetchedFrame {
    QByteArray nv12;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    /// UV 平面偏移，原样来自服务端。0 = 等于 stride*height
    uint32_t uv_offset = 0;
    uint32_t fourcc = 0;
    uint64_t sequence = 0;
    uint64_t timestamp_ns = 0;
    bool valid = false;
};

/**
 * @brief 后台取帧线程
 *
 * 用法：
 *
 * ```cpp
 * auto* fetcher = new VideoFetchThread(this);
 * connect(fetcher, &VideoFetchThread::frameReady,
 *         this, &MainWindow::onVideoFrame);
 * connect(fetcher, &VideoFetchThread::frameUnavailable,
 *         this, &MainWindow::onVideoUnavailable);
 * fetcher->start();
 * ```
 *
 * 所有信号在**主线程**发射（Qt 自动排队）。
 */
class VideoFetchThread : public QThread {
    Q_OBJECT

public:
    explicit VideoFetchThread(QObject* parent = nullptr);
    ~VideoFetchThread() override;

    /// 设置取帧间隔（毫秒）。默认 100 = 10fps，与服务端中枢的发布限速一致。
    void setIntervalMs(int ms);

    /// 请求线程退出（等当前这一轮取完，不强杀）
    void stop();

signals:
    /// 取到一帧有效画面
    void frameReady(const weaknet::dashboard::FetchedFrame& frame);

    /// 服务端正常但没有帧（摄像头未启动 / 刚启动还没出第一帧）
    ///
    /// 与 `fetchFailed` 区别很重要：这是**正常形态**，界面上应显示
    /// 「暂无画面」而不是红色错误。
    void frameUnavailable();

    /// 取帧调用本身失败（未连接 / 服务端拒绝 / 协议不符）
    void fetchFailed(const QString& error);

protected:
    void run() override;

private:
    int interval_ms_ = 100;
};

}  // namespace dashboard
}  // namespace weaknet

// 跨线程信号参数必须注册元类型，否则 Qt 在 queued connection 时会拒绝投递
// （运行时报 "Cannot queue arguments of type 'FetchedFrame'"）。
// 声明放在命名空间之外，这是 Qt 的要求。
Q_DECLARE_METATYPE(weaknet::dashboard::FetchedFrame)

#endif  // WEAKNET_QT_DASHBOARD_VIDEO_FETCH_THREAD_HPP

/**
 * @file data_fetch_thread.hpp
 * @brief 后台取数线程
 *
 * ## 为什么必须单独一个线程
 *
 * `DataSource` 的每个方法最终都走 D-Bus 同步调用
 * （`dbus_connection_send_with_reply_and_block`）。服务端忙、D-Bus 排队、
 * 或连不上服务时，单次调用可能阻塞数秒。
 *
 * 如果在 Qt 主线程调用，`QApplication::exec()` 的消息循环会停摆，
 * 界面表现为「点了没反应 / 窗口变灰 / 标题栏出现未响应」。
 *
 * ## 模式来源
 *
 * 借鉴 monitor_system 的 `DataFetchThread`（讨论文档 4.3 已认定为值得复用的
 * 标准模式）：`QThread` 子类 + `QMutex` 保护共享状态 + 信号回主线程。
 *
 * 这里做了两处改进：
 *
 * 1. **不在 `run()` 里跑无限循环**，而是用 `QTimer` 驱动的一次性任务队列，
 *    便于测试和优雅退出
 * 2. **错误也通过信号回传**，而不是只打日志——看板需要把错误显示给用户
 */

#ifndef WEAKNET_QT_DASHBOARD_DATA_FETCH_THREAD_HPP
#define WEAKNET_QT_DASHBOARD_DATA_FETCH_THREAD_HPP

#include <QByteArray>
#include <QMutex>
#include <QString>
#include <QThread>

#include "data/dashboard_data.hpp"

namespace weaknet {
namespace dashboard {

/**
 * @brief 后台取数线程
 *
 * 用法：
 *
 * ```cpp
 * auto* fetcher = new DataFetchThread(this);
 * connect(fetcher, &DataFetchThread::experienceReady,
 *         this, &MainWindow::onExperience);
 * connect(fetcher, &DataFetchThread::fetchFailed,
 *         this, &MainWindow::onFetchError);
 * fetcher->start();
 * fetcher->requestRefresh();   // 可以反复调用
 * ```
 *
 * 所有信号都在**主线程**发射（Qt 自动排队），可以直接操作 UI。
 */
class DataFetchThread : public QThread {
    Q_OBJECT

public:
    explicit DataFetchThread(QObject* parent = nullptr);
    ~DataFetchThread() override;

    /**
     * @brief 请求一次完整刷新
     *
     * 非阻塞，立即返回。实际取数在后台线程执行。
     * 若上一次刷新还没结束，本次请求被丢弃（避免堆积）。
     */
    void requestRefresh();

    /**
     * @brief 设置历史查询参数
     *
     * 在下一次 `requestRefresh()` 生效。可在任意线程调用。
     *
     * @param interface_name 网卡名；空串表示全部
     * @param window         时间窗（决定 start 时间与 limit）
     *
     * 参数在**下一次刷新**时生效，不在这里同步取数：本方法是主线程调的，
     * 而取数在后台线程，同步取数会把界面卡住。
     */
    void setHistoryQuery(const QString& interface_name, HistoryWindow window);

    /**
     * @brief 请求线程退出
     *
     * 会等待当前这一轮取数完成后退出，不会强杀。
     * 若服务端无响应，最长可能等一个 D-Bus 超时。
     */
    void stop();

signals:
    /// 网络体验快照就绪（原始 JSON）
    void experienceReady(const QByteArray& json);

    /// 诊断事实就绪（原始 JSON）
    void diagnosisReady(const QByteArray& json);

    /// 历史趋势就绪（原始 JSON）
    void historyReady(const QByteArray& json);

    /// 任一取数失败。stage 标识是哪一步失败。
    void fetchFailed(const QString& stage, const QString& error);

    /// 一轮刷新完成（无论成败），可用于停止"加载中"动画
    void refreshFinished();

protected:
    void run() override;

private:
    // 这些成员只在后台线程读写，或由 QMutex 保护
    QMutex mutex_;
    bool refresh_pending_ = false;
    bool quit_requested_ = false;
    QString history_interface_;
    HistoryWindow history_window_ = HistoryWindow::Last1h;
};

} // namespace dashboard
} // namespace weaknet

#endif // WEAKNET_QT_DASHBOARD_DATA_FETCH_THREAD_HPP

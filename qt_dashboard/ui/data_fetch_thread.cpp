/**
 * @file data_fetch_thread.cpp
 * @brief DataFetchThread 实现
 */

#include "data_fetch_thread.hpp"

#include <QMutexLocker>
#include <QThread>

#include <ctime>

#include "data/data_source.hpp"

namespace weaknet {
namespace dashboard {

namespace {

/// 两次刷新之间的最小间隔，防止用户狂点按钮打爆服务端
constexpr int kMinRefreshIntervalMs = 500;

/**
 * @brief 初始化失败后的重试间隔
 *
 * 板端服务由 systemd 管理，冷启动时看板可能先于服务就绪；D-Bus 系统总线
 * 也可能在开机早期还没开始接受连接。此时 `weaknet_init()` 会失败，
 * 但几秒后再试就能成功。
 *
 * 取 2 秒：短到用户不会察觉"界面卡在等待"，长到不会在服务真的没装时
 * 把 CPU 和日志刷爆（一次失败会写一行 glog）。
 */
constexpr int kInitRetryMs = 2000;

} // namespace

DataFetchThread::DataFetchThread(QObject* parent) : QThread(parent) {}

DataFetchThread::~DataFetchThread() {
    stop();
    wait();
}

void DataFetchThread::requestRefresh() {
    QMutexLocker lock(&mutex_);
    refresh_pending_ = true;
}

void DataFetchThread::setHistoryQuery(const QString& interface_name,
                                      HistoryWindow window) {
    QMutexLocker lock(&mutex_);
    history_interface_ = interface_name;
    history_window_ = window;
}

void DataFetchThread::stop() {
    QMutexLocker lock(&mutex_);
    quit_requested_ = true;
}

void DataFetchThread::run() {
    DataSource source;

    // 初始化失败**不能直接 return**：那样线程就结束了，之后 requestRefresh()
    // 只是往一个死掉的线程里设标志位，定时器每 10 秒照常触发但界面永远停在
    // 第一条"取数失败"上。这里改成重试循环，直到成功或收到退出请求。
    //
    // 每轮重试只发一次 fetchFailed，避免每隔 2 秒刷屏；成功之后 state
    // 复位，下次再断连时还能重新提示。
    bool init_ok = source.init();
    bool failure_reported = false;
    while (!init_ok) {
        if (!failure_reported) {
            emit fetchFailed(QStringLiteral("init"),
                             QString::fromStdString(source.last_error()));
            // 也要发 refreshFinished：主线程用它清 refresh_in_flight_，
            // 否则手动刷新按钮会永久失效。之后线程仍在重试，成功后
            // 会自动取数并再发一次 refreshFinished。
            emit refreshFinished();
            failure_reported = true;
        }

        // 分片休眠：stop() 之后最多 kInitRetryMs 才退出，不会卡住析构
        for (int slept = 0; slept < kInitRetryMs; slept += 100) {
            {
                QMutexLocker lock(&mutex_);
                if (quit_requested_) {
                    return;
                }
            }
            QThread::msleep(100);
        }

        init_ok = source.init();
    }

    while (true) {
        bool should_quit = false;
        bool should_fetch = false;
        QString iface;
        HistoryWindow window = HistoryWindow::Last1h;

        {
            QMutexLocker lock(&mutex_);
            should_quit = quit_requested_;
            should_fetch = refresh_pending_;
            iface = history_interface_;
            window = history_window_;
            // 取走这次请求，避免重复执行
            refresh_pending_ = false;
        }

        if (should_quit) {
            break;
        }

        if (!should_fetch) {
            // 没有请求时短暂休眠，避免空转烧 CPU
            QThread::msleep(50);
            continue;
        }

        // --- 依次取三份数据，互不阻断 ---
        // 某一项失败不影响其它项，让界面尽可能多地显示信息

        const FetchResult exp = source.fetch_experience_raw();
        if (exp.ok) {
            emit experienceReady(QByteArray::fromStdString(exp.json));
        } else {
            emit fetchFailed(QStringLiteral("experience"),
                             QString::fromStdString(exp.error));
        }

        const FetchResult diag = source.fetch_diagnosis_raw();
        if (diag.ok) {
            emit diagnosisReady(QByteArray::fromStdString(diag.json));
        } else {
            emit fetchFailed(QStringLiteral("diagnosis"),
                             QString::fromStdString(diag.error));
        }

        const FetchResult hist = source.fetch_history_raw(
            iface.toStdString(), history_window_limit(window),
            history_window_start(window, std::time(nullptr)),
            std::string());
        if (hist.ok) {
            emit historyReady(QByteArray::fromStdString(hist.json));
        } else {
            emit fetchFailed(QStringLiteral("history"),
                             QString::fromStdString(hist.error));
        }

        emit refreshFinished();

        // 速率限制：避免连续请求
        QThread::msleep(kMinRefreshIntervalMs);
    }
}

} // namespace dashboard
} // namespace weaknet

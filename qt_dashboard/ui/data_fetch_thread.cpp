/**
 * @file data_fetch_thread.cpp
 * @brief DataFetchThread 实现
 */

#include "data_fetch_thread.hpp"

#include <QMutexLocker>
#include <QThread>

#include "data/data_source.hpp"

namespace weaknet {
namespace dashboard {

namespace {

/// 两次刷新之间的最小间隔，防止用户狂点按钮打爆服务端
constexpr int kMinRefreshIntervalMs = 500;

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

void DataFetchThread::setHistoryQuery(const QString& interface_name, int limit) {
    QMutexLocker lock(&mutex_);
    history_interface_ = interface_name;
    // 服务端会把 limit <= 0 钳制为 1，这里先自己兜底
    history_limit_ = (limit > 0) ? limit : 100;
}

void DataFetchThread::stop() {
    QMutexLocker lock(&mutex_);
    quit_requested_ = true;
}

void DataFetchThread::run() {
    DataSource source;

    // 初始化失败时不要静默退出：把原因发给 UI，让用户看到
    if (!source.init()) {
        emit fetchFailed(QStringLiteral("init"),
                         QString::fromStdString(source.last_error()));
        emit refreshFinished();
        return;
    }

    while (true) {
        bool should_quit = false;
        bool should_fetch = false;
        QString iface;
        int limit = 100;

        {
            QMutexLocker lock(&mutex_);
            should_quit = quit_requested_;
            should_fetch = refresh_pending_;
            iface = history_interface_;
            limit = history_limit_;
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

        const FetchResult hist =
            source.fetch_history_raw(iface.toStdString(), limit);
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

// looper.cpp
// 实现阻塞式 looper

#include <dbus/dbus.h>

#include "server.hpp"
#include "looper.hpp"
#include "logger.hpp"

namespace weaknet_dbus {

static thread_local Looper* t_looper = nullptr;

Looper* Looper::current() {
    if (!t_looper) t_looper = new Looper();
    return t_looper;
}

void Looper::attach(DBusConnection* conn) {
    conn_ = conn;
}

void Looper::run(ServerContext* ctx) {
    if (!conn_) {
        LOG_ERROR(LogModule::DBUS, "Looper::run: no connection attached");
        return;
    }

    LOG_INFO(LogModule::DBUS, "Looper::run: entering event loop");
    // 使用 1000ms 超时，以便定期检查 running 标志实现优雅退出
    // 同时检查 async-signal-safe 的 stop_requested 标志（signal handler 设置）
    while (ctx->running.load() && !Logger::stopRequested()) {
        if (!dbus_connection_read_write_dispatch(conn_, 1000)) {
            LOG_ERROR(LogModule::DBUS, "Looper::run: D-Bus connection lost, exiting event loop");
            break;  // 连接断开则退出循环
        }
    }
    // signal handler 设置了 stop_requested 时，同步通知 ctx.running
    if (Logger::stopRequested()) ctx->running.store(false);
    LOG_INFO(LogModule::DBUS, "Looper::run: event loop exited");
}

}  // namespace weaknet_dbus



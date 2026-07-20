// looper.cpp
// 实现阻塞式 looper

#include <dbus/dbus.h>

#include "server.hpp"
#include "looper.hpp"

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
    if (!conn_) return;

    // 使用 1000ms 超时，以便定期检查 running 标志实现优雅退出
    while (ctx->running.load()) {
        if (!dbus_connection_read_write_dispatch(conn_, 1000)) {
            break;  // 连接断开则退出循环
        }
    }
}

}  // namespace weaknet_dbus



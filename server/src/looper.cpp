// looper.cpp
// 实现阻塞式 looper

#include <dbus/dbus.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>

#include "server.hpp"
#include "looper.hpp"
#include "logger.hpp"

namespace weaknet_dbus {

static thread_local Looper* t_looper = nullptr;

// signal handler 通过写 pipe 通知 Looper，比设置 volatile flag 更可靠
// （dbus_connection_read_write_dispatch 可能阻塞，不返回，flag 检查不到）
static int s_stop_pipe[2] = {-1, -1};

static void looper_signal_handler(int signum) {
    // async-signal-safe: write() 是 async-signal-safe 的
    if (s_stop_pipe[1] >= 0) {
        char c = 1;
        (void)write(s_stop_pipe[1], &c, 1);
    }
}

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

    // 创建 self-pipe：signal handler 写一端，poll 读另一端
    if (pipe(s_stop_pipe) < 0) {
        LOG_ERROR(LogModule::DBUS, "Looper::run: pipe() failed, falling back to timeout mode");
        s_stop_pipe[0] = s_stop_pipe[1] = -1;
    }

    // 注册信号处理器
    struct sigaction sa = {};
    sa.sa_handler = looper_signal_handler;
    sa.sa_flags = 0;  // 不用 SA_RESTART，让 dbus 调用被中断
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    // 获取 D-Bus fd 用于 poll()
    int dbus_fd = -1;
    dbus_connection_get_socket(conn_, &dbus_fd);

    LOG_INFO(LogModule::DBUS, "Looper::run: entering event loop");

    if (s_stop_pipe[0] >= 0 && dbus_fd >= 0) {
        // poll 模式：同时监听 D-Bus fd 和 stop pipe
        struct pollfd fds[2];
        fds[0].fd = dbus_fd;
        fds[0].events = POLLIN;
        fds[1].fd = s_stop_pipe[0];
        fds[1].events = POLLIN;

        while (ctx->running.load()) {
            int ret = poll(fds, 2, 100);  // 100ms 超时，更快响应方法调用
            if (ret < 0) {
                if (errno == EINTR) continue;  // 信号中断，重试
                LOG_ERROR(LogModule::DBUS, "Looper::run: poll() error");
                break;
            }
            if (ret == 0) continue;  // 超时，检查 running

            // stop pipe 可读 → 收到信号
            if (fds[1].revents & POLLIN) {
                char buf[8];
                while (read(s_stop_pipe[0], buf, sizeof(buf)) > 0) {}  // 清空
                LOG_INFO(LogModule::DBUS, "Looper::run: received stop signal");
                ctx->running.store(false);
                break;
            }

            // D-Bus fd 可读 → 处理消息（循环 dispatch 直到没有更多消息）
            if (fds[0].revents & POLLIN) {
                while (dbus_connection_read_write_dispatch(conn_, 0)) {
                    // 继续 dispatch 直到没有更多消息
                }
            }
        }
    } else {
        // 降级模式：无 pipe 时用原有 timeout 循环
        while (ctx->running.load() && !Logger::stopRequested()) {
            if (!dbus_connection_read_write_dispatch(conn_, 1000)) {
                LOG_ERROR(LogModule::DBUS, "Looper::run: D-Bus connection lost");
                break;
            }
        }
        if (Logger::stopRequested()) ctx->running.store(false);
    }

    // 清理 pipe
    if (s_stop_pipe[0] >= 0) { close(s_stop_pipe[0]); s_stop_pipe[0] = -1; }
    if (s_stop_pipe[1] >= 0) { close(s_stop_pipe[1]); s_stop_pipe[1] = -1; }

    LOG_INFO(LogModule::DBUS, "Looper::run: event loop exited");
}

}  // namespace weaknet_dbus

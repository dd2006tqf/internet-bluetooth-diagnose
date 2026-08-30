/**
 * @file looper.cpp
 * @brief D-Bus 事件循环实现（单例 Looper）
 *
 * Looper 是 D-Bus 消息派发的核心驱动。D-Bus 本身不启动线程，
 * 所有消息处理都必须在调用 dbus_connection_read_write_dispatch
 * 的线程上完成。本文件实现的就是这个"唯一的 D-Bus 线程"。
 *
 * 两种运行模式：
 *   1. poll 模式（推荐）：同时监听 D-Bus fd 和 signal pipe
 *      - 通过 self-pipe trick 解决信号中断问题
 *      - 100ms 超时快速响应外部状态变化
 *   2. timeout 模式（降级）：pipe 创建失败时使用
 *      - 依赖 dbus_connection_read_write_dispatch(conn_, 1000) 的 1 秒超时
 *      - 需要 Logger::stopRequested() 配合（signal handler 标记 stop）
 *
 * self-pipe trick 设计原理：
 *   原始 signal handler 只能设置 volatile flag，但 dbus_connection_read_write_dispatch
 *   可能阻塞在 poll/select 上看不到 flag 变化。解法是创建 pipe，
 *   signal handler 写一端，事件循环 poll 读一端，这样信号能立即唤醒 poll。
 */

#include <dbus/dbus.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include <memory>
#include <cerrno>

#include "server.hpp"
#include "looper.hpp"
#include "logger.hpp"

namespace weaknet_dbus {

/// 每个线程独立的 Looper 实例（单例 + thread_local）
static thread_local std::unique_ptr<Looper> t_looper;

/**
 * @brief self-pipe：signal handler 写一端，poll 循环读另一端
 *
 * 为什么用 pipe 而不是 volatile flag？
 *   dbus_connection_read_write_dispatch() 内部可能阻塞在 poll/epoll_wait 上，
 *   flag 设置后 poll 不会返回，事件循环收不到信号。
 *   pipe 写入会让 poll 的 POLLIN 立即触发，保证信号不丢失。
 *
 * 为什么 signal handler 里能用 write()？
 *   write() 是 POSIX async-signal-safe 函数，signal handler 中可以安全调用。
 *   我们只写 1 字节，几乎不会阻塞（pipe 缓冲区通常 4KB）。
 */
static int s_stop_pipe[2] = {-1, -1};

/**
 * @brief SIGTERM/SIGINT 处理器：写 pipe 通知 Looper 退出
 *
 * async-signal-safe 约束：只能调用 write、_exit、signal 等少数函数，
 * 严禁调用 malloc/printf/mutex_lock 等非 async-signal-safe 函数。
 * 这里只做一件事：向 pipe 写 1 字节，让 poll 立即返回。
 */
static void looper_signal_handler(int signum) {
    if (s_stop_pipe[1] >= 0) {
        char c = 1;
        (void)write(s_stop_pipe[1], &c, 1);  // 忽略返回值（pipe 满时丢弃）
    }
}

// ---- Looper 单例实现 ----

/**
 * @brief 获取当前线程的 Looper 实例（懒汉构造）
 *
 * 首次调用时通过 Looper::create() 生成实例存入 thread_local，
 * 后续调用直接返回缓存指针。
 */
Looper* Looper::current() {
    if (!t_looper) t_looper = Looper::create();
    return t_looper.get();
}

/// 将 D-Bus 连接对象绑定到 Looper（Looper 不拥有 conn_ 的所有权）
void Looper::attach(DBusConnection* conn) {
    conn_ = conn;
}

/**
 * @brief 启动 D-Bus 事件主循环（阻塞直到收到退出信号）
 *
 * @param ctx  ServerContext 生命周期句柄；ctx->running 为退出条件
 *
 * 执行顺序：
 *   1. 验证 conn_ 非空，否则报错返回
 *   2. 创建 self-pipe（失败则降级到 timeout 模式）
 *   3. 注册 SIGTERM/SIGINT signal handler（通过 sigaction）
 *   4. 进入 poll 循环（或降级的 timeout 循环）
 *   5. 收到退出条件后清理 pipe fd，正常返回
 *
 * 退出条件（任一满足）：
 *   - ctx->running.store(false)（外部请求退出）
 *   - poll 返回错误（非 EINTR）
 *   - dbus_connection_read_write_dispatch 返回 false（连接丢失）
 *   - Logger::stopRequested()（timeout 模式下，signal handler 标记）
 */
void Looper::run(ServerContext* ctx) {
    // ---- 前置检查 ----
    if (!conn_) {
        LOG_ERROR(LogModule::DBUS, "Looper::run: no connection attached");
        return;
    }

    // ---- 创建 self-pipe ----
    // pipe() 失败（EMFILE 或 ENFILE）时设为 -1，后续走降级路径
    if (pipe(s_stop_pipe) < 0) {
        LOG_ERROR(LogModule::DBUS, "Looper::run: pipe() failed, falling back to timeout mode");
        s_stop_pipe[0] = s_stop_pipe[1] = -1;
    }

    // ---- 注册信号处理器 ----
    // 不设 SA_RESTART：让被信号中断的系统调用返回 EINTR 而不是自动重入，
    // 这样 dbus_connection_read_write_dispatch 能及时返回让我们检查退出条件
    struct sigaction sa = {};
    sa.sa_handler = looper_signal_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    // ---- 获取 D-Bus socket fd ----
    int dbus_fd = -1;
    dbus_connection_get_socket(conn_, &dbus_fd);

    LOG_INFO(LogModule::DBUS, "Looper::run: entering event loop");

    // ---- poll 模式（推荐） ----
    // 同时监听 D-Bus 消息 fd 和 signal pipe；100ms 超时快速响应状态变化
    if (s_stop_pipe[0] >= 0 && dbus_fd >= 0) {
        struct pollfd fds[2];
        fds[0].fd = dbus_fd;          // 索引 0：D-Bus fd
        fds[0].events = POLLIN;
        fds[1].fd = s_stop_pipe[0];   // 索引 1：signal pipe 读端
        fds[1].events = POLLIN;

        while (ctx->running.load()) {
            // 100ms 超时：既保证及时收到信号，又定期检查 ctx->running
            int ret = poll(fds, 2, 100);
            if (ret < 0) {
                if (errno == EINTR) continue;  // 信号中断，重试
                LOG_ERROR(LogModule::DBUS, "Looper::run: poll() error");
                break;
            }
            if (ret == 0) continue;  // 超时无事件，回去检查 running flag

            // 收到退出信号（signal handler 写入）
            if (fds[1].revents & POLLIN) {
                char buf[8];
                // 循环 read 直到 pipe 清空（pipe 缓冲区可能有多字节积压）
                while (read(s_stop_pipe[0], buf, sizeof(buf)) > 0) {}
                LOG_INFO(LogModule::DBUS, "Looper::run: received stop signal");
                ctx->running.store(false);
                break;
            }

            // D-Bus fd 可读：循环 dispatch 直到队列清空
            // dbus_connection_read_write_dispatch(conn_, 0) 中 timeout=0 表示非阻塞
            if (fds[0].revents & POLLIN) {
                while (dbus_connection_read_write_dispatch(conn_, 0)) {
                    // 内部会读取、解析、分发 D-Bus 消息到注册的回调处理器
                }
            }
        }
    } else {
        // ---- 降级模式 ----
        // pipe 创建失败或 dbus_fd 无效时的兜底：依赖 dbus 内部 1 秒超时
        // 退出条件额外检查 Logger::stopRequested()（signal handler 设置的 flag）
        while (ctx->running.load() && !Logger::stopRequested()) {
            if (!dbus_connection_read_write_dispatch(conn_, 1000)) {
                LOG_ERROR(LogModule::DBUS, "Looper::run: D-Bus connection lost");
                break;
            }
        }
        if (Logger::stopRequested()) ctx->running.store(false);
    }

    // ---- 清理 ----
    if (s_stop_pipe[0] >= 0) { close(s_stop_pipe[0]); s_stop_pipe[0] = -1; }
    if (s_stop_pipe[1] >= 0) { close(s_stop_pipe[1]); s_stop_pipe[1] = -1; }

    LOG_INFO(LogModule::DBUS, "Looper::run: event loop exited");
}

}  // namespace weaknet_dbus

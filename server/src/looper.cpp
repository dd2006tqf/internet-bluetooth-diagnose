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
#include <fcntl.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <memory>
#include <cerrno>
#include <exception>
#include <utility>

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

/// 实现见 looper.hpp 的契约说明（该函数同时被单元测试直接验证）
bool dispatchUntilDrained(DBusConnection* conn, int max_rounds) {
    for (int round = 0; round < max_rounds; ++round) {
        if (!dbus_connection_read_write_dispatch(conn, 0)) return false;
        if (dbus_connection_get_dispatch_status(conn) != DBUS_DISPATCH_DATA_REMAINS) {
            return true;  // 队列已清空
        }
    }
    return true;
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
Looper::~Looper() {
    // 关闭任务队列：此后 postToMainThread 一律拒绝，避免任务入队后无人消费。
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        accepting_tasks_ = false;
        tasks_.clear();
    }
    if (wake_fd_ >= 0) {
        close(wake_fd_);
        wake_fd_ = -1;
    }
}

void Looper::wakeup() {
    if (wake_fd_ < 0) return;
    // eventfd 语义：write 一个 8 字节的 uint64 作为计数。写 1 即可让 poll
    // 立刻返回；多个后台线程同时写也只会累加计数，不会丢失唤醒。
    // 用 EFD_NONBLOCK 创建，管道满（计数值溢出）时返回 EAGAIN —— 那种情况
    // 说明已经有大量未处理的唤醒，本就无需再加。
    const uint64_t one = 1;
    ssize_t n;
    do {
        n = write(wake_fd_, &one, sizeof(one));
    } while (n < 0 && errno == EINTR);
    (void)n;
}

int Looper::drainWakeups() {
    if (wake_fd_ >= 0) {
        uint64_t counter = 0;
        // 一次 read 就把累计计数取出并清零，不需要循环（eventfd 的读是原子的）。
        ssize_t n;
        do {
            n = read(wake_fd_, &counter, sizeof(counter));
        } while (n < 0 && errno == EINTR);
        (void)n;
    }

    // 把队列整体换出再执行：任务本身可能调用 postToMainThread（链式任务），
    // 持锁执行会自锁。换出后新的任务进下一轮，天然有界。
    std::deque<MainThreadTask> batch;
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        batch.swap(tasks_);
    }
    int executed = 0;
    for (auto& task : batch) {
        if (!task) continue;
        try {
            task();
        } catch (const std::exception& ex) {
            // 一条坏任务不得让整个事件循环停止排空。
            LOG_ERROR(LogModule::DBUS, "Looper: main-thread task threw: " << ex.what());
        } catch (...) {
            LOG_ERROR(LogModule::DBUS, "Looper: main-thread task threw unknown exception");
        }
        ++executed;
    }
    return executed;
}

bool Looper::postToMainThread(MainThreadTask task) {
    if (!task) return false;
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        if (!accepting_tasks_) return false;
        tasks_.push_back(std::move(task));
    }
    wakeup();
    return true;
}

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
    } else {
        // 两端均设 O_NONBLOCK：
        // - 读端：排空循环用 `while (read(...) > 0)` 清空管道，阻塞模式下管道清空后
        //   第二次 read 会永久挂起（SIGTERM 后主线程卡死在 pipe_read，systemd 只能
        //   等 TimeoutStopSec 后 SIGKILL）。
        // - 写端：signal handler 中的 write 不因管道满而阻塞（async-signal-safe 约束）。
        for (int& fd : s_stop_pipe) {
            const int flags = fcntl(fd, F_GETFL, 0);
            if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
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

    // ---- 创建 eventfd（后台线程唤醒通道） ----
    // 失败不致命：退回"只靠 100ms 超时轮询"，代价是排队到主线程的回复最多
    // 多等一个超时周期。可用性优先于延迟。
    if (wake_fd_ < 0) {
        wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wake_fd_ < 0) {
            LOG_ERROR(LogModule::DBUS,
                      "Looper::run: eventfd() failed (errno=" << errno
                      << "), falling back to timeout-only wakeups");
        }
    }

    // ---- 获取 D-Bus socket fd ----
    int dbus_fd = -1;
    dbus_connection_get_socket(conn_, &dbus_fd);

    LOG_INFO(LogModule::DBUS, "Looper::run: entering event loop");

    // ---- poll 模式（推荐） ----
    // 同时监听 D-Bus 消息 fd 和 signal pipe；100ms 超时快速响应状态变化
    if (s_stop_pipe[0] >= 0 && dbus_fd >= 0) {
        struct pollfd fds[3];
        fds[0].fd = dbus_fd;          // 索引 0：D-Bus fd
        fds[0].events = POLLIN;
        fds[1].fd = s_stop_pipe[0];   // 索引 1：signal pipe 读端
        fds[1].events = POLLIN;
        // 索引 2：后台工作线程唤醒（-1 时 poll 直接忽略该槽位）
        fds[2].fd = wake_fd_;
        fds[2].events = POLLIN;

        while (ctx->running.load()) {
            // 100ms 超时：既保证及时收到信号，又定期检查 ctx->running
            int ret = poll(fds, 3, 100);
            if (ret < 0) {
                if (errno == EINTR) continue;  // 信号中断，重试
                LOG_ERROR(LogModule::DBUS, "Looper::run: poll() error");
                break;
            }

            // 收到退出信号（signal handler 写入）
            if (fds[1].revents & POLLIN) {
                char buf[8];
                // 循环 read 直到 pipe 清空（pipe 缓冲区可能有多字节积压）
                while (read(s_stop_pipe[0], buf, sizeof(buf)) > 0) {}
                LOG_INFO(LogModule::DBUS, "Looper::run: received stop signal");
                ctx->running.store(false);
                break;
            }

            // 后台工作线程排队到主线程的动作（DB 查询回复等）。
            // 必须排在 D-Bus 派发之前：这些任务本身就是"把算好的结果发出去"，
            // 早一轮发出能让调用方少等一个 poll 周期。
            // 不看 revents，每轮都排空——与下面 D-Bus 派发同理，唤醒计数
            // 只是加速信号，不是执行前提。
            drainWakeups();

            // D-Bus 派发
            //
            // 历史行为（已修复的缺陷）：每次 POLLIN 只派发一次，且只在 POLLIN
            // 分支里派发。该写法假设“若仍有数据到达，poll 会立即再次唤醒”，
            // 但两个前提都不成立：
            //
            //   1. 单次 read_write_dispatch 只派发**一条**消息。多个监控线程
            //      持续向系统总线发送消息时 libdbus 内部队列积压，外部方法调用
            //      要排队等后续轮次。实测 GetInterfaces 延迟呈
            //      13ms / 300ms / 1000ms / 3000ms 的离散阶梯，最坏 25s。
            //   2. 内部队列有积压但 socket 已读空时，poll 不再报告 POLLIN。
            //      实测：队列剩余数据时 poll(fd, POLLIN, 300ms) 返回 0（超时），
            //      即积压期间主线程会一直睡到 100ms 超时，且醒来后若判定为
            //      超时就直接 continue、不派发 → 积压只能等下一条 socket 数据
            //      才被处理。
            //
            // 因此这里的正确写法是：**每一轮循环都派发到队列为空**，
            // 不看 revents。POLLIN 只是“有新数据”的加速信号，不是派发前提。
            //
            // 上限不能去掉：持续有内部消息时无界 while 会饿死 self-pipe 的
            // 停止信号（systemd 停服要等 TimeoutStopSec 后 SIGKILL）。
            // 64 轮足以清空正常负载下的队列，超出则回到 poll 让信号有机会被处理。
            if (fds[0].revents & POLLIN) {
                if (!dispatchUntilDrained(conn_, kMaxDispatchRounds)) {
                    LOG_ERROR(LogModule::DBUS, "Looper::run: D-Bus connection lost");
                    ctx->running.store(false);
                    break;
                }
            } else if (dbus_connection_get_dispatch_status(conn_) == DBUS_DISPATCH_DATA_REMAINS) {
                // poll 超时但内部队列仍有积压：主动排空，避免积压等到下一条
                // socket 数据才被处理（上面实测过的 100ms 阶梯来源）。
                if (!dispatchUntilDrained(conn_, kMaxDispatchRounds)) {
                    LOG_ERROR(LogModule::DBUS, "Looper::run: D-Bus connection lost");
                    ctx->running.store(false);
                    break;
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
    // 先关任务队列：之后入队的任务会被拒绝，说明它们的主线程已不再服务。
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        accepting_tasks_ = false;
    }
    // eventfd 由析构函数关闭（run 可能被重复调用，fd 生命周期与对象一致）。
    if (s_stop_pipe[0] >= 0) { close(s_stop_pipe[0]); s_stop_pipe[0] = -1; }
    if (s_stop_pipe[1] >= 0) { close(s_stop_pipe[1]); s_stop_pipe[1] = -1; }

    LOG_INFO(LogModule::DBUS, "Looper::run: event loop exited");
}

}  // namespace weaknet_dbus

/**
 * @file looper.hpp
 * @brief D-Bus 主线程事件循环封装
 *
 * Looper 是一个单例，在 start_server() 中被创建，
 * 由主线程调用 run() 阻塞处理 D-Bus 消息，直到 ServerContext::running_ 变为 false。
 *
 * 两条运行路径：
 *   1. poll 模式（主路径）：poll() 同时监听 D-Bus socket 与 self-pipe（停止信号），
 *      100ms 超时；每轮唤醒后由 dispatchUntilDrained() 派发到队列清空。
 *   2. 降级模式：pipe 创建失败或拿不到 D-Bus fd 时，退回
 *      dbus_connection_read_write_dispatch(conn, 1000) 的 1 秒轮询，
 *      配合 Logger::stopRequested() 退出。
 *
 * 为什么不是 epoll：
 *   只需要监听两个 fd 且超时精度要求不高，poll() 足够且代码更短。
 *
 * 派发语义的三个坑（2026-11-01 实测，详见 dispatchUntilDrained 注释）：
 *   一次 read_write_dispatch 只派发一条消息；内部队列积压时 poll 不再报
 *   POLLIN；无界派发会饿死 self-pipe 的停止信号。三者共同决定了
 *   "每轮都派发 + 清空即停 + 轮次上限" 的写法。
 */

#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <mutex>

struct DBusConnection;

namespace weaknet_dbus {

class ServerContext;

/**
 * @brief 单轮事件循环内最多派发的消息条数
 *
 * 一次 dbus_connection_read_write_dispatch 只派发一条消息，因此这个上限
 * 等价于“单轮最多处理多少条积压消息”。取 64 的理由：
 *   - 正常负载下 libdbus 内部队列远小于此值（实测积压个位数）；
 *   - 足够小，超限回到 poll 后 self-pipe 的停止信号能被及时处理，
 *     不会拖长 systemd 停服时间。
 *
 * 无界 while 会在持续有内部消息时永久饿死停止信号，导致 SIGTERM/SIGINT
 * 失效（systemd 停服只能等 TimeoutStopSec 后 SIGKILL）。
 */
inline constexpr int kMaxDispatchRounds = 64;

/**
 * @brief 派发 libdbus 内部队列直到清空，带轮次上限
 *
 * 为什么需要它，而不是直接调一次 read_write_dispatch？
 *   `dbus_connection_read_write_dispatch` 的语义是“读一次 + 派发**一条**消息”
 *   （实测：队列积压 N 条时，一次调用只让回调计数 +1，dispatch_status 仍是
 *   DATA_REMAINS）。只有反复调用才能清空队列。若只调一次，外部方法调用
 *   就要排队等后续事件循环轮次——2026-11-01 实测 GetInterfaces 延迟呈
 *   13ms / 300ms / 1000ms / 3000ms 的离散阶梯，最坏 25s。
 *
 * 为什么必须带上限？
 *   read_write_dispatch 的返回值语义是“连接未断开”而不是“还有数据”，
 *   无界 while 在持续有内部消息时会饿死停止信号（见 kMaxDispatchRounds）。
 *
 * @param conn       D-Bus 连接（不拥有所有权）
 * @param max_rounds 最多派发轮次，必须为正
 * @return false 表示连接已断开，调用方应退出事件循环
 *
 * @note 该函数有真实生产调用方（Looper::run），同时被
 *       server/test/unit/test_looper_dispatch_gtest.cpp 直接验证。
 */
bool dispatchUntilDrained(DBusConnection* conn, int max_rounds);

/**
 * @brief D-Bus 事件循环封装（单例模式）
 *
 * 职责：
 *   1. attach() 绑定外部创建的 DBusConnection
 *   2. run() 阻塞处理 D-Bus 消息，直到 ctx->running 被置 false
 *   3. 不拥有 DBusConnection 的所有权
 */
class Looper {
public:
    /**
     * @brief 获取单例实例（nullptr 表示 Looper 尚未创建）
     * @return 全局 Looper 指针
     */
    static Looper* current();

    /**
     * @brief 绑定 D-Bus 会话总线连接
     *
     * 必须在 run() 之前调用，且 conn 的生命周期必须覆盖 run() 全程。
     * Looper 不负责关闭连接。
     *
     * @param conn D-Bus 连接指针（由 init_dbus 创建）
     */
    void attach(DBusConnection* conn);

    /**
     * @brief 阻塞运行，处理 D-Bus 消息直到 ctx->running 为 false
     *
     * poll 模式下每轮：poll(2 fds, 100ms) → 检查停止信号 → 派发到队列清空。
     * 正常退出后 conn_ 保持不变（由调用者负责关闭 DBusConnection）。
     *
     * @param ctx 全局上下文，线程安全读取 running_ 标志
     */
    void run(ServerContext* ctx);

    /**
     * @brief 创建 Looper 实例（唯一的公开构造入口）
     * @return unique_ptr 管理的 Looper
     */
    static std::unique_ptr<Looper> create() {
        return std::unique_ptr<Looper>(new Looper());
    }

    /**
     * @brief 唤醒事件循环（线程安全，可从任意线程调用）
     *
     * 用途：把**只有主线程能做**的收尾工作（典型是 D-Bus 回复的构造与
     * 发送）排队回主线程。后台工作线程算完结果后调用本函数，poll 立即
     * 返回并执行 drainWakeups()。
     *
     * 为什么必须回主线程发回复：libdbus 并非在所有路径上都线程安全，
     * 而本项目的全部 D-Bus 收发历来只发生在主派发线程上。让工作线程
     * 直接 dbus_connection_send 会把这个前提打破，且出问题时表现为
     * 随机崩溃而非错误码。回复的构造与发送必须留在主线程。
     *
     * 实现用 eventfd：可写可 poll、自带计数、不需要 pipe 的读写两端。
     */
    void wakeup();

    /**
     * @brief 排空唤醒计数并执行排队的主线程任务
     *
     * 由 run() 在每轮 poll 返回后调用。任务按 FIFO 执行，单个任务抛异常
     * 不会中断其余任务（否则一条坏任务会让整个事件循环停止排空）。
     *
     * @return 执行的任务数（0 表示本轮没有排队任务）
     */
    int drainWakeups();

    /// 主线程任务队列类型。shared_ptr 而非 unique_ptr：允许任务在
    /// 入队方已销毁后仍被执行（析构顺序不可控）。
    using MainThreadTask = std::function<void()>;

    /**
     * @brief 把一个任务排到主线程执行；可从任意线程调用。
     * @return false 表示队列已关闭（Looper 正在退出），任务未入队
     */
    bool postToMainThread(MainThreadTask task);

    /// 析构必须公开：`std::unique_ptr<Looper>` 的默认删除器要访问它
    /// （默认删除器不是 Looper 的友元，私有析构会直接编译失败）。
    /// 这不会泄漏构造入口——默认构造仍为私有，唯一创建路径是 create()。
    ~Looper();

private:
    Looper() = default;    ///< 私有构造：仅通过 create() 创建
    Looper(const Looper&) = delete;
    Looper& operator=(const Looper&) = delete;

    DBusConnection* conn_ = nullptr;  ///< 绑定的 D-Bus 连接（弱引用，不负责生命周期）

    /// eventfd：后台线程写 1 即可唤醒 poll。（-1 = 创建失败，退回纯超时）
    int wake_fd_ = -1;
    /// 主线程任务队列。mutex 保护 queue_ 与 accepting_tasks_。
    std::mutex tasks_mutex_;
    std::deque<MainThreadTask> tasks_;
    /// run() 退出后置 false：之后入队的任务会被拒绝，避免队列永不消费。
    bool accepting_tasks_ = true;
};

}  // namespace weaknet_dbus

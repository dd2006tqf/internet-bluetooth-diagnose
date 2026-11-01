/**
 * @file looper.hpp
 * @brief D-Bus 主线程事件循环封装
 *
 * Looper 是一个单例，在 start_server() 中被创建，
 * 由主线程调用 run() 阻塞处理 D-Bus 消息，直到 ServerContext::running_ 变为 false。
 * 内部使用 dbus_connection_read_write_dispatch 非阻塞调用，
 * 配合 epoll 或 dbus_connection_get_dispatch_status 实现超时退出机制。
 *
 * 设计思路：
 *   D-Bus 库没有提供优雅的"从 dispatch 中跳出"API。Looper 通过
 *   dbus_connection_read_write_dispatch(1000) 以 1 秒超时轮询，
 *   每次超时后检查 ctx->running_，为 false 则退出循环。
 */

#pragma once

#include <memory>

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
     * 内部循环：dbus_connection_read_write_dispatch(1000) → 检查 running_ → 继续或退出
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

private:
    Looper() = default;    ///< 私有构造：仅通过 create() 创建
    DBusConnection* conn_ = nullptr;  ///< 绑定的 D-Bus 连接（弱引用，不负责生命周期）
};

}  // namespace weaknet_dbus

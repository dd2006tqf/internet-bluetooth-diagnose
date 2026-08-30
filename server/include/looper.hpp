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

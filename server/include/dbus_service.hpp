/**
 * @file dbus_service.hpp
 * @brief D-Bus 服务层：方法处理 + 信号发射
 *
 * DbusService 封装了 libdbus-1 C API 的样板代码，提供：
 *   - register_on_connection() 将自身方法表注册到 DBusConnection
 *   - emitChanged() / emitSpecificSignal() 发射主动通知信号
 *   - handleXxx() 系列方法：供静态回调函数转发调用（避免头文件暴露 dbus 类型）
 *
 * 设计模式：
 *   D-Bus 要求回调函数签名为自由函数，且不允许捕获 this。
 *   因此使用静态函数 MessageHandlerStatic → 从 DBusConnection user_data
 *   取回 DbusService* → 调用成员函数 handleXxx。
 *
 * 线程安全：
 *   send_mutex_ 保护对同一 DBusConnection 的并发信号发射写操作
 *   （libdbus-1 的部分内部结构在多线程写入下不安全）
 */

#pragma once

#include <string>
#include <vector>
#include <mutex>

struct DBusConnection;
struct DBusMessage;

// 前置声明监控器类型（仅用于指针成员，避免头文件依赖）
namespace weaknet_dbus {
class DnsMonitor;
class WifiPacketLossMonitor;
class HttpLatencyMonitor;
class ProcessNetProfiler;
}

namespace weaknet_dbus {

class ServerContext;  ///< 前置声明

/**
 * @brief D-Bus 服务实现
 *
 * 构造时接收 ServerContext 指针（非 owning），
 * 所有 handleXxx 方法通过 ctx_ 访问 weak_mgr / bt_monitor / eBPF 监控器。
 */
class DbusService {
public:
    /**
     * @brief 构造：绑定 ServerContext
     * @param ctx 全局上下文（非 owning，必须在 DbusService 析构前保持有效）
     */
    explicit DbusService(ServerContext* ctx);

    ~DbusService() = default;

    /**
     * @brief 注册方法分发器到 D-Bus 连接
     *
     * 注册 kObjectPath 对象路径和 kMethodXxx 方法的分发回调。
     * 内部通过 dbus_connection_add_filter 安装一个全局过滤器，
     * 在过滤器中通过 DBUS_MESSAGE_TYPE_METHOD_CALL 判断并路由。
     *
     * @param conn 已注册服务名的 DBusConnection
     * @return true 注册成功，false 失败（通常是 conn 为空）
     */
    bool register_on_connection(DBusConnection* conn);

    // ---------- 信号发射接口 ----------

    /**
     * @brief 发射通用 Changed 信号
     *
     * 携带 message（文本）和 counter（单调递增计数器，调试用）。
     * 这是旧版兼容信号，新功能应使用 emitSpecificSignal 指定更精确的信号名。
     *
     * @param message 信号载荷文本
     * @param counter 计数器值（通常由 EventManager 维护）
     * @return true 发射成功
     */
    bool emitChanged(const std::string& message, int32_t counter);

    /**
     * @brief 发射指定类型的事件信号
     *
     * 支持 kSignalInterfaceChanged / kSignalBluetoothDeviceChanged 等所有具体信号名。
     * 内部实现带 2 次重试（间隔 10ms），应对 D-Bus daemon 短暂不可用。
     *
     * @param signalName 信号名（必须是 common.hpp 中定义的常量之一）
     * @param message    信号载荷文本
     * @param counter    事件计数器
     * @return true 至少一次发射成功
     */
    bool emitSpecificSignal(const std::string& signalName, const std::string& message, int32_t counter);

    /**
     * @brief 发射带详细信息的 NetworkQualityChanged 信号
     *
     * 与通用信号不同，此方法携带两个字符串载荷（message + details），
     * details 为 JSON 格式，包含各维度指标和权重评分。
     *
     * @param message 简短描述（如 "Quality changed to GOOD"）
     * @param details JSON 格式详细信息
     * @param counter 事件计数器
     */
    bool emitNetworkQualitySignal(const std::string& message, const std::string& details, int32_t counter);

    // ---------- 方法实现（供静态分发函数调用） ----------

    // 基础查询方法
    bool handleGet(DBusConnection* conn, DBusMessage* msg);                  ///< Get: 返回服务标识字符串
    bool handleListInterfaces(DBusConnection* conn, DBusMessage* msg);       ///< ListInterfaces: 返回网卡名数组
    bool handleHealthCheck(DBusConnection* conn, DBusMessage* msg);          ///< HealthCheck: 执行完整健康检查
    bool handlePing(DBusConnection* conn, DBusMessage* msg);                 ///< Ping: 对指定主机执行 ICMP

    // 蓝牙相关方法
    bool handleGetBluetoothDevices(DBusConnection* conn, DBusMessage* msg);   ///< GetBluetoothDevices: 返回设备列表
    bool handleGetBluetoothAdapter(DBusConnection* conn, DBusMessage* msg);   ///< GetBluetoothAdapter: 返回适配器状态

    // eBPF 监控数据方法
    bool handleGetDnsStats(DBusConnection* conn, DBusMessage* msg);           ///< GetDnsStats: DNS 监控最近统计
    bool handleGetWifiLossStats(DBusConnection* conn, DBusMessage* msg);      ///< GetWifiLossStats: Wi-Fi 丢包归因
    bool handleGetHttpLatencyStats(DBusConnection* conn, DBusMessage* msg);  ///< GetHttpLatencyStats: HTTP TTFB 统计
    bool handleGetProcessProfiling(DBusConnection* conn, DBusMessage* msg);  ///< GetProcessProfiling: 进程画像
    bool handleGetEbpfMonitorHealth(DBusConnection* conn, DBusMessage* msg);  ///< GetEbpfMonitorHealth: 各监控器健康状态

    // 历史数据
    bool handleGetHistory(DBusConnection* conn, DBusMessage* msg);            ///< GetHistory: SQLite 历史查询

private:
    /**
     * @brief 内部辅助：向调用方回复字符串数组
     * @param conn D-Bus 连接
     * @param msg  原始方法调用消息（用于创建 reply）
     * @param arr  要返回的字符串数组
     */
    bool replyStringArray(DBusConnection* conn, DBusMessage* msg, const std::vector<std::string>& arr);

    /**
     * @brief 发射 D-Bus 信号（带重试的底层实现）
     *
     * @param signalName 信号名
     * @param args       可变载荷（每个 pair 的 first 是 D-Bus 类型字符，second 是指向值的指针）
     *                   支持 's' (string) / 'i' (int32_t) / 'u' (uint32_t)
     * @param counter    事件计数器
     */
    bool sendSignalInternal(const std::string& signalName,
                            const std::vector<std::pair<int, const void*>>& args,
                            int32_t counter);

private:
    ServerContext* ctx_;     ///< 全局上下文非 owning 指针
    std::mutex send_mutex_; ///< 保护对同一 DBusConnection 的并发信号写（libdbus-1 线程安全边界）
};

}  // namespace weaknet_dbus

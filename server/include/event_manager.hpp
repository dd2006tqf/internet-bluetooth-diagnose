/**
 * @file event_manager.hpp
 * @brief 事件管理器：统一事件路由 + 回调分发
 *
 * NetworkEventManager 是全局单例（通过 getEventManager() 获取），
 * 负责在各监控器产生事件时，将事件路由到：
 *   1. 客户端注册的 EventCallback（进程内回调）
 *   2. DbusService 对应的 D-Bus 信号（跨进程通知）
 *
 * 使用模式：
 *   监控线程 → EventManager::emitXxxChanged(message)
 *                → invokeCallbacks(EventType::Xxx)  [调用注册回调]
 *                → dbus_service->emitSpecificSignal(kSignalXxx)  [发射 D-Bus 信号]
 *
 * 线程安全：所有回调向量的注册/注销/遍历都在 cb_mutex_ 保护下进行。
 * 回调本身的执行时间应尽量短（避免阻塞监控线程），耗时操作应投递到独立线程。
 */

#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include "net_info.hpp"

namespace weaknet_dbus {

/**
 * @brief 事件类型枚举
 *
 * 每个值对应一种 D-Bus 信号名（见 common.hpp 的 kSignalXxx 常量），
 * 同时作为内部回调路由的键。
 */
enum class EventType {
    InterfaceChanged,         ///< 网卡添加/删除
    ConnectionModeChanged,   ///< 上网网卡切换
    NetworkQualityChanged,    ///< 综合网络质量等级变化
    TcpLossRateChanged,      ///< TCP 丢包率变化
    RttChanged,              ///< RTT 延迟变化
    RssiChanged,             ///< Wi-Fi RSSI 变化
    BluetoothDeviceChanged   ///< 蓝牙设备状态变化
};

/**
 * @brief 事件数据载体
 *
 * 携带事件的完整上下文，供回调处理函数和 D-Bus 信号发射使用。
 */
struct NetworkEvent {
    EventType type;                                    ///< 事件类型（路由键）
    std::string message;                               ///< 文本消息（D-Bus 信号主要载荷）
    std::chrono::system_clock::time_point timestamp;   ///< 事件产生时间戳（自动填充）
    std::string source;                                ///< 触发源（如网卡名 "wlan0"、服务名 "bt_monitor"）
    std::string details;                               ///< 详细信息（JSON 可选，传递更多结构化数据）
    int32_t priority = 0;                              ///< 优先级 0-10，越大越重要

    NetworkEvent() = default;

    /**
     * @brief 便捷构造：自动填充当前时间戳
     * @param t   事件类型
     * @param msg 文本消息
     * @param src 触发源标识（可选）
     * @param pri 优先级（默认 0）
     */
    NetworkEvent(EventType t, const std::string& msg,
                 const std::string& src = "", int32_t prio = 0)
        : type(t), message(msg), timestamp(std::chrono::system_clock::now()),
          source(src), priority(prio) {}
};

/// 事件回调类型：接收 NetworkEvent 常量引用（只读）
using EventCallback = std::function<void(const NetworkEvent&)>;

// 前向声明，避免循环依赖
struct ServerContext;

/**
 * @brief 全局事件管理器
 *
 * 单例设计，通过 getEventManager() 访问。
 * 内部按 EventType 维护 7 组回调向量，emitEvent 时遍历并依次调用。
 *
 * 线程安全：所有公开方法在 cb_mutex_ 保护下运行。
 * 设计注意：回调中若嵌套调用 emitEvent 会产生锁重入，当前实现在 emitEvent
 *   中先拷贝回调向量再解锁，避免回调中触发新事件时的死锁。
 */
class NetworkEventManager {
public:
    NetworkEventManager();
    ~NetworkEventManager() = default;

    /**
     * @brief 注册指定类型的事件回调
     *
     * 同一 EventType 可注册多个回调，emitEvent 时按注册顺序依次调用。
     *
     * @param type     事件类型
     * @param callback 回调函数（捕获需保证生命周期覆盖调用期）
     */
    void registerCallback(EventType type, EventCallback callback);

    /**
     * @brief 注销指定类型的所有回调
     * @param type 事件类型
     */
    void unregisterCallback(EventType type);

    /**
     * @brief 发射事件：路由到回调 + 发射 D-Bus 信号
     *
     * 执行顺序：
     *   1. invokeCallbacks() → 调用所有注册的回调
     *   2. 若已调用 startEventMonitoring()，通过 ServerContext::service 发射 D-Bus 信号
     *
     * @param event 事件载体（类型决定 D-Bus 信号名）
     */
    void emitEvent(const NetworkEvent& event);

    // ---------- 便捷 emit 方法（免去手动构造 NetworkEvent）----------

    void emitInterfaceChanged(const std::string& message, const std::string& source = "");
    void emitConnectionModeChanged(const std::string& message, const std::string& source = "");
    void emitNetworkQualityChanged(const std::string& message, const std::string& details = "", const std::string& source = "");
    void emitTcpLossRateChanged(const std::string& message, const std::string& source = "");
    void emitRttChanged(const std::string& message, const std::string& source = "");
    void emitRssiChanged(const std::string& message, const std::string& source = "");
    void emitBluetoothDeviceChanged(const std::string& message, const std::string& source = "");

    // ---------- 与 ServerContext/D-Bus 的集成 ----------

    /**
     * @brief 将 EventManager 绑定到 ServerContext
     *
     * 绑定后 emitXxxChanged() 会自动调用 ctx->service->emitSpecificSignal()。
     * 必须在 ServerContext::service 初始化完成后调用。
     *
     * @param ctx 全局上下文（非 owning，保存裸指针）
     */
    void startEventMonitoring(struct ServerContext* ctx);

    /**
     * @brief 解除与 ServerContext 的绑定
     *
     * 调用后 emitXxxChanged() 不再发射 D-Bus 信号（仅调用回调）。
     * 用于服务关闭或测试场景。
     */
    void stopEventMonitoring();

private:
    std::mutex cb_mutex_;   ///< 保护所有 callback 向量的并发访问

    // 7 组回调向量（每个 EventType 对应一组）
    std::vector<EventCallback> interface_callbacks_;
    std::vector<EventCallback> connection_mode_callbacks_;
    std::vector<EventCallback> network_quality_callbacks_;
    std::vector<EventCallback> tcp_loss_callbacks_;
    std::vector<EventCallback> rtt_callbacks_;
    std::vector<EventCallback> rssi_callbacks_;
    std::vector<EventCallback> bluetooth_callbacks_;

    struct ServerContext* server_ctx_ = nullptr;  ///< 集成层：emit 时需通过 service 发 D-Bus 信号
    bool monitoring_active_ = false;               ///< 是否已绑定 ServerContext

    /**
     * @brief 内部：遍历并调用指定类型的所有回调（先拷贝再解锁，防死锁）
     */
    void invokeCallbacks(EventType type, const NetworkEvent& event);

    /**
     * @brief 内部：将 EventType 映射到 D-Bus 信号名
     * @return common.hpp 中定义的 kSignalXxx 常量之一
     */
    std::string getSignalName(EventType type) const;
};

/**
 * @brief 获取全局事件管理器单例
 * @return 引用（生命周期到进程退出）
 */
NetworkEventManager& getEventManager();

} // namespace weaknet_dbus

/**
 * @file event_manager.cpp
 * @brief 网络事件管理器实现 —— 观察者模式的事件总线
 *
 * 本文件实现 NetworkEventManager 单例，负责将网卡变更、上网方式切换、
 * 网络质量波动、蓝牙设备变化等事件以「回调分发 + DBus 信号」双通道广播。
 *
 * 设计思路：
 *   - 每种 EventType 维护独立的回调 vector，避免互斥事件相互阻塞
 *   - emitEvent() 内部先同步调用回调，再通过 DbusService 发送 DBus 信号
 *   - 回调与 DBus 信号共享一个原子计数器 eventCounter，便于接收方去重/排序
 *
 * 线程安全：
 *   - 回调注册/注销与分发通过 cb_mutex_ 保护
 *   - emitEvent 中的 DbusService 发送由 send_mutex_（在 dbus_service.cpp 中）保护
 *   - ServerContext 指针仅在 startEventMonitoring 中赋值一次，之后只读
 */

#include "event_manager.hpp"
#include "server.hpp"
#include "dbus_service.hpp"
#include "common.hpp"
#include "logger.hpp"
#include <cstdio>
#include <chrono>
#include <thread>
#include <atomic>

using namespace std::chrono_literals;

namespace weaknet_dbus {

/** 全局事件管理器单例，通过 getEventManager() 访问 */
static std::unique_ptr<NetworkEventManager> g_event_manager = std::make_unique<NetworkEventManager>();

NetworkEventManager& getEventManager() {
    return *g_event_manager;
}

NetworkEventManager::NetworkEventManager()
    : server_ctx_(nullptr) {
    LOG_INFO(LogModule::EVENT_MGR, "NetworkEventManager initialized");
}

/**
 * @brief 为指定事件类型注册回调函数
 * @param type      事件类型（EventType 枚举）
 * @param callback  事件触发时要执行的回调，参数为 NetworkEvent
 * @note  同一事件类型可注册多个回调，分发时按注册顺序依次执行
 */
void NetworkEventManager::registerCallback(EventType type, EventCallback callback) {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    switch (type) {
        case EventType::InterfaceChanged:
            interface_callbacks_.push_back(callback);
            break;
        case EventType::ConnectionModeChanged:
            connection_mode_callbacks_.push_back(callback);
            break;
        case EventType::NetworkQualityChanged:
            network_quality_callbacks_.push_back(callback);
            break;
        case EventType::TcpLossRateChanged:
            tcp_loss_callbacks_.push_back(callback);
            break;
        case EventType::RttChanged:
            rtt_callbacks_.push_back(callback);
            break;
        case EventType::RssiChanged:
            rssi_callbacks_.push_back(callback);
            break;
        case EventType::BluetoothDeviceChanged:
            bluetooth_callbacks_.push_back(callback);
            break;
    }
    LOG_INFO(LogModule::EVENT_MGR, "registered callback for event type " << static_cast<int>(type));
}

/**
 * @brief 注销指定事件类型的全部回调
 * @param type 事件类型
 */
void NetworkEventManager::unregisterCallback(EventType type) {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    switch (type) {
        case EventType::InterfaceChanged:
            interface_callbacks_.clear();
            break;
        case EventType::ConnectionModeChanged:
            connection_mode_callbacks_.clear();
            break;
        case EventType::NetworkQualityChanged:
            network_quality_callbacks_.clear();
            break;
        case EventType::TcpLossRateChanged:
            tcp_loss_callbacks_.clear();
            break;
        case EventType::RttChanged:
            rtt_callbacks_.clear();
            break;
        case EventType::RssiChanged:
            rssi_callbacks_.clear();
            break;
        case EventType::BluetoothDeviceChanged:
            bluetooth_callbacks_.clear();
            break;
    }
    LOG_INFO(LogModule::EVENT_MGR, "unregistered all callbacks for event type " << static_cast<int>(type));
}

/**
 * @brief 将 EventType 映射为对应的 DBus 信号名
 * @param type 事件类型
 * @return DBus 信号名字符串，未知类型回退到通用 kSignalChanged
 */
std::string NetworkEventManager::getSignalName(EventType type) const {
    switch (type) {
        case EventType::InterfaceChanged:
            return kSignalInterfaceChanged;
        case EventType::ConnectionModeChanged:
            return kSignalConnectionModeChanged;
        case EventType::NetworkQualityChanged:
            return kSignalNetworkQualityChanged;
        case EventType::BluetoothDeviceChanged:
            return kSignalBluetoothDeviceChanged;
        default:
            return kSignalChanged; // 默认使用通用信号
    }
}

/**
 * @brief 分发事件：先调用所有注册回调，再通过 DBus 发送信号
 * @param event 完整的 NetworkEvent 对象
 *
 * 分发顺序：invokeCallbacks() → emitNetworkQualitySignal()/emitSpecificSignal()
 * 其中 DBus 信号发送自带互斥锁 + 重试逻辑，不会阻塞回调分发
 */
void NetworkEventManager::emitEvent(const NetworkEvent& event) {
    LOG_INFO(LogModule::EVENT_MGR, "emitting event: type=" << static_cast<int>(event.type) << ", message='" << event.message << "', source='" << event.source << "'");

    invokeCallbacks(event.type, event);

    // 如果有DBus服务，发送信号
    if (server_ctx_ && server_ctx_->service) {
        std::string signalName = getSignalName(event.type);
        // 将 source 前缀格式化进 message，便于接收方日志追踪
        std::string fullMessage = event.source.empty()
            ? event.message
            : "[" + event.source + "] " + event.message;

        // 原子计数器：跨事件类型递增，接收方可用来判断事件先后顺序与去重
        static std::atomic<int32_t> eventCounter{0};

        // 网络质量事件携带 details 字段，走专用三参数信号；其余走通用两参数信号
        if (event.type == EventType::NetworkQualityChanged && !event.details.empty()) {
            server_ctx_->service->emitNetworkQualitySignal(fullMessage, event.details, eventCounter++);
        } else {
            server_ctx_->service->emitSpecificSignal(signalName, fullMessage, eventCounter++);
        }
    }
}

/**
 * @brief 便捷封装：发出网卡接口变更事件
 * @param message 事件描述
 * @param source  事件来源模块标识（可空）
 */
void NetworkEventManager::emitInterfaceChanged(const std::string& message, const std::string& source) {
    emitEvent(NetworkEvent(EventType::InterfaceChanged, message, source, 8));
}

/**
 * @brief 便捷封装：发出上网方式变更事件
 */
void NetworkEventManager::emitConnectionModeChanged(const std::string& message, const std::string& source) {
    emitEvent(NetworkEvent(EventType::ConnectionModeChanged, message, source, 9));
}

/**
 * @brief 便捷封装：发出网络质量变更事件（携带 details）
 * @param message 事件描述
 * @param details 详细的质量评估信息
 * @param source  事件来源
 */
void NetworkEventManager::emitNetworkQualityChanged(const std::string& message, const std::string& details, const std::string& source) {
    NetworkEvent event(EventType::NetworkQualityChanged, message, source, 7);
    event.details = details;
    emitEvent(event);
}

/**
 * @brief 便捷封装：发出 TCP 丢包率变更事件
 */
void NetworkEventManager::emitTcpLossRateChanged(const std::string& message, const std::string& source) {
    emitEvent(NetworkEvent(EventType::TcpLossRateChanged, message, source, 6));
}

/**
 * @brief 便捷封装：发出 RTT 变更事件
 */
void NetworkEventManager::emitRttChanged(const std::string& message, const std::string& source) {
    emitEvent(NetworkEvent(EventType::RttChanged, message, source, 5));
}

/**
 * @brief 便捷封装：发出 RSSI 变更事件
 */
void NetworkEventManager::emitRssiChanged(const std::string& message, const std::string& source) {
    emitEvent(NetworkEvent(EventType::RssiChanged, message, source, 4));
}

/**
 * @brief 便捷封装：发出蓝牙设备变更事件
 */
void NetworkEventManager::emitBluetoothDeviceChanged(const std::string& message, const std::string& source) {
    emitEvent(NetworkEvent(EventType::BluetoothDeviceChanged, message, source, 5));
}

/**
 * @brief 启动事件监控，绑定 ServerContext 并注册日志回调
 * @param ctx 服务器上下文（包含 DbusService 指针）
 * @note  必须在 DbusService 初始化完成后调用
 */
void NetworkEventManager::startEventMonitoring(struct ServerContext* ctx) {
    server_ctx_ = ctx;

    LOG_INFO(LogModule::EVENT_MGR, "event monitoring started");

    // 注册默认日志回调，保证事件至少被记录到日志，便于问题排查
    registerCallback(EventType::InterfaceChanged, [](const NetworkEvent& event) {
        LOG_INFO(LogModule::EVENT_MGR, "Interface change event: " << event.message);
    });

    registerCallback(EventType::ConnectionModeChanged, [](const NetworkEvent& event) {
        LOG_INFO(LogModule::EVENT_MGR, "Connection mode change event: " << event.message);
    });

    registerCallback(EventType::BluetoothDeviceChanged, [](const NetworkEvent& event) {
        LOG_INFO(LogModule::EVENT_MGR, "Bluetooth device event: " << event.message);
    });
}

/**
 * @brief 内部方法：按事件类型分发回调
 * @param type  事件类型，决定遍历哪个回调 vector
 * @param event 传递给每个回调的事件对象
 * @note  先在 cb_mutex_ 保护下把该类型的回调**拷贝一份**，释放锁后再逐个调用。
 *       回调执行期间不持锁，因此回调内可以安全地 registerCallback /
 *       unregisterCallback，也可以嵌套触发新事件，不会重入死锁。
 *       （此前实现是持锁遍历调用，与头文件描述相反，回调内再注册即死锁。）
 */
void NetworkEventManager::invokeCallbacks(EventType type, const NetworkEvent& event) {
    std::vector<EventCallback> snapshot;
    {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        const std::vector<EventCallback>* src = nullptr;
        switch (type) {
            case EventType::InterfaceChanged:        src = &interface_callbacks_; break;
            case EventType::ConnectionModeChanged:   src = &connection_mode_callbacks_; break;
            case EventType::NetworkQualityChanged:   src = &network_quality_callbacks_; break;
            case EventType::TcpLossRateChanged:      src = &tcp_loss_callbacks_; break;
            case EventType::RttChanged:              src = &rtt_callbacks_; break;
            case EventType::RssiChanged:             src = &rssi_callbacks_; break;
            case EventType::BluetoothDeviceChanged:  src = &bluetooth_callbacks_; break;
        }
        if (!src) return;
        snapshot = *src;
    }

    // 锁外调用：拷贝保持了注册顺序，且本轮的增删不会影响已开始的这一轮分发。
    for (const auto& callback : snapshot) {
        if (callback) callback(event);
    }
}

} // namespace weaknet_dbus

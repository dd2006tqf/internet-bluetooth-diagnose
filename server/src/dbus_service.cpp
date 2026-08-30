/**
 * @file dbus_service.cpp
 * @brief DBus 服务层实现：方法分发、信号发送、载荷持久化
 *
 * 本文件实现 DbusService 类，作为 WeakNet 服务与外部世界交互的唯一入口。
 * 上层通过它暴露约 12 个 DBus 方法（Get、ListInterfaces、HealthCheck、Ping、
 * GetBluetoothDevices、GetEbpfMonitorHealth、GetHistory 等），同时向外发送
 * Changed / NetworkQualityChanged 等 DBus 信号。
 *
 * 设计思路：
 *   - MessageHandler 采用「C 回调 → 静态函数 → 对象方法」三段式，规避 libdbus
 *     纯 C 回调签名不能直接绑定 this 指针的问题
 *   - 每个 handleXxx 方法遵循统一模板：日志 → 解析参数 → 获取依赖 → 构造回复 → 发送
 *   - 信号发送统一走 sendSignalInternal，自带 3 次重试（每次间隔 100ms），
 *     由 send_mutex_ 串行化，保证 DBus 连接上发送顺序与调用顺序一致
 *   - ChangedPayload 在成功发信号后同步持久化到文件，供重启或崩溃后恢复
 *   - 所有结构化返回值（蓝牙、DNS、Wi-Fi、HTTP 等）采用 "key:value|key:value"
 *     扁平编码，避免引入 JSON 库依赖
 *
 * 线程安全：
 *   - 所有 handleXxx 在 DBus 主循环线程中串行执行（libdbus 单线程 dispatching）
 *   - 信号发送由 send_mutex_ 保护，可被多线程并发调用
 *   - ServerContext 中各 monitor 指针仅在启动阶段赋值，之后只读；业务方法调用
 *     时通过 monitor->isAvailable() 或 ctx_ 空指针检查兜底
 */

#include <dbus/dbus.h>
#include <cstdio>
#include <cstring>
#include "logger.hpp"

#include "common.hpp"
#include "serializer.hpp"
#include "server.hpp"
#include "dbus_service.hpp"
#include "weak_netmgr.hpp"
#include "net_info.hpp"
#include "network_quality_assessor.hpp"
#include "net_ping.h"
#include "bt_monitor.hpp"
#include "bt_audio_analyzer.hpp"
#include "dns_monitor.hpp"
#include "wifi_packet_loss_monitor.hpp"
#include "http_latency_monitor.hpp"
#include "process_net_profiler.hpp"
#include "tcp_retransmit_monitor.hpp"
#include "utils/json_escape.hpp"
#include "database_manager.hpp"
#include <sstream>

namespace weaknet_dbus {

DbusService::DbusService(ServerContext* ctx) : ctx_(ctx) {}

/**
 * @brief libdbus C 回调入口 → 转调到具体成员方法
 * @param conn     DBus 连接
 * @param msg      收到的消息（方法调用）
 * @param user_data 注册时传入的 this 指针
 * @return DBUS_HANDLER_RESULT_HANDLED 表示已处理；NOT_YET_HANDLED 表示交给下一个处理器
 *
 * 每个 if 分支对应一个公开的 DBus 方法；匹配到接口名 + 方法名后直接调用
 * 对应的 handleXxx 成员。未匹配的方法返回 NOT_YET_HANDLED，让 libdbus 继续寻找
 * 其他注册的处理器（如果有的话）。
 */
static DBusHandlerResult MessageHandlerStatic(DBusConnection* conn, DBusMessage* msg, void* user_data) {
    auto* self = reinterpret_cast<DbusService*>(user_data);
    if (!self) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    if (dbus_message_is_method_call(msg, kInterface, kMethodGet)) {
        self->handleGet(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodListInterfaces)) {
        self->handleListInterfaces(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    // GetInterfaces 是 ListInterfaces 的别名，行为完全一致
    if (dbus_message_is_method_call(msg, kInterface, kMethodGetInterfaces)) {
        self->handleListInterfaces(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodHealthCheck)) {
        self->handleHealthCheck(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodPing)) {
        self->handlePing(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodGetBluetoothDevices)) {
        self->handleGetBluetoothDevices(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodGetBluetoothAdapter)) {
        self->handleGetBluetoothAdapter(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodGetDnsStats)) {
        self->handleGetDnsStats(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodGetWifiLossStats)) {
        self->handleGetWifiLossStats(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodGetHttpLatencyStats)) {
        self->handleGetHttpLatencyStats(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodGetProcessProfiling)) {
        self->handleGetProcessProfiling(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodGetEbpfMonitorHealth)) {
        self->handleGetEbpfMonitorHealth(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(msg, kInterface, kMethodGetHistory)) {
        self->handleGetHistory(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/**
 * @brief 在 DBus 连接上注册对象路径 vtable
 * @param conn DBus 连接
 * @return true 注册成功；false 连接无效或已存在 vtable
 *
 * libdbus 采用 vtable 模式：一个对象路径对应一个 vtable，其中 message_function
 * 就是上面的静态转发函数。这样每条方法调用都会被路由到 MessageHandlerStatic。
 */
bool DbusService::register_on_connection(DBusConnection* conn) {
    static DBusObjectPathVTable vtable{};
    vtable.message_function = &MessageHandlerStatic;
    return dbus_connection_register_object_path(conn, kObjectPath, &vtable, this);
}

/**
 * @brief 内部统一信号发送函数（带重试）
 * @param signalName DBus 信号名（如 kSignalChanged、kSignalNetworkQualityChanged）
 * @param args       信号参数列表，每项为 {DBUS_TYPE_*, 值指针}
 * @param counter    事件计数器，便于接收方去重/排序
 * @return true 发送成功；false 三次重试全部失败
 *
 * 重试策略：最多 3 次，失败间隔 100ms。每次迭代：创建 signal → 追加参数 → send → flush。
 * 若 create signal 或 append 参数失败直接返回，不重试（属于编程错误而非网络抖动）。
 */
bool DbusService::sendSignalInternal(const std::string& signalName,
                                    const std::vector<std::pair<int, const void*>>& args,
                                    int32_t counter) {
    if (!ctx_ || !ctx_->connection) return false;

    const int max_retries = 3;
    const int retry_delay_ms = 100;

    for (int attempt = 0; attempt < max_retries; ++attempt) {
        DBusMessage* signal = dbus_message_new_signal(kObjectPath, kInterface, signalName.c_str());
        if (!signal) {
            LOG_ERROR(LogModule::DBUS, "sendSignalInternal: failed to create signal " << signalName);
            return false;
        }

        DBusMessageIter iter;
        dbus_message_iter_init_append(signal, &iter);

        // 按顺序逐个追加参数；args 中的指针在调用栈上，生命周期覆盖整个循环迭代
        for (const auto& [type, value] : args) {
            if (!dbus_message_iter_append_basic(&iter, type, value)) {
                LOG_ERROR(LogModule::DBUS, "sendSignalInternal: failed to append argument for " << signalName);
                dbus_message_unref(signal);
                return false;
            }
        }

        bool ok = dbus_connection_send(ctx_->connection, signal, nullptr);
        dbus_connection_flush(ctx_->connection);
        dbus_message_unref(signal);

        if (ok) {
            LOG_INFO(LogModule::DBUS, "sendSignalInternal: emitted " << signalName << " counter=" << counter);
            return true;
        }

        LOG_WARNING(LogModule::DBUS, "sendSignalInternal(" << signalName << "): attempt " << attempt + 1
                    << " failed, retrying in " << retry_delay_ms << "ms");

        if (attempt < max_retries - 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms));
        }
    }

    LOG_ERROR(LogModule::DBUS, "sendSignalInternal(" << signalName << "): all " << max_retries << " attempts failed");
    return false;
}

/**
 * @brief 发送通用 Changed 信号（带持久化）
 * @param message 信号载荷字符串
 * @param counter 事件计数器
 * @return true 发送成功
 *
 * 与 emitSpecificSignal 的区别：成功发送后额外将载荷序列化到文件，供重启恢复。
 * 内部持 send_mutex_，保证与其他信号发送的串行顺序。
 */
bool DbusService::emitChanged(const std::string& message, int32_t counter) {
    std::lock_guard<std::mutex> lock(send_mutex_);

    // 构造参数列表
    const char* s = message.c_str();
    std::vector<std::pair<int, const void*>> args = {
        {DBUS_TYPE_STRING, &s},
        {DBUS_TYPE_INT32, &counter}
    };

    bool ok = sendSignalInternal(kSignalChanged, args, counter);

    // 仅在信号发送成功时持久化，避免文件与 DBus 状态不一致
    if (ok) {
        ChangedPayload payload{message, counter};
        std::string err;
        serializeChangedPayloadToFile(payload, kSignalSerializedFile, &err);
    }

    return ok;
}

// MessageHandler 实现已移动到静态自由函数（见文件顶部 MessageHandlerStatic）

/**
 * @brief DBus 方法实现：Get —— 健康检查接口，返回固定字符串
 * @param conn DBus 连接
 * @param msg 接收到的方法调用消息
 * @return true 回复发送成功
 *
 * 同时将回复内容序列化到 kGetReplySerializedFile，便于非 DBus 客户端读取。
 */
bool DbusService::handleGet(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleGet called");
    const char* reply_text = "Hello from WeakNet Server";
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) {
        LOG_ERROR(LogModule::DBUS, "handleGet: failed to create reply");
        return false;
    }
    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);
    const char* s = reply_text;
    if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &s)) {
        LOG_ERROR(LogModule::DBUS, "handleGet: failed to append message");
        dbus_message_unref(reply);
        // 参数追加失败时，主动发送一个错误回复，让客户端收到明确的失败原因
        DBusMessage* error_reply = dbus_message_new_error(msg, "com.example.WeakNet.Error", "Failed to append message");
        if (error_reply) {
            dbus_connection_send(conn, error_reply, nullptr);
            dbus_connection_flush(conn);
            dbus_message_unref(error_reply);
        }
        return false;
    }
    if (!dbus_connection_send(conn, reply, nullptr)) {
        LOG_ERROR(LogModule::DBUS, "handleGet: failed to send reply");
        dbus_message_unref(reply);
        return false;
    }
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    std::string err;
    serializeGetReplyToFile(reply_text, kGetReplySerializedFile, &err);
    return true;
}

/**
 * @brief 内部辅助：向调用方返回 string 数组
 * @param conn DBus 连接
 * @param msg 接收到的方法调用消息
 * @param arr 待返回的字符串列表
 * @return true 回复发送成功
 *
 * libdbus 中发送数组需要三步：open_container → append_basic × N → close_container。
 * 本方法将此模板封装，所有返回字符串列表的 handleXxx（如 ListInterfaces、
 * GetBluetoothDevices）都复用它。
 */
bool DbusService::replyStringArray(DBusConnection* conn, DBusMessage* msg, const std::vector<std::string>& arr) {
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) {
        LOG_ERROR(LogModule::DBUS, "replyStringArray: failed to create reply");
        return false;
    }
    DBusMessageIter iter;
    dbus_message_iter_init_append(reply, &iter);
    DBusMessageIter array_iter;
    // DBUS_TYPE_STRING_AS_STRING = "s"，用于告诉 dbus_message_iter_open_container 数组元素类型
    if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING_AS_STRING, &array_iter)) {
        LOG_ERROR(LogModule::DBUS, "replyStringArray: failed to open array container");
        dbus_message_unref(reply);
        return false;
    }
    for (const auto& s : arr) {
        const char* cs = s.c_str();
        if (!dbus_message_iter_append_basic(&array_iter, DBUS_TYPE_STRING, &cs)) {
            LOG_ERROR(LogModule::DBUS, "replyStringArray: failed to append string");
            dbus_message_iter_close_container(&iter, &array_iter);
            dbus_message_unref(reply);
            return false;
        }
    }
    if (!dbus_message_iter_close_container(&iter, &array_iter)) {
        LOG_ERROR(LogModule::DBUS, "replyStringArray: failed to close array container");
        dbus_message_unref(reply);
        return false;
    }
    bool ok = dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return ok;
}

/**
 * @brief DBus 方法实现：ListInterfaces —— 返回当前所有网卡接口名
 * @param conn DBus 连接
 * @param msg 接收到的方法调用消息
 * @return true 回复发送成功
 *
 * 数据源来自 WeakNetMgr::current_interfaces_（线程安全 getCurrentInterfaces()），
 * 返回值是接口名的字符串数组。
 */
bool DbusService::handleListInterfaces(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleListInterfaces called");
    // 接口列表唯一事实源 = WeakNetMgr::current_interfaces_（线程安全接口）
    std::vector<NetInfo> snapshot = ctx_->weak_mgr->getCurrentInterfaces();
    return replyStringArray(conn, msg, WeakNetMgr::namesOf(snapshot));
}

/**
 * @brief DBus 方法实现：HealthCheck —— 返回当前网络质量评估
 * @param conn DBus 连接
 * @param msg 接收到的方法调用消息
 * @return true 回复发送成功
 *
 * 构造临时 NetworkQualityAssessor，对当前网卡快照做一次性评估，结果以字符串返回。
 * 不缓存 assessor，保证每次调用都是最新快照。
 */
bool DbusService::handleHealthCheck(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleHealthCheck called");
    // 接口列表唯一事实源 = WeakNetMgr::current_interfaces_（线程安全接口）
    std::vector<NetInfo> snapshot = ctx_->weak_mgr->getCurrentInterfaces();

    NetworkQualityAssessor assessor;
    NetworkQualityResult result = assessor.assessQuality(snapshot);
    std::string reply_text = result.details;

    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;
    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);
    const char* s = reply_text.c_str();
    if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &s)) { dbus_message_unref(reply); return false; }
    if (!dbus_connection_send(conn, reply, nullptr)) { dbus_message_unref(reply); return false; }
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return true;
}

/**
 * @brief 发送带计数器的指定 DBus 信号（通用两参数版）
 * @param signalName 具体信号名（如 kSignalInterfaceChanged）
 * @param message    信号消息体
 * @param counter    事件计数器
 * @return true 发送成功
 *
 * 与 emitChanged 的区别：不持久化；与 emitNetworkQualitySignal 的区别：少一个 details 参数。
 * 内部持 send_mutex_，保证发送顺序。
 */
bool DbusService::emitSpecificSignal(const std::string& signalName, const std::string& message, int32_t counter) {
    std::lock_guard<std::mutex> lock(send_mutex_);

    // 构造参数列表
    const char* msg = message.c_str();
    std::vector<std::pair<int, const void*>> args = {
        {DBUS_TYPE_STRING, &msg},
        {DBUS_TYPE_INT32, &counter}
    };

    return sendSignalInternal(signalName, args, counter);
}

/**
 * @brief 发送网络质量变更信号（三参数版：message + details + counter）
 * @param message 简短质量描述
 * @param details 详细质量评估（JSON 或长文本）
 * @param counter 事件计数器
 * @return true 发送成功
 */
bool DbusService::emitNetworkQualitySignal(const std::string& message, const std::string& details, int32_t counter) {
    std::lock_guard<std::mutex> lock(send_mutex_);

    // 构造参数列表
    const char* quality = message.c_str();
    const char* details_str = details.c_str();
    std::vector<std::pair<int, const void*>> args = {
        {DBUS_TYPE_STRING, &quality},
        {DBUS_TYPE_STRING, &details_str},
        {DBUS_TYPE_INT32, &counter}
    };

    return sendSignalInternal(kSignalNetworkQualityChanged, args, counter);
}

/**
 * @brief DBus 方法实现：Ping —— 对指定主机执行 ICMP 探测
 * @param conn DBus 连接
 * @param msg 接收到的方法调用消息（参数：hostname 字符串）
 * @return true 回复发送成功
 *
 * 步骤：解析 hostname → 找到当前上网网卡 → 调用 NetPing::ping()（3 秒超时）→ 回复结果字符串。
 * 任何环节失败都返回 DBus error 消息而非空回复。
 */
bool DbusService::handlePing(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handlePing called");

    // 解析参数：目标主机名
    DBusError err;
    dbus_error_init(&err);
    const char* hostname = nullptr;

    if (!dbus_message_get_args(msg, &err, DBUS_TYPE_STRING, &hostname, DBUS_TYPE_INVALID)) {
        LOG_ERROR(LogModule::DBUS, "Ping method error: " << err.message);
        dbus_error_free(&err);

        // 发送错误回复
        DBusMessage* reply = dbus_message_new_error(msg, "com.example.WeakNet.Error", "Invalid arguments");
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return false;
    }

    if (!hostname || strlen(hostname) == 0) {
        LOG_ERROR(LogModule::DBUS, "Ping method error: empty hostname");

        // 发送错误回复
        DBusMessage* reply = dbus_message_new_error(msg, "com.example.WeakNet.Error", "Empty hostname");
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return false;
    }

    LOG_INFO(LogModule::DBUS, "Ping request for host: " << hostname);

    // 获取当前上网网卡（接口列表唯一事实源 = WeakNetMgr::current_interfaces_）
    std::string currentIface;
    {
        auto interfaces = ctx_->weak_mgr->getCurrentInterfaces();
        for (const auto& net : interfaces) {
            if (net.usingNow()) {
                currentIface = net.ifName();
                break;
            }
        }
    }

    if (currentIface.empty()) {
        LOG_ERROR(LogModule::DBUS, "Ping method error: no active interface found");

        // 发送错误回复
        DBusMessage* reply = dbus_message_new_error(msg, "com.example.WeakNet.Error", "No active network interface");
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
        return false;
    }

    LOG_INFO(LogModule::DBUS, "Using interface: " << currentIface << " for ping to " << hostname);

    // 调用NetPing进行ping测试
    auto pingInstance = NetPing::getInstance();
    int pingResult = pingInstance->ping(hostname, currentIface, 3000); // 3秒超时

    // 构建回复消息
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) {
        LOG_ERROR(LogModule::DBUS, "Failed to create ping reply message");
        return false;
    }

    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);

    // 根据 ping 返回值（≥0 为 RTT ms，<0 为错误码）构造人类可读字符串
    std::string result;
    if (pingResult >= 0) {
        result = std::string("PING ") + hostname + " via " + currentIface + ": " + std::to_string(pingResult) + "ms";
        LOG_INFO(LogModule::DBUS, "Ping successful: " << result);
    } else {
        result = std::string("PING ") + hostname + " via " + currentIface + ": FAILED (error code: " + std::to_string(pingResult) + ")";
        LOG_INFO(LogModule::DBUS, "Ping failed: " << result);
    }

    const char* resultStr = result.c_str();
    if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &resultStr)) {
        LOG_ERROR(LogModule::DBUS, "Failed to append ping result to reply");
        dbus_message_unref(reply);
        return false;
    }

    // 发送回复
    bool ok = dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);

    LOG_INFO(LogModule::DBUS, "Ping reply sent: " << (ok ? "success" : "failed"));
    return ok;
}

// ============================================================================
// 蓝牙设备相关方法
// ============================================================================

/**
 * @brief DBus 方法实现：GetBluetoothDevices —— 返回蓝牙设备列表
 * @param conn DBus 连接
 * @param msg 接收到的方法调用消息
 * @return true 回复发送成功
 *
 * 每个设备编码为一条 "MAC|Name|RSSI|Connected|Type|Level" 字符串，
 * 所有设备组成 string 数组返回。BtMonitor 未初始化时返回空数组。
 */
bool DbusService::handleGetBluetoothDevices(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleGetBluetoothDevices called");

    BtMonitor* monitor = ctx_ ? ctx_->bt_monitor.get() : nullptr;
    if (!monitor) {
        // 无蓝牙监测器 → 返回空数组。必须构造合法的空 DBus 数组容器，不能跳过 open_container
        DBusMessage* reply = dbus_message_new_method_return(msg);
        if (reply) {
            DBusMessageIter iter;
            dbus_message_iter_init_append(reply, &iter);
            DBusMessageIter arr;
            dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING_AS_STRING, &arr);
            dbus_message_iter_close_container(&iter, &arr);
            dbus_connection_send(conn, reply, nullptr);
            dbus_connection_flush(conn);
            dbus_message_unref(reply);
        }
        return true;
    }

    // 获取设备列表，格式化为 "MAC|Name|RSSI|Connected|Type|Level" 字符串
    auto devices = monitor->getDevices();
    std::vector<std::string> lines;
    lines.reserve(devices.size());
    for (const auto& dev : devices) {
        // 字段顺序固定：MAC → 显示名（优先 alias，无则 name）→ RSSI dBm → 连接标记 → 类型 → RSSI 等级
        std::string line = dev.macAddress + "|"
            + (dev.name.empty() ? dev.alias : dev.name) + "|"
            + std::to_string(dev.rssiDbm) + "|"
            + (dev.connected ? "1" : "0") + "|"
            + (dev.deviceType == BtDeviceType::BLE ? "BLE" :
               dev.deviceType == BtDeviceType::Classic ? "Classic" : "Dual") + "|"
            + dev.rssiLevel();
        lines.push_back(line);
    }
    return replyStringArray(conn, msg, lines);
}

/**
 * @brief DBus 方法实现：GetBluetoothAdapter —— 返回本机蓝牙适配器状态
 * @param conn DBus 连接
 * @param msg 接收到的方法调用消息
 * @return true 回复发送成功
 *
 * 编码格式："Powered:0/1|Name:xxx|Address:XX:XX:...|Discovering:0/1|Discoverable:0/1|Pairable:0/1"
 * BtMonitor 未初始化时返回 "No Bluetooth adapter available"。
 */
bool DbusService::handleGetBluetoothAdapter(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleGetBluetoothAdapter called");

    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;

    std::string result;
    BtMonitor* monitor = ctx_ ? ctx_->bt_monitor.get() : nullptr;
    if (monitor && monitor->isInitialized()) {
        auto state = monitor->getAdapterState();
        result = std::string("Powered:") + (state.powered ? "1" : "0")
            + "|Name:" + state.name
            + "|Address:" + state.macAddress
            + "|Discovering:" + (state.discovering ? "1" : "0")
            + "|Discoverable:" + (state.discoverable ? "1" : "0")
            + "|Pairable:" + (state.pairable ? "1" : "0");
    } else {
        result = "No Bluetooth adapter available";
    }

    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);
    const char* s = result.c_str();
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &s);
    dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return true;
}

// ====================================================================
// eBPF 监控数据 D-Bus 方法
// ====================================================================
// 以下 5 个方法结构高度相似：获取对应 monitor → 检查 isAvailable() →
// 用 "key:value|key:value" 格式拼接 → 返回单字符串。
// 区别仅在数据源和字段集合。

/**
 * @brief DBus 方法实现：GetDnsStats —— 返回 DNS 监控统计
 * 字段: totalQueries | totalResponses | totalTimeouts | totalErrors | avgLatencyMs | maxLatencyMs | timeoutRate
 */
bool DbusService::handleGetDnsStats(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleGetDnsStats called");
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;

    std::string result;
    DnsMonitor* monitor = ctx_ ? ctx_->dns_monitor.get() : nullptr;
    if (monitor && monitor->isAvailable()) {
        auto stats = monitor->getStats();
        result = "totalQueries:" + std::to_string(stats.totalQueries)
            + "|totalResponses:" + std::to_string(stats.totalResponses)
            + "|totalTimeouts:" + std::to_string(stats.totalTimeouts)
            + "|totalErrors:" + std::to_string(stats.totalErrors)
            + "|avgLatencyMs:" + std::to_string(stats.avgLatencyMs)
            + "|maxLatencyMs:" + std::to_string(stats.maxLatencyMs)
            + "|timeoutRate:" + std::to_string(stats.timeoutRate());
    } else {
        result = "DNS monitor not available";
    }

    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);
    const char* s = result.c_str();
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &s);
    dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return true;
}

/**
 * @brief DBus 方法实现：GetWifiLossStats —— 返回 Wi-Fi 丢包监控统计
 *
 * 多网卡场景：按 ifindex 分隔每段 "ifindex:N|rxPkts:...|txPkts:...|txDrops:...|txLossRate:...%|"
 */
bool DbusService::handleGetWifiLossStats(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleGetWifiLossStats called");
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;

    std::string result;
    WifiPacketLossMonitor* monitor = ctx_ ? ctx_->wifi_loss_monitor.get() : nullptr;
    if (monitor && monitor->isAvailable()) {
        auto stats = monitor->getStats();
        for (auto& [ifindex, s] : stats) {
            result += "ifindex:" + std::to_string(ifindex)
                + " rxPkts:" + std::to_string(s.rxPkts)
                + " txPkts:" + std::to_string(s.txPkts)
                + " txDrops:" + std::to_string(s.txDrops)
                + " txLossRate:" + std::to_string(s.txLossRate()) + "%"
                + "|";
        }
        if (result.empty()) result = "No interface stats available";
    } else {
        result = "Wi-Fi loss monitor not available";
    }

    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);
    const char* s = result.c_str();
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &s);
    dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return true;
}

/**
 * @brief DBus 方法实现：GetHttpLatencyStats —— 返回 HTTP 延迟监控统计
 * 字段: totalTxns | p50Ms | p95Ms | p99Ms | maxMs | analysis
 */
bool DbusService::handleGetHttpLatencyStats(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleGetHttpLatencyStats called");
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;

    std::string result;
    HttpLatencyMonitor* monitor = ctx_ ? ctx_->http_latency_monitor.get() : nullptr;
    if (monitor && monitor->isAvailable()) {
        auto stats = monitor->getGlobalStats();
        result = "totalTxns:" + std::to_string(stats.totalTxns)
            + "|p50Ms:" + std::to_string(stats.p50Ns / 1000000)
            + "|p95Ms:" + std::to_string(stats.p95Ns / 1000000)
            + "|p99Ms:" + std::to_string(stats.p99Ns / 1000000)
            + "|maxMs:" + std::to_string(stats.maxNs / 1000000)
            + "|analysis:" + stats.analysis;
    } else {
        result = "HTTP latency monitor not available";
    }

    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);
    const char* s = result.c_str();
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &s);
    dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return true;
}

/**
 * @brief DBus 方法实现：GetProcessProfiling —— 返回进程级网络 profiling 数据
 *
 * 输出分两段：Top Bandwidth（按 txBytes 排序 top 5）和 Top Retransmit（按重传次数排序 top 5）。
 * 每条格式: "pid:N comm:xxx txBytes:... txPackets:... retrans:...|"
 */
bool DbusService::handleGetProcessProfiling(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleGetProcessProfiling called");
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;

    std::string result;
    ProcessNetProfiler* monitor = ctx_ ? ctx_->process_net_profiler.get() : nullptr;
    if (monitor && monitor->isAvailable()) {
        result += "=== Top Bandwidth ===|";
        auto topBw = monitor->getTopBandwidth(5);
        for (auto& p : topBw) {
            result += "pid:" + std::to_string(p.pid)
                + " comm:" + p.comm
                + " txBytes:" + std::to_string(p.txBytes)
                + " txPackets:" + std::to_string(p.txPackets)
                + " retrans:" + std::to_string(p.retransCount)
                + "|";
        }
        result += "=== Top Retransmit ===|";
        auto topRetrans = monitor->getTopRetransmit(5);
        for (auto& p : topRetrans) {
            result += "pid:" + std::to_string(p.pid)
                + " comm:" + p.comm
                + " txBytes:" + std::to_string(p.txBytes)
                + " retrans:" + std::to_string(p.retransCount)
                + "|";
        }
    } else {
        result = "Process net profiler not available";
    }

    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);
    const char* s = result.c_str();
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &s);
    dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return true;
}

/**
 * @brief DBus 方法实现：GetEbpfMonitorHealth —— 返回所有 eBPF monitor 健康状态（JSON）
 * @return JSON 字符串，结构: {"monitors":[{name,state,available,healthy,map_reads,map_read_errors,samples,average_read_time_us,status}, ...]}
 *
 * 遍历 6 个实现了 IEbpfMonitor 接口的组件（DNS/Wi-Fi 丢包/HTTP 延迟/进程 profiling/TCP 重传/蓝牙音频），
 * 每个采集 health() 和 metrics()。蓝牙音频 monitor 有条件时走 bt_monitor->audioAnalyzer()，否则用
 * 栈上的 fallbackAudioAnalyzer 占位，保证 JSON 始终包含 6 项而不出现 key 缺失。
 */
bool DbusService::handleGetEbpfMonitorHealth(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleGetEbpfMonitorHealth called");
    // 预先检查所有必要组件，任何一个缺失都直接返回 DBus error，避免下游空指针崩溃
    if (!ctx_ || !ctx_->dns_monitor || !ctx_->wifi_loss_monitor ||
        !ctx_->http_latency_monitor || !ctx_->process_net_profiler ||
        !ctx_->tcp_retrans_monitor || !ctx_->bt_monitor) {
        DBusMessage* error = dbus_message_new_error(msg, "com.example.WeakNet.Error", "eBPF monitor context unavailable");
        if (error) {
            dbus_connection_send(conn, error, nullptr);
            dbus_connection_flush(conn);
            dbus_message_unref(error);
        }
        return false;
    }

    // 蓝牙音频分析器可选：有就用真实实例，没有就用栈上 fallback（状态=unavailable）
    BtAudioAnalyzer fallbackAudioAnalyzer;
    const IEbpfMonitor* audioMonitor = ctx_->bt_monitor->audioAnalyzer();
    if (!audioMonitor) audioMonitor = &fallbackAudioAnalyzer;

    // 统一用 IEbpfMonitor 接口指针收集，后续循环按 interface 方法处理，避免每个 monitor 单独写一遍
    const std::vector<const IEbpfMonitor*> monitors = {
        static_cast<const IEbpfMonitor*>(ctx_->dns_monitor.get()),
        static_cast<const IEbpfMonitor*>(ctx_->wifi_loss_monitor.get()),
        static_cast<const IEbpfMonitor*>(ctx_->http_latency_monitor.get()),
        static_cast<const IEbpfMonitor*>(ctx_->process_net_profiler.get()),
        static_cast<const IEbpfMonitor*>(ctx_->tcp_retrans_monitor.get()),
        audioMonitor
    };

    // 手工拼接 JSON：项目不依赖 JSON 库，字段名和字符串值都要做 JSON 转义
    std::ostringstream json;
    json << "{\"monitors\":[";
    for (size_t i = 0; i < monitors.size(); ++i) {
        if (i > 0) json << ",";
        const auto health = monitors[i]->health();
        const auto metrics = monitors[i]->metrics();
        json << "{\"name\":\"" << health.name
             << "\",\"state\":\"" << ebpfMonitorStateName(health.state)
             << "\",\"available\":" << (health.available ? "true" : "false")
             << ",\"healthy\":" << (health.healthy ? "true" : "false")
             << ",\"map_reads\":" << metrics.mapReads
             << ",\"map_read_errors\":" << metrics.mapReadErrors
             << ",\"samples\":" << metrics.samples
             << ",\"average_read_time_us\":" << metrics.averageReadTimeUs
             << ",\"status\":\"" << weaknet_utils::escapeJsonString(health.status)
             << "\"}";
    }
    json << "]}";

    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;
    DBusMessageIter args;
    dbus_message_iter_init_append(reply, &args);
    const std::string result = json.str();
    const char* value = result.c_str();
    dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &value);
    bool ok = dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return ok;
}

/**
 * @brief DBus 方法实现：GetHistory —— 从数据库查询历史记录
 * @param conn DBus 连接
 * @param msg 接收到的方法调用消息（参数：interface[可选]、start[可选]、end[可选]、limit[可选，默认 100]）
 * @return true 回复发送成功；DB 返回值是 JSON 字符串（或错误 JSON）
 *
 * 参数解析采用"逐次 dbus_message_iter_next"风格：先按约定顺序迭代，缺省的用默认值填充。
 * 任何参数类型都没有严格强制——缺失或类型不符就跳过，用默认值。
 */
bool DbusService::handleGetHistory(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleGetHistory called");

    // 解析参数：interface(string), start(string), end(string), limit(int32)
    std::string iface_filter, start_time, end_time;
    int32_t limit = 100;

    DBusMessageIter args;
    dbus_message_iter_init_append(msg, &args);

    // 参数 1: interface (string)，可缺省
    if (dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_STRING) {
        const char* val = nullptr;
        dbus_message_iter_get_basic(&args, &val);
        if (val) iface_filter = val;
    }
    if (dbus_message_iter_next(&args)) {
        // 参数 2: start (string)
        if (dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_STRING) {
            const char* val = nullptr;
            dbus_message_iter_get_basic(&args, &val);
            if (val) start_time = val;
        }
    }
    if (dbus_message_iter_next(&args)) {
        // 参数 3: end (string)
        if (dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_STRING) {
            const char* val = nullptr;
            dbus_message_iter_get_basic(&args, &val);
            if (val) end_time = val;
        }
    }
    if (dbus_message_iter_next(&args)) {
        // 参数 4: limit (int32)
        if (dbus_message_iter_get_arg_type(&args) == DBUS_TYPE_INT32) {
            dbus_message_iter_get_basic(&args, &limit);
        }
    }

    std::string result = "[]";
    if (ctx_ && ctx_->db_mgr && ctx_->db_mgr->isOpen()) {
        result = ctx_->db_mgr->queryHistory(iface_filter, start_time, end_time, limit);
    } else {
        result = "{\"error\":\"database not available\"}";
    }

    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) return false;

    DBusMessageIter reply_args;
    dbus_message_iter_init_append(reply, &reply_args);
    const char* s = result.c_str();
    dbus_message_iter_append_basic(&reply_args, DBUS_TYPE_STRING, &s);
    dbus_connection_send(conn, reply, nullptr);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
    return true;
}

}  // namespace weaknet_dbus

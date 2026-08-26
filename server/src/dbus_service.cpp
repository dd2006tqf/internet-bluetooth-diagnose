// dbus_service.cpp
// 实现 DBus 服务类：方法处理与信号发送

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
#include "dns_monitor.hpp"
#include "wifi_packet_loss_monitor.hpp"
#include "http_latency_monitor.hpp"
#include "process_net_profiler.hpp"
#include "database_manager.hpp"
#include <sstream>

namespace weaknet_dbus {

DbusService::DbusService(ServerContext* ctx) : ctx_(ctx) {}

// 静态自由函数，转调到对象实例
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
    if (dbus_message_is_method_call(msg, kInterface, kMethodGetHistory)) {
        self->handleGetHistory(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

bool DbusService::register_on_connection(DBusConnection* conn) {
    static DBusObjectPathVTable vtable{};
    vtable.message_function = &MessageHandlerStatic;
    return dbus_connection_register_object_path(conn, kObjectPath, &vtable, this);
}

// 内部辅助方法：发送 D-Bus 信号（带重试）
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

        // 添加所有参数
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

bool DbusService::emitChanged(const std::string& message, int32_t counter) {
    std::lock_guard<std::mutex> lock(send_mutex_);

    // 构造参数列表
    const char* s = message.c_str();
    std::vector<std::pair<int, const void*>> args = {
        {DBUS_TYPE_STRING, &s},
        {DBUS_TYPE_INT32, &counter}
    };

    bool ok = sendSignalInternal(kSignalChanged, args, counter);

    // 序列化到文件（保持原有逻辑）
    if (ok) {
        ChangedPayload payload{message, counter};
        std::string err;
        serializeChangedPayloadToFile(payload, kSignalSerializedFile, &err);
    }

    return ok;
}

// MessageHandler 实现已移动到静态自由函数

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
        // 发送错误回复
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

bool DbusService::replyStringArray(DBusConnection* conn, DBusMessage* msg, const std::vector<std::string>& arr) {
    DBusMessage* reply = dbus_message_new_method_return(msg);
    if (!reply) {
        LOG_ERROR(LogModule::DBUS, "replyStringArray: failed to create reply");
        return false;
    }
    DBusMessageIter iter;
    dbus_message_iter_init_append(reply, &iter);
    DBusMessageIter array_iter;
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

bool DbusService::handleListInterfaces(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleListInterfaces called");
    // 接口列表唯一事实源 = WeakNetMgr::current_interfaces_（线程安全接口）
    std::vector<NetInfo> snapshot = ctx_->weak_mgr->getCurrentInterfaces();
    return replyStringArray(conn, msg, WeakNetMgr::namesOf(snapshot));
}

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
    
    // 构建结果字符串
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

bool DbusService::handleGetBluetoothDevices(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleGetBluetoothDevices called");

    BtMonitor* monitor = ctx_ ? ctx_->bt_monitor.get() : nullptr;
    if (!monitor) {
        // 无蓝牙监测器 → 返回空数组
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

    // 获取设备列表，格式化为 JSON 风格字符串数组
    auto devices = monitor->getDevices();
    std::vector<std::string> lines;
    lines.reserve(devices.size());
    for (const auto& dev : devices) {
        // 格式: "MAC|Name|RSSI|Connected|Type|Level"
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

bool DbusService::handleGetHistory(DBusConnection* conn, DBusMessage* msg) {
    LOG_INFO(LogModule::DBUS, "handleGetHistory called");

    // 解析参数：interface(JSON string), start(JSON string), end(JSON string), limit(INT32)
    std::string iface_filter, start_time, end_time;
    int32_t limit = 100;

    DBusMessageIter args;
    dbus_message_iter_init_append(msg, &args);

    // 参数 1: interface (string)
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


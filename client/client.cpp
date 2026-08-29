// client.cpp
// 使用 libdbus-1 客户端：提供对外调用接口与server通信的工具库

#include <dbus/dbus.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>

#include "common.hpp"
#include "serializer.hpp"
#include "weaknet_client.h"
#include "logger.hpp"

namespace weaknet_dbus {

// 客户端连接管理类
class WeakNetClient {
private:
    DBusConnection* conn_;
    bool connected_;

    // 初始化DBus连接
    bool initConnection() {
        DBusError err;
        dbus_error_init(&err);
        conn_ = dbus_bus_get(DBUS_BUS_SESSION, &err);
        if (dbus_error_is_set(&err)) {
            LOG_ERROR(LogModule::CLIENT, "连接DBus总线失败: " << err.message);
            dbus_error_free(&err);
        }
        connected_ = (conn_ != nullptr);
        return connected_;
    }

public:
    WeakNetClient() : conn_(nullptr), connected_(false) {}
    
    ~WeakNetClient() {
        if (conn_) {
            dbus_connection_unref(conn_);
        }
    }

    // 连接到服务
    bool connect() {
        return initConnection();
    }

    // 检查连接状态
    bool isConnected() const {
        return connected_ && conn_ != nullptr;
    }

    // 调用GetInterfaces方法获取当前网络接口列表
    bool getInterfaces(std::string& result, std::string& errorMsg) {
        if (!isConnected()) {
            errorMsg = "客户端未连接";
            return false;
        }

        DBusMessage* msg = dbus_message_new_method_call(kBusName, kObjectPath, kInterface, kMethodGetInterfaces);
        if (!msg) {
            errorMsg = "创建方法调用消息失败";
            return false;
        }

        DBusError err;
        dbus_error_init(&err);

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn_, msg, 5000, &err);
        dbus_message_unref(msg);
        
        if (dbus_error_is_set(&err)) {
            errorMsg = std::string("调用失败: ") + err.message;
            dbus_error_free(&err);
            return false;
        }
        
        if (!reply) {
            errorMsg = "未收到应答";
            return false;
        }

        DBusMessageIter iter;
        if (!dbus_message_iter_init(reply, &iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
            errorMsg = "解析接口列表失败: 返回类型不是字符串数组";
            dbus_message_unref(reply);
            return false;
        }

        DBusMessageIter array_iter;
        dbus_message_iter_recurse(&iter, &array_iter);

        result.clear();
        while (dbus_message_iter_get_arg_type(&array_iter) == DBUS_TYPE_STRING) {
            const char* iface = nullptr;
            dbus_message_iter_get_basic(&array_iter, &iface);
            if (iface && iface[0] != '\0') {
                if (!result.empty()) {
                    result += ",";
                }
                result += iface;
            }
            dbus_message_iter_next(&array_iter);
        }

        dbus_message_unref(reply);
        return true;
    }

    // 调用HealthCheck方法获取网络健康检查结果
    bool healthCheck(std::string& result, std::string& errorMsg) {
        if (!isConnected()) {
            errorMsg = "客户端未连接";
            return false;
        }

        DBusMessage* msg = dbus_message_new_method_call(kBusName, kObjectPath, kInterface, kMethodHealthCheck);
        if (!msg) {
            errorMsg = "创建健康检查消息失败";
            return false;
        }

        DBusError err;
        dbus_error_init(&err);

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn_, msg, 5000, &err);
        dbus_message_unref(msg);

        if (dbus_error_is_set(&err)) {
            errorMsg = std::string("健康检查调用失败: ") + err.message;
            dbus_error_free(&err);
            return false;
        }

        if (!reply) {
            errorMsg = "未收到健康检查应答";
            return false;
        }

        const char* s = nullptr;
        if (!dbus_message_get_args(reply, &err, DBUS_TYPE_STRING, &s, DBUS_TYPE_INVALID)) {
            if (dbus_error_is_set(&err)) {
                errorMsg = std::string("解析健康检查返回失败: ") + err.message;
                dbus_error_free(&err);
            } else {
                errorMsg = "解析健康检查返回失败";
            }
            dbus_message_unref(reply);
            return false;
        }

        result = s ? s : "";
        dbus_message_unref(reply);
        return true;
    }

    // 订阅网络状态变化信号
    bool subscribeToChanges(bool (*callback)(const std::string& message, int32_t counter) = nullptr) {
        if (!isConnected()) {
            return false;
        }

        DBusError err;
        dbus_error_init(&err);

        std::string rule = std::string("type='signal',interface='") + kInterface + "',member='" + kSignalChanged + "'";
        dbus_bus_add_match(conn_, rule.c_str(), &err);
        dbus_connection_flush(conn_);
        
        if (dbus_error_is_set(&err)) {
            LOG_ERROR(LogModule::CLIENT, "添加匹配规则失败: " << err.message);
            dbus_error_free(&err);
            return false;
        }

        LOG_INFO(LogModule::CLIENT, "已订阅 " << kSignalChanged << " 信号，等待变化通知...");
        
        // 定期输出客户端状态
        auto lastStatusTime = std::chrono::steady_clock::now();
        const auto statusInterval = std::chrono::seconds(5);
        
        while (true) {
            dbus_connection_read_write(conn_, 100);
            DBusMessage* msg = dbus_connection_pop_message(conn_);
            
            // 定期输出客户端状态
            auto now = std::chrono::steady_clock::now();
            if (now - lastStatusTime >= statusInterval) {
                LOG_INFO(LogModule::CLIENT, "CLIENT_STATUS: 连接正常，等待网络变化信号...");
                lastStatusTime = now;
            }
            
            if (!msg) continue;

            if (dbus_message_is_signal(msg, kInterface, kSignalChanged)) {
                const char* text = nullptr;
                int32_t counter = 0;
                DBusError e;
                dbus_error_init(&e);
                
                if (dbus_message_get_args(msg, &e, DBUS_TYPE_STRING, &text, DBUS_TYPE_INT32, &counter, DBUS_TYPE_INVALID)) {
                    LOG_INFO(LogModule::CLIENT, "收到网络状态变化: '" << (text ? text : "<null>") << "', counter=" << counter);
                    
                    // 调用回调函数（如果提供）
                    if (callback && callback(text ? std::string(text) : "", counter)) {
                        dbus_message_unref(msg);
                        break; // 回调返回true时停止监听
                    }
                } else if (dbus_error_is_set(&e)) {
                    LOG_ERROR(LogModule::CLIENT, "解析信号失败: " << e.message);
                    dbus_error_free(&e);
                }

                // 读取服务端序列化到文件的信号负载
                ChangedPayload restored{};
                std::string ferr;
                if (deserializeChangedPayloadFromFile(kSignalSerializedFile, &restored, &ferr)) {
                    LOG_INFO(LogModule::CLIENT, "从文件读取的详细信息: text='" << restored.message << "', counter=" << restored.counter);
                }
            }

            dbus_message_unref(msg);
        }
        return true;
    }

    // 单次检查网络状态变化（非阻塞）
    bool checkForChanges(std::string& message, int32_t& counter) {
        if (!isConnected()) {
            return false;
        }

        dbus_connection_read_write(conn_, 0); // 非阻塞轮询
        DBusMessage* msg = dbus_connection_pop_message(conn_);
        if (!msg) return false;

        if (dbus_message_is_signal(msg, kInterface, kSignalChanged)) {
            const char* text = nullptr;
            DBusError e;
            dbus_error_init(&e);
            
            if (dbus_message_get_args(msg, &e, DBUS_TYPE_STRING, &text, DBUS_TYPE_INT32, &counter, DBUS_TYPE_INVALID)) {
                message = text ? std::string(text) : "";
                dbus_message_unref(msg);
                return true;
            } else if (dbus_error_is_set(&e)) {
                dbus_error_free(&e);
            }
        }

        dbus_message_unref(msg);
        return false;
    }

    // 发送网络健康检查请求
    bool requestHealthCheck(std::string& result, std::string& errorMsg) {
        return healthCheck(result, errorMsg);
    }

    // 读取最新的网络接口状态（从序列化文件）
    bool getLatestFromFile(std::string& result, std::string& errorMsg) {
        std::string file_err;
        if (deserializeGetReplyFromFile(kGetReplySerializedFile, &result, &file_err)) {
            return true;
        } else {
            errorMsg = std::string("读取序列化文件失败: ") + file_err;
            return false;
        }
    }

    // Ping指定主机
    bool pingHost(const std::string& hostname, std::string& result, std::string& errorMsg) {
        if (!isConnected()) {
            errorMsg = "客户端未连接";
            return false;
        }

        if (hostname.empty()) {
            errorMsg = "主机名不能为空";
            return false;
        }

        DBusMessage* msg = dbus_message_new_method_call(kBusName, kObjectPath, kInterface, kMethodPing);
        if (!msg) {
            errorMsg = "创建ping方法调用消息失败";
            return false;
        }

        DBusMessageIter args;
        dbus_message_iter_init_append(msg, &args);
        const char* host = hostname.c_str();
        if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &host)) {
            dbus_message_unref(msg);
            errorMsg = "添加主机名参数失败";
            return false;
        }

        DBusError err;
        dbus_error_init(&err);

        DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn_, msg, 10000, &err); // 10秒超时
        dbus_message_unref(msg);
        
        if (dbus_error_is_set(&err)) {
            errorMsg = std::string("ping调用失败: ") + err.message;
            dbus_error_free(&err);
            return false;
        }
        
        if (!reply) {
            errorMsg = "未收到ping应答";
            return false;
        }

        const char* s = nullptr;
        if (!dbus_message_get_args(reply, &err, DBUS_TYPE_STRING, &s, DBUS_TYPE_INVALID)) {
            if (dbus_error_is_set(&err)) {
                errorMsg = std::string("解析ping返回失败: ") + err.message;
                dbus_error_free(&err);
            } else {
                errorMsg = "解析ping返回失败";
            }
            dbus_message_unref(reply);
            return false;
        }
        
        result = s ? s : "";
        dbus_message_unref(reply);
        return true;
    }

    // 断开连接
    void disconnect() {
        if (conn_) {
            dbus_connection_unref(conn_);
            conn_ = nullptr;
            connected_ = false;
        }
    }

    // 获取连接对象（用于C接口）
    DBusConnection* getConnection() const {
        return conn_;
    }

    // 订阅特定事件类型
    bool subscribeToEvent(const std::string& eventType) {
        if (!isConnected()) return false;

        DBusError err;
        dbus_error_init(&err);

        // 添加事件信号匹配规则
        std::string rule = std::string("type='signal',interface='") + kInterface + "',member='" + eventType + "'";
        dbus_bus_add_match(conn_, rule.c_str(), &err);
        dbus_connection_flush(conn_);
        
        if (dbus_error_is_set(&err)) {
            LOG_ERROR(LogModule::CLIENT, "添加事件匹配规则失败: " << err.message);
            dbus_error_free(&err);
            return false;
        }

        LOG_INFO(LogModule::CLIENT, "已订阅事件: " << eventType);
        return true;
    }

    // 订阅网络质量事件
    bool subscribeToNetworkQuality(bool (*callback)(const std::string& quality, const std::string& details, int32_t counter) = nullptr) {
        if (!isConnected()) {
            return false;
        }

        DBusError err;
        dbus_error_init(&err);

        // 订阅网络质量变化信号
        std::string rule = std::string("type='signal',interface='") + kInterface + "',member='" + kSignalNetworkQualityChanged + "'";
        dbus_bus_add_match(conn_, rule.c_str(), &err);
        dbus_connection_flush(conn_);
        
        if (dbus_error_is_set(&err)) {
            LOG_ERROR(LogModule::CLIENT, "添加网络质量事件匹配规则失败: " << err.message);
            dbus_error_free(&err);
            return false;
        }

        LOG_INFO(LogModule::CLIENT, "已订阅网络质量事件，等待质量变化通知...");
        
        // 定期输出客户端状态
        auto lastStatusTime = std::chrono::steady_clock::now();
        const auto statusInterval = std::chrono::seconds(5);
        
        while (true) {
            dbus_connection_read_write(conn_, 100);
            DBusMessage* msg = dbus_connection_pop_message(conn_);
            
            // 定期输出客户端状态
            auto now = std::chrono::steady_clock::now();
            if (now - lastStatusTime >= statusInterval) {
                LOG_INFO(LogModule::CLIENT, "CLIENT_STATUS: 连接正常，等待网络质量变化信号...");
                lastStatusTime = now;
            }
            
            if (!msg) continue;

            if (dbus_message_is_signal(msg, kInterface, kSignalNetworkQualityChanged)) {
                const char* quality = nullptr;
                const char* details = nullptr;
                int32_t counter = 0;
                DBusError e;
                dbus_error_init(&e);
                
                if (dbus_message_get_args(msg, &e, DBUS_TYPE_STRING, &quality, DBUS_TYPE_STRING, &details, DBUS_TYPE_INT32, &counter, DBUS_TYPE_INVALID)) {
                    LOG_INFO(LogModule::CLIENT, "收到网络质量变化: quality='" << (quality ? quality : "<null>") 
                        << "', details='" << (details ? details : "<null>") << "', counter=" << counter);
                    
                    // 调用回调函数（如果提供）
                    if (callback && callback(quality ? std::string(quality) : "", details ? std::string(details) : "", counter)) {
                        dbus_message_unref(msg);
                        break; // 回调返回true时停止监听
                    }
                } else if (dbus_error_is_set(&e)) {
                    LOG_ERROR(LogModule::CLIENT, "解析网络质量信号失败: " << e.message);
                    dbus_error_free(&e);
                }
            }

            dbus_message_unref(msg);
        }
        return true;
    }

    // 非阻塞检查事件
    bool checkForEvents(std::string& eventType, std::string& message, int32_t& counter, std::string& source) {
        if (!isConnected()) return false;

        dbus_connection_read_write(conn_, 0); // 非阻塞轮询
        DBusMessage* msg = dbus_connection_pop_message(conn_);
        if (!msg) return false;

        // 检查是否为事件信号
        if (dbus_message_is_signal(msg, kInterface, kSignalInterfaceChanged) ||
            dbus_message_is_signal(msg, kInterface, kSignalConnectionModeChanged) ||
            dbus_message_is_signal(msg, kInterface, kSignalNetworkQualityChanged) ||
            dbus_message_is_signal(msg, kInterface, kSignalBluetoothDeviceChanged)) {
            
            const char* signal_name = dbus_message_get_member(msg);
            const char* text = nullptr;
            DBusError e;
            dbus_error_init(&e);
            
            if (dbus_message_get_args(msg, &e, DBUS_TYPE_STRING, &text, DBUS_TYPE_INT32, &counter, DBUS_TYPE_INVALID)) {
                eventType = signal_name ? signal_name : "unknown";
                message = text ? std::string(text) : "";
                source = "event_manager";
                dbus_message_unref(msg);
                return true;
            } else if (dbus_error_is_set(&e)) {
                dbus_error_free(&e);
            }
        }

        dbus_message_unref(msg);
        return false;
    }

    // ----- 蓝牙设备查询 -----

    // 获取蓝牙设备列表
    bool getBluetoothDevices(std::string& result, std::string& errorMsg) {
        if (!isConnected()) {
            errorMsg = "客户端未连接";
            return false;
        }

        DBusMessage* msg = dbus_message_new_method_call(kBusName, kObjectPath, kInterface, kMethodGetBluetoothDevices);
        if (!msg) {
            errorMsg = "创建蓝牙设备查询消息失败";
            return false;
        }

        DBusError err;
        dbus_error_init(&err);
        DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn_, msg, 3000, &err);
        dbus_message_unref(msg);

        if (dbus_error_is_set(&err)) {
            errorMsg = std::string("蓝牙设备查询失败: ") + err.message;
            dbus_error_free(&err);
            return false;
        }
        if (!reply) {
            errorMsg = "未收到蓝牙设备查询应答";
            return false;
        }

        DBusMessageIter iter;
        if (!dbus_message_iter_init(reply, &iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
            errorMsg = "解析蓝牙设备列表失败";
            dbus_message_unref(reply);
            return false;
        }

        DBusMessageIter array_iter;
        dbus_message_iter_recurse(&iter, &array_iter);
        result.clear();
        while (dbus_message_iter_get_arg_type(&array_iter) == DBUS_TYPE_STRING) {
            const char* dev = nullptr;
            dbus_message_iter_get_basic(&array_iter, &dev);
            if (dev && dev[0] != '\0') {
                if (!result.empty()) result += "\n";
                result += dev;
            }
            dbus_message_iter_next(&array_iter);
        }
        dbus_message_unref(reply);
        return true;
    }

    // 获取蓝牙适配器信息
    bool getBluetoothAdapter(std::string& result, std::string& errorMsg) {
        if (!isConnected()) {
            errorMsg = "客户端未连接";
            return false;
        }

        DBusMessage* msg = dbus_message_new_method_call(kBusName, kObjectPath, kInterface, kMethodGetBluetoothAdapter);
        if (!msg) {
            errorMsg = "创建蓝牙适配器查询消息失败";
            return false;
        }

        DBusError err;
        dbus_error_init(&err);
        DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn_, msg, 3000, &err);
        dbus_message_unref(msg);

        if (dbus_error_is_set(&err)) {
            errorMsg = std::string("蓝牙适配器查询失败: ") + err.message;
            dbus_error_free(&err);
            return false;
        }
        if (!reply) {
            errorMsg = "未收到蓝牙适配器查询应答";
            return false;
        }

        const char* s = nullptr;
        if (!dbus_message_get_args(reply, &err, DBUS_TYPE_STRING, &s, DBUS_TYPE_INVALID)) {
            errorMsg = "解析蓝牙适配器信息失败";
            dbus_message_unref(reply);
            return false;
        }
        result = s ? s : "";
        dbus_message_unref(reply);
        return true;
    }

    // 获取 DNS 监控统计
    bool getDnsStats(std::string& result, std::string& errorMsg) {
        if (!isConnected()) return fail("客户端未连接", errorMsg);
        return requestStringData(kMethodGetDnsStats, "DNS 监控统计", result, errorMsg);
    }

    // 获取 Wi-Fi 丢包统计
    bool getWifiLossStats(std::string& result, std::string& errorMsg) {
        if (!isConnected()) return fail("客户端未连接", errorMsg);
        return requestStringData(kMethodGetWifiLossStats, "Wi-Fi 丢包统计", result, errorMsg);
    }

    // 获取 HTTP 请求延迟统计
    bool getHttpLatencyStats(std::string& result, std::string& errorMsg) {
        if (!isConnected()) return fail("客户端未连接", errorMsg);
        return requestStringData(kMethodGetHttpLatencyStats, "HTTP 请求延迟统计", result, errorMsg);
    }

    // 获取进程网络画像
    bool getProcessProfiling(std::string& result, std::string& errorMsg) {
        if (!isConnected()) return fail("客户端未连接", errorMsg);
        return requestStringData(kMethodGetProcessProfiling, "进程网络画像", result, errorMsg);
    }

    // 获取六个 eBPF 监控器健康与性能快照
    bool getEbpfMonitorHealth(std::string& result, std::string& errorMsg) {
        if (!isConnected()) return fail("客户端未连接", errorMsg);
        return requestStringData(kMethodGetEbpfMonitorHealth, "eBPF 监控器健康状态", result, errorMsg);
    }

    // 查询历史监控数据（带参数）
    bool getHistory(const std::string& iface, const std::string& start,
                    const std::string& end, int32_t limit,
                    std::string& result, std::string& errorMsg) {
        if (!isConnected()) return fail("客户端未连接", errorMsg);

        DBusMessage* msg = dbus_message_new_method_call(kBusName, kObjectPath, kInterface, kMethodGetHistory);
        if (!msg) {
            errorMsg = "创建历史查询消息失败";
            return false;
        }

        // 添加参数：interface(string), start(string), end(string), limit(int32)
        DBusMessageIter args;
        dbus_message_iter_init_append(msg, &args);

        const char* iface_cstr = iface.c_str();
        const char* start_cstr = start.c_str();
        const char* end_cstr = end.c_str();

        dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &iface_cstr);
        dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &start_cstr);
        dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &end_cstr);
        dbus_message_iter_append_basic(&args, DBUS_TYPE_INT32, &limit);

        DBusError err;
        dbus_error_init(&err);
        DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn_, msg, 5000, &err);
        dbus_message_unref(msg);

        if (dbus_error_is_set(&err)) {
            errorMsg = "历史查询失败: " + std::string(err.message);
            dbus_error_free(&err);
            return false;
        }
        if (!reply) {
            errorMsg = "未收到历史查询应答";
            return false;
        }

        const char* data = nullptr;
        if (!dbus_message_get_args(reply, &err, DBUS_TYPE_STRING, &data, DBUS_TYPE_INVALID)) {
            errorMsg = "解析历史查询结果失败";
            dbus_message_unref(reply);
            return false;
        }
        result = data ? data : "";
        dbus_message_unref(reply);
        return true;
    }

private:
    // 统一失败的辅助函数
    bool fail(const char* msg, std::string& errorMsg) {
        errorMsg = msg;
        return false;
    }

    // 通用字符串返回型 D-Bus 方法调用
    bool requestStringData(const char* methodName, const char* label,
                           std::string& result, std::string& errorMsg) {
        DBusMessage* msg = dbus_message_new_method_call(kBusName, kObjectPath, kInterface, methodName);
        if (!msg) {
            errorMsg = std::string("创建") + label + "查询消息失败";
            return false;
        }
        DBusError err;
        dbus_error_init(&err);
        DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn_, msg, 3000, &err);
        dbus_message_unref(msg);
        if (dbus_error_is_set(&err)) {
            errorMsg = std::string(label) + "查询失败: " + err.message;
            dbus_error_free(&err);
            return false;
        }
        if (!reply) {
            errorMsg = "未收到" + std::string(label) + "查询应答";
            return false;
        }
        const char* data = nullptr;
        if (!dbus_message_get_args(reply, &err, DBUS_TYPE_STRING, &data, DBUS_TYPE_INVALID)) {
            errorMsg = "解析" + std::string(label) + "信息失败";
            dbus_message_unref(reply);
            return false;
        }
        result = data ? data : "";
        dbus_message_unref(reply);
        return true;
    }

    // 订阅蓝牙设备变化事件
    bool subscribeToBluetoothEvents(bool (*callback)(const std::string& message, int32_t counter) = nullptr) {
        if (!isConnected()) return false;

        DBusError err;
        dbus_error_init(&err);
        std::string rule = std::string("type='signal',interface='") + kInterface + "',member='" + kSignalBluetoothDeviceChanged + "'";
        dbus_bus_add_match(conn_, rule.c_str(), &err);
        dbus_connection_flush(conn_);
        if (dbus_error_is_set(&err)) {
            LOG_ERROR(LogModule::CLIENT, "添加蓝牙事件匹配规则失败: " << err.message);
            dbus_error_free(&err);
            return false;
        }
        LOG_INFO(LogModule::CLIENT, "已订阅蓝牙设备变化事件");

        auto lastStatusTime = std::chrono::steady_clock::now();
        const auto statusInterval = std::chrono::seconds(5);

        while (true) {
            dbus_connection_read_write(conn_, 100);
            DBusMessage* signal = dbus_connection_pop_message(conn_);

            auto now = std::chrono::steady_clock::now();
            if (now - lastStatusTime >= statusInterval) {
                LOG_INFO(LogModule::CLIENT, "BT_CLIENT_STATUS: 等待蓝牙设备变化信号...");
                lastStatusTime = now;
            }
            if (!signal) continue;

            if (dbus_message_is_signal(signal, kInterface, kSignalBluetoothDeviceChanged)) {
                const char* text = nullptr;
                int32_t counter = 0;
                DBusError e;
                dbus_error_init(&e);
                if (dbus_message_get_args(signal, &e, DBUS_TYPE_STRING, &text, DBUS_TYPE_INT32, &counter, DBUS_TYPE_INVALID)) {
                    LOG_INFO(LogModule::CLIENT, "收到蓝牙设备变化: '" << (text ? text : "<null>") << "', counter=" << counter);
                    if (callback && callback(text ? std::string(text) : "", counter)) {
                        dbus_message_unref(signal);
                        break;
                    }
                } else if (dbus_error_is_set(&e)) {
                    dbus_error_free(&e);
                }
            }
            dbus_message_unref(signal);
        }
        return true;
    }

private:
    std::string getSignalMember(DBusMessage* msg) {
        const char* member = dbus_message_get_member(msg);
        return member ? std::string(member) : "";
    }
};

// 全局客户端实例（单例模式）
static WeakNetClient* g_client = nullptr;

// 初始化客户端
extern "C" bool weaknet_init() {
    if (g_client) {
        LOG_INFO(LogModule::CLIENT, "weaknet_init: already initialized, connected=" << g_client->isConnected());
        return g_client->isConnected();
    }

    // 初始化日志系统（客户端使用独立的日志目录）
    Logger::init("weaknet-client", "./logs/client");

    g_client = new WeakNetClient();
    bool result = g_client->connect();
    LOG_INFO(LogModule::CLIENT, "weaknet_init: connect result=" << result);
    return result;
}

// 清理客户端
extern "C" void weaknet_cleanup() {
    LOG_INFO(LogModule::CLIENT, "weaknet_cleanup: cleaning up");
    if (g_client) {
        g_client->disconnect();
        delete g_client;
        g_client = nullptr;
    }
    Logger::shutdown();
}

// 获取当前网络接口信息
extern "C" bool weaknet_get_interfaces(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size) {
    if (!g_client || !g_client->isConnected()) {
        LOG_ERROR(LogModule::CLIENT, "weaknet_get_interfaces: client not connected");
        snprintf(error_buffer, error_size, "客户端未连接");
        return false;
    }

    std::string result, errorMsg;
    if (g_client->getInterfaces(result, errorMsg)) {
        LOG_INFO(LogModule::CLIENT, "weaknet_get_interfaces: success, result size=" << result.size());
        snprintf(buffer, buffer_size, "%s", result.c_str());
        return true;
    } else {
        LOG_ERROR(LogModule::CLIENT, "weaknet_get_interfaces: failed: " << errorMsg);
        snprintf(error_buffer, error_size, "%s", errorMsg.c_str());
        return false;
    }
}

// 获取网络状态变化（非阻塞）
extern "C" bool weaknet_check_changes(char* message_buffer, size_t message_size, int32_t* counter, char* error_buffer, size_t error_size) {
    if (!g_client || !g_client->isConnected()) {
        LOG_ERROR(LogModule::CLIENT, "weaknet_check_changes: client not connected");
        snprintf(error_buffer, error_size, "客户端未连接");
        return false;
    }

    std::string message;
    if (g_client->checkForChanges(message, *counter)) {
        LOG_INFO(LogModule::CLIENT, "weaknet_check_changes: change detected");
        snprintf(message_buffer, message_size, "%s", message.c_str());
        return true;
    }

    snprintf(error_buffer, error_size, "无新的状态变化");
    return false;
}

// 请求网络健康检查
extern "C" bool weaknet_health_check(char* result_buffer, size_t result_size, char* error_buffer, size_t error_size) {
    if (!g_client || !g_client->isConnected()) {
        LOG_ERROR(LogModule::CLIENT, "weaknet_health_check: client not connected");
        snprintf(error_buffer, error_size, "客户端未连接");
        return false;
    }

    std::string result, errorMsg;
    if (g_client->requestHealthCheck(result, errorMsg)) {
        LOG_INFO(LogModule::CLIENT, "weaknet_health_check: success");
        snprintf(result_buffer, result_size, "%s", result.c_str());
        return true;
    } else {
        LOG_ERROR(LogModule::CLIENT, "weaknet_health_check: failed: " << errorMsg);
        snprintf(error_buffer, error_size, "%s", errorMsg.c_str());
        return false;
    }
}

// 从文件读取最新状态（离线模式）
extern "C" bool weaknet_get_from_file(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size) {
    if (!g_client) {
        snprintf(error_buffer, error_size, "客户端未初始化");
        return false;
    }

    std::string result, errorMsg;
    if (g_client->getLatestFromFile(result, errorMsg)) {
        snprintf(buffer, buffer_size, "%s", result.c_str());
        return true;
    } else {
        snprintf(error_buffer, error_size, "%s", errorMsg.c_str());
        return false;
    }
}

// Ping指定主机
extern "C" bool weaknet_ping_host(const char* hostname, char* result_buffer, size_t result_size, char* error_buffer, size_t error_size) {
    if (!g_client || !g_client->isConnected()) {
        LOG_ERROR(LogModule::CLIENT, "weaknet_ping_host: client not connected");
        snprintf(error_buffer, error_size, "客户端未连接");
        return false;
    }

    if (!hostname || strlen(hostname) == 0) {
        LOG_ERROR(LogModule::CLIENT, "weaknet_ping_host: hostname is empty");
        snprintf(error_buffer, error_size, "主机名不能为空");
        return false;
    }

    std::string result, errorMsg;
    if (g_client->pingHost(std::string(hostname), result, errorMsg)) {
        LOG_INFO(LogModule::CLIENT, "weaknet_ping_host: success for " << hostname);
        snprintf(result_buffer, result_size, "%s", result.c_str());
        return true;
    } else {
        LOG_ERROR(LogModule::CLIENT, "weaknet_ping_host: failed for " << hostname << ": " << errorMsg);
        snprintf(error_buffer, error_size, "%s", errorMsg.c_str());
        return false;
    }
}

} // namespace weaknet_dbus

// C接口函数实现

using namespace weaknet_dbus;

// 订阅特定事件类型
extern "C" bool weaknet_subscribe_event(const char* event_type, weaknet_event_callback_t callback) {
    if (!weaknet_dbus::g_client || !weaknet_dbus::g_client->isConnected()) {
        LOG_ERROR(LogModule::CLIENT, "weaknet_subscribe_event: client not connected");
        return false;
    }
    LOG_INFO(LogModule::CLIENT, "weaknet_subscribe_event: subscribing to " << event_type);
    return weaknet_dbus::g_client->subscribeToEvent(std::string(event_type));
}

// 取消订阅事件（简化实现）
extern "C" bool weaknet_unsubscribe_event(const char* event_type) {
    // 注意：这个简化实现只是返回成功，实际项目中可能需要更复杂的去订阅逻辑
    // 简化实现，不记录日志
    return true;
}

// 获取支持的事件类型列表
extern "C" bool weaknet_get_event_types(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size) {
    snprintf(buffer, buffer_size, "%s,%s,%s,%s",
             weaknet_dbus::kSignalInterfaceChanged,
             weaknet_dbus::kSignalConnectionModeChanged,
             weaknet_dbus::kSignalNetworkQualityChanged,
             weaknet_dbus::kSignalBluetoothDeviceChanged);
    return true;
}

// 非阻塞检查事件
extern "C" bool weaknet_check_events(char* event_type_buffer, size_t event_type_size,
                                   char* message_buffer, size_t message_size,
                                   int32_t* counter, char* source_buffer, size_t source_size,
                                   char* error_buffer, size_t error_size) {
    if (!weaknet_dbus::g_client || !weaknet_dbus::g_client->isConnected()) {
        LOG_ERROR(LogModule::CLIENT, "weaknet_check_events: client not connected");
        snprintf(error_buffer, error_size, "客户端未连接");
        return false;
    }

    std::string eventType, message, source;
    if (weaknet_dbus::g_client->checkForEvents(eventType, message, *counter, source)) {
        LOG_INFO(LogModule::CLIENT, "weaknet_check_events: event detected: " << eventType);
        snprintf(event_type_buffer, event_type_size, "%s", eventType.c_str());
        snprintf(message_buffer, message_size, "%s", message.c_str());
        snprintf(source_buffer, source_size, "%s", source.c_str());
        return true;
    }

    snprintf(error_buffer, error_size, "没有检测到事件");
    return false;
}

// 检查客户端连接状态
extern "C" bool weaknet_is_connected() {
    return weaknet_dbus::g_client && weaknet_dbus::g_client->isConnected();
}

// 获取WeakNet客户端库版本信息
extern "C" bool weaknet_get_version(char* buffer, size_t buffer_size) {
    snprintf(buffer, buffer_size, "WeakNet Client Library v1.0.0");
    return true;
}

// 获取库的编译时间和编译选项信息
extern "C" bool weaknet_get_build_info(char* buffer, size_t buffer_size) {
    snprintf(buffer, buffer_size, "Built: %s %s | DBus-enabled | C++17", __DATE__, __TIME__);
    return true;
}

// 订阅网络质量事件
extern "C" bool weaknet_subscribe_network_quality(weaknet_network_quality_callback_t callback) {
    if (!weaknet_dbus::g_client || !weaknet_dbus::g_client->isConnected()) {
        LOG_ERROR(LogModule::CLIENT, "weaknet_subscribe_network_quality: client not connected");
        return false;
    }
    LOG_INFO(LogModule::CLIENT, "weaknet_subscribe_network_quality: subscribing");
    
    // 创建C++回调包装器
    static weaknet_network_quality_callback_t s_callback = nullptr;
    s_callback = callback;
    
    auto cpp_callback = [](const std::string& quality, const std::string& details, int32_t counter) -> bool {
        if (s_callback) {
            return s_callback(quality.c_str(), details.c_str(), counter);
        }
        return false;
    };
    
    return weaknet_dbus::g_client->subscribeToNetworkQuality(cpp_callback);
}

// 非阻塞检查网络质量事件
extern "C" bool weaknet_check_network_quality(char* quality_buffer, size_t quality_size,
                                             char* details_buffer, size_t details_size, 
                                             int32_t* counter, char* error_buffer, size_t error_size) {
    if (!weaknet_dbus::g_client || !weaknet_dbus::g_client->isConnected()) {
        snprintf(error_buffer, error_size, "客户端未连接");
        return false;
    }

    if (!weaknet_dbus::g_client->isConnected()) return false;

    dbus_connection_read_write(weaknet_dbus::g_client->getConnection(), 0); // 非阻塞轮询
    DBusMessage* msg = dbus_connection_pop_message(weaknet_dbus::g_client->getConnection());
    if (!msg) return false;

    if (dbus_message_is_signal(msg, weaknet_dbus::kInterface, weaknet_dbus::kSignalNetworkQualityChanged)) {
        const char* quality = nullptr;
        const char* details = nullptr;
        DBusError e;
        dbus_error_init(&e);
        
        if (dbus_message_get_args(msg, &e, DBUS_TYPE_STRING, &quality, DBUS_TYPE_STRING, &details, DBUS_TYPE_INT32, counter, DBUS_TYPE_INVALID)) {
            snprintf(quality_buffer, quality_size, "%s", quality ? quality : "");
            snprintf(details_buffer, details_size, "%s", details ? details : "");
            dbus_message_unref(msg);
            return true;
        } else if (dbus_error_is_set(&e)) {
            snprintf(error_buffer, error_size, "解析网络质量信号失败: %s", e.message);
            dbus_error_free(&e);
        }
    }

    dbus_message_unref(msg);
    snprintf(error_buffer, error_size, "没有检测到网络质量事件");
    return false;
}

// ============== 蓝牙设备 API ==============

extern "C" bool weaknet_get_bluetooth_devices(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size) {
    if (!weaknet_dbus::g_client || !weaknet_dbus::g_client->isConnected()) {
        snprintf(error_buffer, error_size, "客户端未连接");
        return false;
    }
    std::string result, errorMsg;
    if (weaknet_dbus::g_client->getBluetoothDevices(result, errorMsg)) {
        snprintf(buffer, buffer_size, "%s", result.c_str());
        return true;
    } else {
        snprintf(error_buffer, error_size, "%s", errorMsg.c_str());
        return false;
    }
}

extern "C" bool weaknet_get_bluetooth_adapter(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size) {
    if (!weaknet_dbus::g_client || !weaknet_dbus::g_client->isConnected()) {
        snprintf(error_buffer, error_size, "客户端未连接");
        return false;
    }
    std::string result, errorMsg;
    if (weaknet_dbus::g_client->getBluetoothAdapter(result, errorMsg)) {
        snprintf(buffer, buffer_size, "%s", result.c_str());
        return true;
    } else {
        snprintf(error_buffer, error_size, "%s", errorMsg.c_str());
        return false;
    }
}

extern "C" bool weaknet_subscribe_bluetooth_events(weaknet_event_callback_t callback) {
    if (!weaknet_dbus::g_client || !weaknet_dbus::g_client->isConnected()) {
        return false;
    }
    // 先添加 D-Bus 信号订阅
    weaknet_dbus::g_client->subscribeToEvent(weaknet_dbus::kSignalBluetoothDeviceChanged);
    return true;
}

// ============== eBPF 监控数据 API ==============

extern "C" bool weaknet_get_dns_stats(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size) {
    if (!weaknet_dbus::g_client || !weaknet_dbus::g_client->isConnected()) {
        snprintf(error_buffer, error_size, "客户端未连接");
        return false;
    }
    std::string result, errorMsg;
    if (weaknet_dbus::g_client->getDnsStats(result, errorMsg)) {
        snprintf(buffer, buffer_size, "%s", result.c_str());
        return true;
    } else {
        snprintf(error_buffer, error_size, "%s", errorMsg.c_str());
        return false;
    }
}

extern "C" bool weaknet_get_wifi_loss_stats(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size) {
    if (!weaknet_dbus::g_client || !weaknet_dbus::g_client->isConnected()) {
        snprintf(error_buffer, error_size, "客户端未连接");
        return false;
    }
    std::string result, errorMsg;
    if (weaknet_dbus::g_client->getWifiLossStats(result, errorMsg)) {
        snprintf(buffer, buffer_size, "%s", result.c_str());
        return true;
    } else {
        snprintf(error_buffer, error_size, "%s", errorMsg.c_str());
        return false;
    }
}

extern "C" bool weaknet_get_http_latency_stats(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size) {
    if (!weaknet_dbus::g_client || !weaknet_dbus::g_client->isConnected()) {
        snprintf(error_buffer, error_size, "客户端未连接");
        return false;
    }
    std::string result, errorMsg;
    if (weaknet_dbus::g_client->getHttpLatencyStats(result, errorMsg)) {
        snprintf(buffer, buffer_size, "%s", result.c_str());
        return true;
    } else {
        snprintf(error_buffer, error_size, "%s", errorMsg.c_str());
        return false;
    }
}

extern "C" bool weaknet_get_process_profiling(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size) {
    if (!weaknet_dbus::g_client || !weaknet_dbus::g_client->isConnected()) {
        snprintf(error_buffer, error_size, "客户端未连接");
        return false;
    }
    std::string result, errorMsg;
    if (weaknet_dbus::g_client->getProcessProfiling(result, errorMsg)) {
        snprintf(buffer, buffer_size, "%s", result.c_str());
        return true;
    } else {
        snprintf(error_buffer, error_size, "%s", errorMsg.c_str());
        return false;
    }
}

extern "C" bool weaknet_get_ebpf_monitor_health(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size) {
    if (!weaknet_dbus::g_client || !weaknet_dbus::g_client->isConnected()) {
        snprintf(error_buffer, error_size, "客户端未连接");
        return false;
    }
    std::string result, errorMsg;
    if (weaknet_dbus::g_client->getEbpfMonitorHealth(result, errorMsg)) {
        snprintf(buffer, buffer_size, "%s", result.c_str());
        return true;
    }
    snprintf(error_buffer, error_size, "%s", errorMsg.c_str());
    return false;
}

// 注意: 此文件现在作为动态库使用，不包含main函数

extern "C" bool weaknet_get_history(const char* interface, const char* start, const char* end,
                                    int32_t limit, char* buffer, size_t buffer_size,
                                    char* error_buffer, size_t error_size) {
    if (!weaknet_dbus::g_client || !weaknet_dbus::g_client->isConnected()) {
        snprintf(error_buffer, error_size, "客户端未连接");
        return false;
    }
    std::string result, errorMsg;
    std::string iface_str = interface ? interface : "";
    std::string start_str = start ? start : "";
    std::string end_str = end ? end : "";
    if (weaknet_dbus::g_client->getHistory(iface_str, start_str, end_str, limit, result, errorMsg)) {
        snprintf(buffer, buffer_size, "%s", result.c_str());
        return true;
    } else {
        snprintf(error_buffer, error_size, "%s", errorMsg.c_str());
        return false;
    }
}
// 所有的API通过C接口函数提供，供其他应用程序调用

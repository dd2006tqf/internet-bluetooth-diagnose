/**
 * @file client.cpp
 * @brief WeakNet 客户端动态库实现 —— 通过 libdbus-1 与 WeakNet 服务端通信
 *
 * 本文件实现了 weaknet_client.h 中声明的所有 C API，核心机制：
 *
 *   1. 通过 libdbus-1 连接 D-Bus Session 总线
 *   2. 所有 D-Bus 调用指向：
 *        - BusName:  com.example.WeakNet
 *        - ObjPath:  /com/example/WeakNet
 *        - Interface: com.example.WeakNet
 *   3. 内部用 WeakNetClient 类封装 D-Bus 连接和消息构造，
 *      通过全局单例 g_client 暴露给 C 接口
 *   4. 事件/信号通过 dbus_bus_add_match 注册匹配规则，
 *      非阻塞轮询用 dbus_connection_read_write(conn, 0) + pop_message
 *
 * 注意：本文件现在编译为动态库（libweaknet_client.so），不含 main 函数。
 */

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

/**
 * @brief WeakNet D-Bus 客户端封装类
 *
 * 管理与 WeakNet 服务端的 D-Bus 连接，提供方法调用和信号订阅能力。
 * 此类被包装为 C API（weaknet_* 函数）对外暴露，
 * 外部程序不应直接使用此类。
 */
class WeakNetClient {
private:
    DBusConnection* conn_;    ///< D-Bus Session 总线连接句柄
    bool connected_;          ///< 连接状态标记

    /**
     * @brief 初始化 D-Bus 连接（Session 总线）
     *
     * 调用 dbus_bus_get(DBUS_BUS_SESSION, ...) 连接到当前桌面会话的 D-Bus。
     * 连接失败时记录日志并将 connected_ 置为 false。
     *
     * @return true  - 连接成功
     * @return false - 连接失败
     */
    bool initConnection() {
        DBusError err;
        dbus_error_init(&err);
        // 连接到 D-Bus Session 总线
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

    /** @brief 发起 D-Bus 连接
     *  @return 连接是否成功 */
    bool connect() {
        return initConnection();
    }

    /** @brief 查询当前是否已连接
     *  @return true=已连接 */
    bool isConnected() const {
        return connected_ && conn_ != nullptr;
    }

    /**
     * @brief 调用 GetInterfaces 方法获取当前网络接口列表
     *
     * D-Bus 调用：
     *   - Method:  GetInterfaces
     *   - Returns: ARRAY of STRING
     *   - 解析策略：遍历数组元素，用逗号拼接成 "eth0,wlan0,lo" 格式
     *   - 超时：5000ms（5秒）
     *
     * @param result   输出：逗号分隔的网卡名称
     * @param errorMsg 输出：失败时的错误描述
     * @return true=成功
     */
    bool getInterfaces(std::string& result, std::string& errorMsg) {
        if (!isConnected()) {
            errorMsg = "客户端未连接";
            return false;
        }

        // 构造方法调用消息：com.example.WeakNet /com/example/WeakNet GetInterfaces
        DBusMessage* msg = dbus_message_new_method_call(kBusName, kObjectPath, kInterface, kMethodGetInterfaces);
        if (!msg) {
            errorMsg = "创建方法调用消息失败";
            return false;
        }

        DBusError err;
        dbus_error_init(&err);

        // 阻塞发送并等待应答（5秒超时）
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

        // 解析返回值：期望一个 ARRAY of STRING
        DBusMessageIter iter;
        if (!dbus_message_iter_init(reply, &iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
            errorMsg = "解析接口列表失败: 返回类型不是字符串数组";
            dbus_message_unref(reply);
            return false;
        }

        // 进入数组内部，逐个取出 STRING 元素
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

    /**
     * @brief 调用 HealthCheck 方法获取网络健康检查结果
     *
     * D-Bus 调用：
     *   - Method:  HealthCheck
     *   - Returns: STRING（JSON 格式诊断报告）
     *   - 超时：5000ms
     *
     * @param result   输出：JSON 诊断字符串
     * @param errorMsg 输出：失败时的错误描述
     * @return true=成功
     */
    bool healthCheck(std::string& result, std::string& errorMsg) {
        if (!isConnected()) {
            errorMsg = "客户端未连接";
            return false;
        }

        // 构造方法调用：HealthCheck
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

        // 解析返回值：单个 STRING 参数
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

    /**
     * @brief 阻塞订阅网络状态变化信号（Changed）
     *
     * 流程：
     *   1. 通过 dbus_bus_add_match 注册匹配规则 "type='signal',interface='com.example.WeakNet',member='Changed'"
     *   2. 进入 while(true) 消息循环，每次 read_write 超时 100ms
     *   3. 弹出消息后检查是否为目标信号，解析 STRING + INT32 两个参数
     *   4. 每次收到信号还会从服务端序列化文件读取更详细的负载
     *   5. 若用户提供了 callback 且返回 true，退出监听
     *
     * D-Bus 信号：
     *   - Signal:  Changed
     *   - Payload: STRING text + INT32 counter
     *
     * @param callback 可选回调，返回 true 时停止监听
     * @return true（正常退出时）
     */
    bool subscribeToChanges(bool (*callback)(const std::string& message, int32_t counter) = nullptr) {
        if (!isConnected()) {
            return false;
        }

        DBusError err;
        dbus_error_init(&err);

        // 注册 D-Bus 信号匹配规则，只接收来自 WeakNet 接口的 Changed 信号
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
            // 非阻塞读写，超时 100ms
            dbus_connection_read_write(conn_, 100);
            DBusMessage* msg = dbus_connection_pop_message(conn_);
            
            // 定期输出客户端状态
            auto now = std::chrono::steady_clock::now();
            if (now - lastStatusTime >= statusInterval) {
                LOG_INFO(LogModule::CLIENT, "CLIENT_STATUS: 连接正常，等待网络变化信号...");
                lastStatusTime = now;
            }
            
            if (!msg) continue;

            // 检查是否为期望的 Changed 信号
            if (dbus_message_is_signal(msg, kInterface, kSignalChanged)) {
                const char* text = nullptr;
                int32_t counter = 0;
                DBusError e;
                dbus_error_init(&e);
                
                // 解析信号参数：STRING text + INT32 counter
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

                // 读取服务端序列化到文件的信号负载（补充更多详细信息）
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

    /**
     * @brief 单次非阻塞检查网络状态变化
     *
     * 从 D-Bus 队列尝试弹出一个 Changed 信号。
     *
     * D-Bus 信号：
     *   - Signal:  Changed
     *   - Payload: STRING text + INT32 counter
     *
     * @param message 输出：变化描述
     * @param counter 输出：事件计数器
     * @return true=捕获到变化
     */
    bool checkForChanges(std::string& message, int32_t& counter) {
        if (!isConnected()) {
            return false;
        }

        dbus_connection_read_write(conn_, 0); // 非阻塞轮询，超时0表示不等
        DBusMessage* msg = dbus_connection_pop_message(conn_);
        if (!msg) return false;

        if (dbus_message_is_signal(msg, kInterface, kSignalChanged)) {
            const char* text = nullptr;
            DBusError e;
            dbus_error_init(&e);
            
            // 解析 STRING + INT32
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

    /** @brief 同 healthCheck()，别名接口 */
    bool requestHealthCheck(std::string& result, std::string& errorMsg) {
        return healthCheck(result, errorMsg);
    }

    /**
     * @brief 读取服务端通过序列化文件留下的最新网络接口状态（离线模式）
     *
     * 不发起 D-Bus 调用，直接反序列化本地文件。
     * @param result   输出：序列化文件内容
     * @param errorMsg 输出：错误描述
     */
    bool getLatestFromFile(std::string& result, std::string& errorMsg) {
        std::string file_err;
        if (deserializeGetReplyFromFile(kGetReplySerializedFile, &result, &file_err)) {
            return true;
        } else {
            errorMsg = std::string("读取序列化文件失败: ") + file_err;
            return false;
        }
    }

    /**
     * @brief 调用 Ping 方法，让服务端对指定主机执行 ICMP Ping
     *
     * D-Bus 调用：
     *   - Method:  Ping
     *   - Args:    STRING hostname
     *   - Returns: STRING（包含平均延迟、丢包率等）
     *   - 超时：10000ms（10秒，因为 Ping 本身可能耗时）
     *
     * @param hostname 目标主机名或 IP
     * @param result   输出：Ping 统计文本
     * @param errorMsg 输出：失败时的错误描述
     * @return true=成功
     */
    bool pingHost(const std::string& hostname, std::string& result, std::string& errorMsg) {
        if (!isConnected()) {
            errorMsg = "客户端未连接";
            return false;
        }

        if (hostname.empty()) {
            errorMsg = "主机名不能为空";
            return false;
        }

        // 构造 Ping 方法调用
        DBusMessage* msg = dbus_message_new_method_call(kBusName, kObjectPath, kInterface, kMethodPing);
        if (!msg) {
            errorMsg = "创建ping方法调用消息失败";
            return false;
        }

        // 追加参数：STRING hostname
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

        // 发送并等待应答，超时 10 秒
        DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn_, msg, 10000, &err);
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

        // 解析返回值：单个 STRING
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

    /** @brief 断开 D-Bus 连接并释放句柄 */
    void disconnect() {
        if (conn_) {
            dbus_connection_unref(conn_);
            conn_ = nullptr;
            connected_ = false;
        }
    }

    /** @brief 获取底层 D-Bus 连接句柄（供 C 接口中的非阻塞检查使用） */
    DBusConnection* getConnection() const {
        return conn_;
    }

    /**
     * @brief 订阅指定事件类型（只添加 match 规则，不阻塞监听）
     *
     * 注册 D-Bus 匹配规则：type='signal', interface='com.example.WeakNet', member=<eventType>
     *
     * @param eventType D-Bus 信号名（如 "InterfaceChanged"、"NetworkQualityChanged" 等）
     * @return true=添加成功
     */
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

    /**
     * @brief 阻塞订阅网络质量变化信号（NetworkQualityChanged）
     *
     * D-Bus 信号：
     *   - Signal:  NetworkQualityChanged
     *   - Payload: STRING quality + STRING details + INT32 counter
     *
     * 监听逻辑同 subscribeToChanges()，回调返回 true 时继续、返回 false 时退出。
     */
    bool subscribeToNetworkQuality(bool (*callback)(const std::string& quality, const std::string& details, int32_t counter) = nullptr) {
        if (!isConnected()) {
            return false;
        }

        DBusError err;
        dbus_error_init(&err);

        // 订阅 NetworkQualityChanged 信号
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

            // 检查是否为 NetworkQualityChanged 信号
            if (dbus_message_is_signal(msg, kInterface, kSignalNetworkQualityChanged)) {
                const char* quality = nullptr;
                const char* details = nullptr;
                int32_t counter = 0;
                DBusError e;
                dbus_error_init(&e);
                
                // 解析三个参数：STRING quality + STRING details + INT32 counter
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

    /**
     * @brief 非阻塞检查多类事件信号
     *
     * 从 D-Bus 队列中弹出以下任一信号：
     *   - InterfaceChanged / ConnectionModeChanged / NetworkQualityChanged / BluetoothDeviceChanged
     *
     * 所有这些信号的 Payload 格式统一为 STRING + INT32。
     *
     * @param eventType 输出：信号成员名（如 "InterfaceChanged"）
     * @param message   输出：变化描述文本
     * @param counter   输出：事件计数器
     * @param source    输出：固定为 "event_manager"
     * @return true=捕获到事件
     */
    bool checkForEvents(std::string& eventType, std::string& message, int32_t& counter, std::string& source) {
        if (!isConnected()) return false;

        dbus_connection_read_write(conn_, 0); // 非阻塞轮询
        DBusMessage* msg = dbus_connection_pop_message(conn_);
        if (!msg) return false;

        // 检查是否为四类事件信号中的任何一个
        if (dbus_message_is_signal(msg, kInterface, kSignalInterfaceChanged) ||
            dbus_message_is_signal(msg, kInterface, kSignalConnectionModeChanged) ||
            dbus_message_is_signal(msg, kInterface, kSignalNetworkQualityChanged) ||
            dbus_message_is_signal(msg, kInterface, kSignalBluetoothDeviceChanged)) {
            
            const char* signal_name = dbus_message_get_member(msg);  // 获取信号名
            const char* text = nullptr;
            DBusError e;
            dbus_error_init(&e);
            
            // 解析 STRING text + INT32 counter
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

    /**
     * @brief 调用 GetBluetoothDevices 方法获取蓝牙设备列表
     *
     * D-Bus 调用：
     *   - Method:  GetBluetoothDevices
     *   - Returns: ARRAY of STRING（每行一个设备，'MAC|Name|RSSI|Connected|Type|Level'）
     *   - 超时：3000ms
     */
    bool getBluetoothDevices(std::string& result, std::string& errorMsg) {
        if (!isConnected()) {
            errorMsg = "客户端未连接";
            return false;
        }

        // 构造方法调用：GetBluetoothDevices
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

        // 解析 ARRAY of STRING，用 '\n' 拼接
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

    /**
     * @brief 调用 GetBluetoothAdapter 方法获取蓝牙适配器信息
     *
     * D-Bus 调用：
     *   - Method:  GetBluetoothAdapter
     *   - Returns: STRING（"Powered:1|Name:xxx|Address:xx:xx:..."）
     *   - 超时：3000ms
     */
    bool getBluetoothAdapter(std::string& result, std::string& errorMsg) {
        if (!isConnected()) {
            errorMsg = "客户端未连接";
            return false;
        }

        // 构造方法调用：GetBluetoothAdapter
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

        // 解析返回值：单个 STRING
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

    /** @brief 调用 GetDnsStats 获取 DNS eBPF 监控统计 */
    bool getDnsStats(std::string& result, std::string& errorMsg) {
        if (!isConnected()) return fail("客户端未连接", errorMsg);
        return requestStringData(kMethodGetDnsStats, "DNS 监控统计", result, errorMsg);
    }

    /** @brief 调用 GetWifiLossStats 获取 Wi-Fi 丢包统计 */
    bool getWifiLossStats(std::string& result, std::string& errorMsg) {
        if (!isConnected()) return fail("客户端未连接", errorMsg);
        return requestStringData(kMethodGetWifiLossStats, "Wi-Fi 丢包统计", result, errorMsg);
    }

    /** @brief 调用 GetHttpLatencyStats 获取 HTTP 请求延迟统计 */
    bool getHttpLatencyStats(std::string& result, std::string& errorMsg) {
        if (!isConnected()) return fail("客户端未连接", errorMsg);
        return requestStringData(kMethodGetHttpLatencyStats, "HTTP 请求延迟统计", result, errorMsg);
    }

    /** @brief 调用 GetProcessProfiling 获取进程网络画像 */
    bool getProcessProfiling(std::string& result, std::string& errorMsg) {
        if (!isConnected()) return fail("客户端未连接", errorMsg);
        return requestStringData(kMethodGetProcessProfiling, "进程网络画像", result, errorMsg);
    }

    /** @brief 调用 GetEbpfMonitorHealth 获取 eBPF 监控器健康快照 */
    bool getEbpfMonitorHealth(std::string& result, std::string& errorMsg) {
        if (!isConnected()) return fail("客户端未连接", errorMsg);
        return requestStringData(kMethodGetEbpfMonitorHealth, "eBPF 监控器健康状态", result, errorMsg);
    }

    /**
     * @brief 调用 GetHistory 查询历史监控数据
     *
     * D-Bus 调用：
     *   - Method:  GetHistory
     *   - Args:    STRING interface, STRING start, STRING end, INT32 limit
     *   - Returns: STRING（JSON 数组）
     *   - 超时：5000ms
     */
    bool getHistory(const std::string& iface, const std::string& start,
                    const std::string& end, int32_t limit,
                    std::string& result, std::string& errorMsg) {
        if (!isConnected()) return fail("客户端未连接", errorMsg);

        // 构造方法调用：GetHistory
        DBusMessage* msg = dbus_message_new_method_call(kBusName, kObjectPath, kInterface, kMethodGetHistory);
        if (!msg) {
            errorMsg = "创建历史查询消息失败";
            return false;
        }

        // 按顺序追加四个参数：STRING interface, STRING start, STRING end, INT32 limit
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

        // 解析返回值：单个 STRING（JSON）
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
    /** @brief 统一设置错误消息并返回 false 的辅助函数 */
    bool fail(const char* msg, std::string& errorMsg) {
        errorMsg = msg;
        return false;
    }

    /**
     * @brief 通用字符串返回型 D-Bus 方法调用模板
     *
     * 适用于所有"无参数、返回单个 STRING"的 D-Bus 方法。
     * 被 getDnsStats / getWifiLossStats / getHttpLatencyStats 等复用。
     *
     * @param methodName D-Bus 方法名（如 "GetDnsStats"）
     * @param label      日志/错误信息中用于标识此次调用的中文名称
     * @param result     输出：返回的字符串
     * @param errorMsg   输出：失败时的错误描述
     * @return true=调用成功
     */
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
        // 解析返回值：单个 STRING
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

    /**
     * @brief 阻塞订阅蓝牙设备变化信号（BluetoothDeviceChanged）
     *
     * D-Bus 信号：
     *   - Signal:  BluetoothDeviceChanged
     *   - Payload: STRING text + INT32 counter
     *
     * 监听逻辑同 subscribeToChanges()。
     */
    bool subscribeToBluetoothEvents(bool (*callback)(const std::string& message, int32_t counter) = nullptr) {
        if (!isConnected()) return false;

        DBusError err;
        dbus_error_init(&err);
        // 注册 BluetoothDeviceChanged 信号匹配规则
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

            // 解析 BluetoothDeviceChanged 信号的 STRING + INT32 两个参数
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
    /** @brief 获取 D-Bus 消息的信号成员名（辅助函数） */
    std::string getSignalMember(DBusMessage* msg) {
        const char* member = dbus_message_get_member(msg);
        return member ? std::string(member) : "";
    }
};

// ===== 全局单例客户端实例 =====
static WeakNetClient* g_client = nullptr;

// ========== C 接口实现（weaknet_client.h 中声明） ==========

/** @brief 初始化 WeakNet 客户端库，建立 D-Bus Session 连接 */
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

/** @brief 清理 WeakNet 客户端库资源 */
extern "C" void weaknet_cleanup() {
    LOG_INFO(LogModule::CLIENT, "weaknet_cleanup: cleaning up");
    if (g_client) {
        g_client->disconnect();
        delete g_client;
        g_client = nullptr;
    }
    Logger::shutdown();
}

/** @brief C 接口包装：调用 GetInterfaces 方法 */
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

/** @brief C 接口包装：非阻塞检查 Changed 信号 */
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

/** @brief C 接口包装：调用 HealthCheck 方法 */
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

/** @brief C 接口包装：从序列化文件读取最新状态（离线模式，不发起 D-Bus 调用） */
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

/** @brief C 接口包装：调用 Ping 方法 */
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

// ========== C 接口函数实现（weaknet_dbus 命名空间外） ==========

using namespace weaknet_dbus;

/**
 * @brief C 接口包装：订阅指定 D-Bus 事件（只添加 match，不阻塞）
 *
 * @param event_type 信号成员名（如 "InterfaceChanged"）
 * @param callback   事件回调（当前 C 接口只注册 D-Bus 订阅，回调暂未在内部触发）
 */
extern "C" bool weaknet_subscribe_event(const char* event_type, weaknet_event_callback_t callback) {
    if (!weaknet_dbus::g_client || !weaknet_dbus::g_client->isConnected()) {
        LOG_ERROR(LogModule::CLIENT, "weaknet_subscribe_event: client not connected");
        return false;
    }
    LOG_INFO(LogModule::CLIENT, "weaknet_subscribe_event: subscribing to " << event_type);
    return weaknet_dbus::g_client->subscribeToEvent(std::string(event_type));
}

/** @brief C 接口：取消订阅事件（简化实现，当前固定返回 true） */
extern "C" bool weaknet_unsubscribe_event(const char* event_type) {
    // 注意：这个简化实现只是返回成功，实际项目中可能需要更复杂的去订阅逻辑
    // 简化实现，不记录日志
    return true;
}

/** @brief C 接口：获取支持的事件类型列表（本地拼接，不发起 D-Bus 调用） */
extern "C" bool weaknet_get_event_types(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size) {
    snprintf(buffer, buffer_size, "%s,%s,%s,%s",
             weaknet_dbus::kSignalInterfaceChanged,
             weaknet_dbus::kSignalConnectionModeChanged,
             weaknet_dbus::kSignalNetworkQualityChanged,
             weaknet_dbus::kSignalBluetoothDeviceChanged);
    return true;
}

/** @brief C 接口包装：非阻塞检查多类事件信号 */
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

/** @brief C 接口：检查客户端连接状态 */
extern "C" bool weaknet_is_connected() {
    return weaknet_dbus::g_client && weaknet_dbus::g_client->isConnected();
}

/** @brief C 接口：返回硬编码版本字符串 "WeakNet Client Library v1.0.0"（不发起 D-Bus 调用） */
extern "C" bool weaknet_get_version(char* buffer, size_t buffer_size) {
    snprintf(buffer, buffer_size, "WeakNet Client Library v1.0.0");
    return true;
}

/** @brief C 接口：返回编译时间信息（使用 __DATE__ / __TIME__ 宏，不发起 D-Bus 调用） */
extern "C" bool weaknet_get_build_info(char* buffer, size_t buffer_size) {
    snprintf(buffer, buffer_size, "Built: %s %s | DBus-enabled | C++17", __DATE__, __TIME__);
    return true;
}

/**
 * @brief C 接口包装：阻塞订阅 NetworkQualityChanged 信号
 *
 * 内部通过静态变量保存用户回调指针，构造 C++ lambda 包装器
 * 传给 WeakNetClient::subscribeToNetworkQuality()。
 */
extern "C" bool weaknet_subscribe_network_quality(weaknet_network_quality_callback_t callback) {
    if (!weaknet_dbus::g_client || !weaknet_dbus::g_client->isConnected()) {
        LOG_ERROR(LogModule::CLIENT, "weaknet_subscribe_network_quality: client not connected");
        return false;
    }
    LOG_INFO(LogModule::CLIENT, "weaknet_subscribe_network_quality: subscribing");
    
    // 用静态变量保存用户回调指针（注意：这只支持一次订阅，多次调用会覆盖）
    static weaknet_network_quality_callback_t s_callback = nullptr;
    s_callback = callback;
    
    // 构造 C++ lambda，把 std::string 转换回 const char* 后调用用户 C 回调
    auto cpp_callback = [](const std::string& quality, const std::string& details, int32_t counter) -> bool {
        if (s_callback) {
            return s_callback(quality.c_str(), details.c_str(), counter);
        }
        return false;
    };
    
    return weaknet_dbus::g_client->subscribeToNetworkQuality(cpp_callback);
}

/**
 * @brief C 接口包装：非阻塞检查 NetworkQualityChanged 信号
 *
 * 直接访问底层 D-Bus 连接，手动执行 read_write + pop_message + parse，
 * 与 WeakNetClient 类的 checkForEvents 类似但只处理 NetworkQualityChanged 信号。
 */
extern "C" bool weaknet_check_network_quality(char* quality_buffer, size_t quality_size,
                                             char* details_buffer, size_t details_size, 
                                                     int32_t* counter, char* error_buffer, size_t error_size) {
    if (!weaknet_dbus::g_client || !weaknet_dbus::g_client->isConnected()) {
        snprintf(error_buffer, error_size, "客户端未连接");
        return false;
    }

    if (!weaknet_dbus::g_client->isConnected()) return false;

    // 非阻塞轮询 D-Bus 队列
    dbus_connection_read_write(weaknet_dbus::g_client->getConnection(), 0);
    DBusMessage* msg = dbus_connection_pop_message(weaknet_dbus::g_client->getConnection());
    if (!msg) return false;

    // 检查是否为 NetworkQualityChanged 信号，并解析其三个参数
    if (dbus_message_is_signal(msg, weaknet_dbus::kInterface, weaknet_dbus::kSignalNetworkQualityChanged)) {
        const char* quality = nullptr;
        const char* details = nullptr;
        DBusError e;
        dbus_error_init(&e);
        
        // 解析：STRING quality + STRING details + INT32 counter
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

// ============== 蓝牙设备 C API 实现 ==============

/** @brief C 接口包装：调用 GetBluetoothDevices 方法 */
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

/** @brief C 接口包装：调用 GetBluetoothAdapter 方法 */
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

/**
 * @brief C 接口包装：订阅 BluetoothDeviceChanged 信号
 *
 * 当前实现只添加 D-Bus match 规则，不进入阻塞监听循环。
 * 实际事件需通过 weaknet_check_events() 轮询。
 */
extern "C" bool weaknet_subscribe_bluetooth_events(weaknet_event_callback_t callback) {
    if (!weaknet_dbus::g_client || !weaknet_dbus::g_client->isConnected()) {
        return false;
    }
    // 先添加 D-Bus 信号订阅
    weaknet_dbus::g_client->subscribeToEvent(weaknet_dbus::kSignalBluetoothDeviceChanged);
    return true;
}

// ============== eBPF 监控数据 C API 实现 ==============

/** @brief C 接口包装：调用 GetDnsStats 方法 */
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

/** @brief C 接口包装：调用 GetWifiLossStats 方法 */
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

/** @brief C 接口包装：调用 GetHttpLatencyStats 方法 */
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

/** @brief C 接口包装：调用 GetProcessProfiling 方法 */
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

/** @brief C 接口包装：调用 GetEbpfMonitorHealth 方法 */
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

/** @brief C 接口包装：调用 GetHistory 方法查询历史监控数据 */
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

/**
 * @file bt_monitor.cpp
 * @brief 蓝牙监测器实现 — 通过 BlueZ D-Bus 系统总线实时监控蓝牙适配器、设备及音频传输状态
 *
 * 模块职责：
 *   - 自动发现并绑定本机第一个蓝牙适配器（hci0 等）
 *   - 周期性扫描周围蓝牙设备，维护设备列表与 RSSI 历史
 *   - 实时追踪蓝牙连接/断开/RSSI 变化等事件，通过 EventManager 发射信号
 *   - 监测 A2DP 音频传输状态（依赖 org.bluez.MediaTransport1 接口）
 *   - 基于 RSSI 的路径损耗模型估算设备距离
 *   - Phase 2：集成 BtAudioAnalyzer（eBPF 内核态流量统计）与 BtAudioFusion（融合评分）
 *
 * 依赖的外部接口：
 *   - **BlueZ D-Bus 系统总线** (org.bluez)
 *     · org.bluez.Adapter1        适配器电源管理、扫描控制
 *     · org.bluez.Device1         设备属性（RSSI、Connected、UUIDs、Appearance 等）
 *     · org.bluez.MediaTransport1 A2DP 音频传输状态（State/Delay/Volume/Codec）
 *     · org.freedesktop.DBus.Properties      属性 Get/GetAll/Set
 *     · org.freedesktop.DBus.ObjectManager  GetManagedObjects 枚举
 *   - **libdbus** (dbus/dbus.h)    D-Bus 低级 C API，作为 GLib D-Bus 的替代以减少依赖
 *   - **bt_monitor.hpp**           定义 BtMonitor 类、BtDeviceInfo、BtEvent、BtAdapterState
 *   - **bt_audio_analyzer.hpp**    Phase 2 eBPF 流量分析器
 *   - **bt_audio_fusion.hpp**      Phase 2 音频质量融合评分器
 *
 * 设计思路：
 *   - 使用线程安全的成员（std::mutex + std::lock_guard）保护设备列表、音频传输列表、事件队列
 *   - 主循环在独立线程（start_bt_monitor_thread）中以 3 秒间隔调用 refreshAdapterState + refreshDeviceStates
 *   - 扫描自动续期（DISCOVERY_TIMEOUT_SEC=30s），避免 BlueZ 自动停止扫描
 *   - 事件通过 pendingEvents_ 队列 + fetchEvents() 线程安全地转交给上层
 *   - Phase 2 eBPF 挂载失败时自动降级为纯 D-Bus 模式，不影响基础功能
 *
 * 协作关系：
 *   - 与 EventManager 协作：蓝牙事件统一通过 emitBluetoothDeviceChanged 发射
 *   - 与 BtAudioAnalyzer 协作：Phase 2 中调用其 setSessionActive/getStats 获取内核态流量数据
 *   - 与 BtAudioFusion 协作：将 D-Bus 音频传输状态 + eBPF 流量统计融合为单一质量评分
 */

#include "bt_monitor.hpp"
#include "server.hpp"
#include "dbus_service.hpp"
#include "event_manager.hpp"
#include "bt_audio_analyzer.hpp"
#include "bt_audio_fusion.hpp"
#include "logger.hpp"
#include "common.hpp"

#include <dbus/dbus.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <thread>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <set>

using namespace std::chrono_literals;

namespace weaknet_dbus {

// ============================================================================
// BlueZ D-Bus 常量 (在系统总线上)
// ============================================================================
static constexpr const char* BLUEZ_SERVICE   = "org.bluez";
static constexpr const char* BLUEZ_ADAPTER_IFACE = "org.bluez.Adapter1";
static constexpr const char* BLUEZ_DEVICE_IFACE  = "org.bluez.Device1";
static constexpr const char* BLUEZ_MEDIA_TRANSPORT_IFACE = "org.bluez.MediaTransport1";
static constexpr const char* DBUS_PROPS_IFACE = "org.freedesktop.DBus.Properties";
static constexpr const char* DBUS_OBJMGR_IFACE = "org.freedesktop.DBus.ObjectManager";

// ============================================================================
// D-Bus variant 迭代器辅助函数
// ============================================================================

/** @brief 从 D-Bus 迭代器递归提取 variant 容器内的 int16 值 */
static bool extractInt16FromIter(DBusMessageIter* iter, int16_t* out) {
    int type = dbus_message_iter_get_arg_type(iter);
    if (type == DBUS_TYPE_INT16) {
        dbus_message_iter_get_basic(iter, out);
        return true;
    }
    if (type == DBUS_TYPE_VARIANT) {
        DBusMessageIter sub;
        dbus_message_iter_recurse(iter, &sub);
        return extractInt16FromIter(&sub, out);
    }
    return false;
}

/** @brief 从 D-Bus 迭代器递归提取 variant 容器内的 bool 值 */
static bool extractBoolFromIter(DBusMessageIter* iter, bool* out) {
    int type = dbus_message_iter_get_arg_type(iter);
    if (type == DBUS_TYPE_BOOLEAN) {
        dbus_bool_t v = FALSE;
        dbus_message_iter_get_basic(iter, &v);
        *out = (v != FALSE);
        return true;
    }
    if (type == DBUS_TYPE_VARIANT) {
        DBusMessageIter sub;
        dbus_message_iter_recurse(iter, &sub);
        return extractBoolFromIter(&sub, out);
    }
    return false;
}

/** @brief 从 D-Bus 迭代器递归提取 variant 容器内的 string 值 */
static bool extractStringFromIter(DBusMessageIter* iter, std::string* out) {
    int type = dbus_message_iter_get_arg_type(iter);
    if (type == DBUS_TYPE_STRING) {
        const char* s = nullptr;
        dbus_message_iter_get_basic(iter, &s);
        if (s) *out = s;
        return s != nullptr;
    }
    if (type == DBUS_TYPE_VARIANT) {
        DBusMessageIter sub;
        dbus_message_iter_recurse(iter, &sub);
        return extractStringFromIter(&sub, out);
    }
    return false;
}

/** @brief 从 D-Bus 迭代器递归提取 variant 容器内的 int32 值 */
static bool extractInt32FromIter(DBusMessageIter* iter, int32_t* out) {
    int type = dbus_message_iter_get_arg_type(iter);
    if (type == DBUS_TYPE_INT32) {
        dbus_message_iter_get_basic(iter, out);
        return true;
    }
    if (type == DBUS_TYPE_VARIANT) {
        DBusMessageIter sub;
        dbus_message_iter_recurse(iter, &sub);
        return extractInt32FromIter(&sub, out);
    }
    return false;
}

/** @brief 从 D-Bus 迭代器递归提取 variant 容器内的 uint16 值 */
static bool extractUint16FromIter(DBusMessageIter* iter, uint16_t* out) {
    int type = dbus_message_iter_get_arg_type(iter);
    if (type == DBUS_TYPE_UINT16) {
        dbus_message_iter_get_basic(iter, out);
        return true;
    }
    if (type == DBUS_TYPE_VARIANT) {
        DBusMessageIter sub;
        dbus_message_iter_recurse(iter, &sub);
        return extractUint16FromIter(&sub, out);
    }
    return false;
}

/** @brief 从 D-Bus 迭代器递归提取 variant 容器内的字符串数组 */
static bool extractStringArrayFromIter(DBusMessageIter* iter, std::vector<std::string>* out) {
    int type = dbus_message_iter_get_arg_type(iter);
    if (type == DBUS_TYPE_VARIANT) {
        DBusMessageIter sub;
        dbus_message_iter_recurse(iter, &sub);
        return extractStringArrayFromIter(&sub, out);
    }
    if (type == DBUS_TYPE_ARRAY) {
        DBusMessageIter arr;
        dbus_message_iter_recurse(iter, &arr);
        while (dbus_message_iter_get_arg_type(&arr) != DBUS_TYPE_INVALID) {
            if (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
                const char* s = nullptr;
                dbus_message_iter_get_basic(&arr, &s);
                if (s) out->push_back(s);
            }
            dbus_message_iter_next(&arr);
        }
        return true;
    }
    return false;
}

/** @brief MAC 地址比较（忽略大小写） */
static bool macEquals(const std::string& a, const std::string& b) {
    if (a.length() != b.length()) return false;
    for (size_t i = 0; i < a.length(); ++i) {
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    }
    return true;
}

// ============================================================================
// BtMonitor 构造与析构
// ============================================================================

BtMonitor::BtMonitor() {
    btAudioAnalyzer_ = std::make_unique<BtAudioAnalyzer>();
    LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: instance created");
}

BtMonitor::~BtMonitor() {
    cleanup();
}

/**
 * @brief 初始化蓝牙监测器：连接 D-Bus 系统总线，验证 BlueZ 服务，发现第一个适配器
 * @return true 初始化成功（或 BlueZ 不可用但已记录状态）；false 连接/服务检查失败
 * @note 无蓝牙适配器是正常场景（返回 false 但不是错误），调用方可周期性重试
 */
bool BtMonitor::initialize() {
    if (initialized_.load()) {
        LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: already initialized");
        return true;
    }

    LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: initializing - connecting to D-Bus system bus...");

    DBusError err;
    dbus_error_init(&err);

    // BlueZ 运行在系统总线，而非会话总线
    sysConn_ = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (dbus_error_is_set(&err)) {
        LOG_ERROR(LogModule::BLUETOOTH, "BtMonitor: failed to connect to system bus: " << err.message);
        dbus_error_free(&err);
        return false;
    }
    if (!sysConn_) {
        LOG_ERROR(LogModule::BLUETOOTH, "BtMonitor: system bus connection is null");
        return false;
    }

    // 检查 BlueZ 服务是否可用
    // 先尝试通过 NameHasOwner 检查
    DBusMessage* msg = dbus_message_new_method_call(
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "NameHasOwner");
    if (!msg) {
        LOG_ERROR(LogModule::BLUETOOTH, "BtMonitor: failed to create NameHasOwner message");
        return false;
    }
    const char* bluezName = BLUEZ_SERVICE;
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &bluezName, DBUS_TYPE_INVALID);

    DBusMessage* reply = sendWithReply(sysConn_, msg, 2000);
    dbus_message_unref(msg);

    if (!reply) {
        LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: BlueZ service is not available (no reply). "
                 "This is normal if no Bluetooth adapter is present.");
        // 没有蓝牙适配器不是错误，初始化为不可用状态
        dbus_connection_unref(sysConn_);
        sysConn_ = nullptr;
        return false;
    }

    dbus_bool_t hasOwner = FALSE;
    if (dbus_message_get_args(reply, nullptr, DBUS_TYPE_BOOLEAN, &hasOwner, DBUS_TYPE_INVALID)) {
        dbus_message_unref(reply);
        if (!hasOwner) {
            LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: BlueZ service not registered on system bus");
            dbus_connection_unref(sysConn_);
            sysConn_ = nullptr;
            return false;
        }
    } else {
        dbus_message_unref(reply);
        dbus_connection_unref(sysConn_);
        sysConn_ = nullptr;
        return false;
    }

    LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: BlueZ service found, probing adapter...");

    // 查找第一个蓝牙适配器 (通常为 hci0)
    // 通过 ObjectManager 获取 /org/bluez 下的所有对象
    msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, "/",
        DBUS_OBJMGR_IFACE, "GetManagedObjects");
    reply = sendWithReply(sysConn_, msg, 3000);
    dbus_message_unref(msg);

    if (!reply) {
        LOG_ERROR(LogModule::BLUETOOTH, "BtMonitor: failed to get managed objects from BlueZ");
        dbus_connection_unref(sysConn_);
        sysConn_ = nullptr;
        return false;
    }

    // 解析 GetManagedObjects 返回的字典，找到第一个 Adapter1 对象
    DBusMessageIter rootIter;
    if (!dbus_message_iter_init(reply, &rootIter)) {
        LOG_ERROR(LogModule::BLUETOOTH, "BtMonitor: empty reply from GetManagedObjects");
        dbus_message_unref(reply);
        dbus_connection_unref(sysConn_);
        sysConn_ = nullptr;
        return false;
    }

    // GetManagedObjects 返回: a{oa{sa{sv}}} — 字典: object_path → 字典: iface_name → 字典: prop_name → variant(value)
    if (dbus_message_iter_get_arg_type(&rootIter) == DBUS_TYPE_ARRAY) {
        DBusMessageIter objArray;
        dbus_message_iter_recurse(&rootIter, &objArray);

        while (dbus_message_iter_get_arg_type(&objArray) != DBUS_TYPE_INVALID) {
            // 每个元素: {oa{sa{sv}}} — 字典条目: object_path + ifaces_dict
            DBusMessageIter objEntry;
            dbus_message_iter_recurse(&objArray, &objEntry);

            // 第一个字段: object_path (string)
            const char* objPathC = nullptr;
            dbus_message_iter_get_basic(&objEntry, &objPathC);
            std::string objPath = objPathC ? objPathC : "";
            dbus_message_iter_next(&objEntry);

            // 第二个字段: a{sa{sv}} — ifaces 字典数组
            if (dbus_message_iter_get_arg_type(&objEntry) == DBUS_TYPE_ARRAY) {
                DBusMessageIter ifaceArray;
                dbus_message_iter_recurse(&objEntry, &ifaceArray);

                while (dbus_message_iter_get_arg_type(&ifaceArray) != DBUS_TYPE_INVALID) {
                    DBusMessageIter ifaceEntry;
                    dbus_message_iter_recurse(&ifaceArray, &ifaceEntry);

                    const char* ifaceName = nullptr;
                    dbus_message_iter_get_basic(&ifaceEntry, &ifaceName);

                    // 找到 Adapter1 接口的对象路径
                    if (ifaceName && std::string(ifaceName) == BLUEZ_ADAPTER_IFACE) {
                        adapterPath_ = objPath;
                        LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: found adapter at " << adapterPath_);
                        break;
                    }

                    dbus_message_iter_next(&ifaceArray);
                }
            }

            if (!adapterPath_.empty()) break;
            dbus_message_iter_next(&objArray);
        }
    }
    dbus_message_unref(reply);

    if (adapterPath_.empty()) {
        LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: no Bluetooth adapter found");
        dbus_connection_unref(sysConn_);
        sysConn_ = nullptr;
        return false;
    }

    // 刷新适配器状态
    refreshAdapterState();

    // 添加 PropertiesChanged 信号匹配规则，用于实时接收设备属性变化
    {
        std::string matchRule = std::string("type='signal',sender='")
            + BLUEZ_SERVICE + "',interface='" + DBUS_PROPS_IFACE
            + "',member='PropertiesChanged'";
        dbus_bus_add_match(sysConn_, matchRule.c_str(), &err);
        if (dbus_error_is_set(&err)) {
            LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: warning - failed to add signal match: " << err.message);
            dbus_error_free(&err);
            // 非致命，继续
        }
    }

    initialized_.store(true);
    LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: initialization complete. Adapter: "
             << adapterState_.name << " (" << adapterState_.macAddress << ") "
             << (adapterState_.powered ? "POWERED" : "OFF"));

    return true;
}

/**
 * @brief 释放资源：停止扫描、断开 D-Bus 连接、停止工作线程
 */
void BtMonitor::cleanup() {
    if (!initialized_.load()) return;
    running_.store(false);

    if (workerThread_ && workerThread_->joinable()) {
        workerThread_->join();
    }
    workerThread_.reset();

    if (sysConn_) {
        if (!adapterPath_.empty()) {
            stopDiscovery();
        }
        dbus_connection_unref(sysConn_);
        sysConn_ = nullptr;
    }

    initialized_.store(false);
    LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: cleaned up");
}

// ============================================================================
// 适配器状态刷新
// ============================================================================

/**
 * @brief 从 BlueZ Adapter1 接口拉取适配器当前状态（MAC/名称/电源/扫描中 等）
 * @return true 刷新成功
 */
bool BtMonitor::refreshAdapterState() {
    if (adapterPath_.empty() || !sysConn_) return false;

    std::lock_guard<std::mutex> lock(adapterMutex_);

    adapterState_.macAddress = getStringProperty(sysConn_, adapterPath_, BLUEZ_ADAPTER_IFACE, "Address");
    adapterState_.name      = getStringProperty(sysConn_, adapterPath_, BLUEZ_ADAPTER_IFACE, "Name");
    adapterState_.alias     = getStringProperty(sysConn_, adapterPath_, BLUEZ_ADAPTER_IFACE, "Alias");
    adapterState_.powered   = getBoolProperty(sysConn_, adapterPath_, BLUEZ_ADAPTER_IFACE, "Powered");
    adapterState_.discovering = getBoolProperty(sysConn_, adapterPath_, BLUEZ_ADAPTER_IFACE, "Discovering");
    adapterState_.discoverable = getBoolProperty(sysConn_, adapterPath_, BLUEZ_ADAPTER_IFACE, "Discoverable");
    adapterState_.pairable  = getBoolProperty(sysConn_, adapterPath_, BLUEZ_ADAPTER_IFACE, "Pairable");

    return true;
}

/** @brief 检查是否存在可用的已上电蓝牙适配器 */
bool BtMonitor::hasAdapter() const {
    std::lock_guard<std::mutex> lock(adapterMutex_);
    return !adapterPath_.empty() && adapterState_.powered;
}

/** @brief 获取当前适配器状态快照（线程安全拷贝） */
BtAdapterState BtMonitor::getAdapterState() const {
    std::lock_guard<std::mutex> lock(adapterMutex_);
    return adapterState_;
}

// ============================================================================
// 发现控制
// ============================================================================

/** @brief 调用 BlueZ StartDiscovery 开始扫描，并投递 DiscoveryStarted 事件 */
bool BtMonitor::startDiscovery() {
    if (!sysConn_ || adapterPath_.empty()) return false;
    if (adapterState_.discovering) return true;  // 已经在扫描

    bool ok = callBlueZMethod(adapterPath_, BLUEZ_ADAPTER_IFACE, "StartDiscovery");
    if (ok) {
        lastDiscoveryStart_ = std::chrono::system_clock::now();
        adapterState_.discovering = true;

        BtEvent ev;
        ev.type = BtEvent::Type::DiscoveryStarted;
        ev.adapterMac = adapterState_.macAddress;
        ev.message = "Bluetooth device discovery started";
        ev.timestamp = std::chrono::system_clock::now();
        {
            std::lock_guard<std::mutex> lock(eventMutex_);
            pendingEvents_.push_back(ev);
        }
        LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: discovery started");
    }
    return ok;
}

/** @brief 调用 BlueZ StopDiscovery 停止扫描，并投递 DiscoveryStopped 事件 */
bool BtMonitor::stopDiscovery() {
    if (!sysConn_ || adapterPath_.empty()) return false;
    if (!adapterState_.discovering) return true;  // 已经停止

    bool ok = callBlueZMethod(adapterPath_, BLUEZ_ADAPTER_IFACE, "StopDiscovery");
    if (ok) {
        adapterState_.discovering = false;

        BtEvent ev;
        ev.type = BtEvent::Type::DiscoveryStopped;
        ev.adapterMac = adapterState_.macAddress;
        ev.message = "Bluetooth device discovery stopped";
        ev.timestamp = std::chrono::system_clock::now();
        {
            std::lock_guard<std::mutex> lock(eventMutex_);
            pendingEvents_.push_back(ev);
        }
        LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: discovery stopped");
    }
    return ok;
}

/** @brief 通过 DBus Properties.Set 设置适配器 Powered 属性（开/关蓝牙） */
bool BtMonitor::setPowered(bool on) {
    if (!sysConn_ || adapterPath_.empty()) return false;

    DBusMessage* msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, adapterPath_.c_str(),
        DBUS_PROPS_IFACE, "Set");
    if (!msg) return false;

    const char* iface = BLUEZ_ADAPTER_IFACE;
    const char* prop = "Powered";
    DBusMessageIter iter;
    dbus_message_iter_init_append(msg, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &prop);

    DBusMessageIter variant;
    dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT,
        DBUS_TYPE_BOOLEAN_AS_STRING, &variant);
    dbus_bool_t val = on ? TRUE : FALSE;
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &val);
    dbus_message_iter_close_container(&iter, &variant);

    DBusMessage* reply = sendWithReply(sysConn_, msg, 2000);
    dbus_message_unref(msg);
    if (reply) {
        dbus_message_unref(reply);
        adapterState_.powered = on;
        LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: adapter power set to " << (on ? "ON" : "OFF"));
        return true;
    }
    return false;
}

// ============================================================================
// 设备属性解析
// ============================================================================

/** @brief 列出当前适配器下所有 Device1 D-Bus 对象路径（/org/bluez/hci0/dev_XX_XX_XX_XX_XX_XX） */
std::vector<std::string> BtMonitor::listDevicePaths(DBusConnection* conn) {
    std::vector<std::string> paths;
    if (!conn || adapterPath_.empty()) return paths;

    DBusMessage* msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, "/",
        DBUS_OBJMGR_IFACE, "GetManagedObjects");
    DBusMessage* reply = sendWithReply(conn, msg, 3000);
    dbus_message_unref(msg);
    if (!reply) return paths;

    DBusMessageIter root;
    if (!dbus_message_iter_init(reply, &root)) {
        dbus_message_unref(reply);
        return paths;
    }

    if (dbus_message_iter_get_arg_type(&root) == DBUS_TYPE_ARRAY) {
        DBusMessageIter objArray;
        dbus_message_iter_recurse(&root, &objArray);

        while (dbus_message_iter_get_arg_type(&objArray) != DBUS_TYPE_INVALID) {
            DBusMessageIter objEntry;
            dbus_message_iter_recurse(&objArray, &objEntry);

            const char* objPath = nullptr;
            dbus_message_iter_get_basic(&objEntry, &objPath);
            std::string pathStr = objPath ? objPath : "";
            dbus_message_iter_next(&objEntry);

            // 检查是否在当前适配器路径下
            if (pathStr.find(adapterPath_ + "/dev_") != 0) {
                dbus_message_iter_next(&objArray);
                continue;
            }

            if (dbus_message_iter_get_arg_type(&objEntry) == DBUS_TYPE_ARRAY) {
                DBusMessageIter ifaceArray;
                dbus_message_iter_recurse(&objEntry, &ifaceArray);

                while (dbus_message_iter_get_arg_type(&ifaceArray) != DBUS_TYPE_INVALID) {
                    DBusMessageIter ifaceEntry;
                    dbus_message_iter_recurse(&ifaceArray, &ifaceEntry);
                    const char* ifaceName = nullptr;
                    dbus_message_iter_get_basic(&ifaceEntry, &ifaceName);
                    if (ifaceName && std::string(ifaceName) == BLUEZ_DEVICE_IFACE) {
                        paths.push_back(pathStr);
                        break;
                    }
                    dbus_message_iter_next(&ifaceArray);
                }
            }
            dbus_message_iter_next(&objArray);
        }
    }
    dbus_message_unref(reply);
    return paths;
}

/**
 * @brief 从 Device1 对象的 D-Bus 路径解析出完整 BtDeviceInfo（含 MAC/RSSI/连接状态/UUIDs/Appearance 等）
 * @param conn D-Bus 系统总线连接
 * @param devPath 设备 D-Bus 对象路径
 * @return 填充好的 BtDeviceInfo；失败时 MAC 为空
 */
BtDeviceInfo BtMonitor::parseDeviceProperties(DBusConnection* conn,
                                                         const std::string& devPath) {
    BtDeviceInfo info;

    // 解析 MAC 地址 (从路径提取: /org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF)
    auto pos = devPath.rfind("dev_");
    if (pos != std::string::npos) {
        std::string mac = devPath.substr(pos + 4);
        // 将下划线替换回冒号
        for (auto& c : mac) {
            if (c == '_') c = ':';
        }
        info.macAddress = mac;
    }

    // 通过 GetAll 一次性获取所有属性 (而非逐个 Get)
    DBusMessage* msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, devPath.c_str(),
        DBUS_PROPS_IFACE, "GetAll");
    const char* deviceIface = BLUEZ_DEVICE_IFACE;
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &deviceIface, DBUS_TYPE_INVALID);

    DBusMessage* reply = sendWithReply(conn, msg, 3000);
    dbus_message_unref(msg);
    if (!reply) return info;

    // GetAll 返回 a{sv} — 字典: prop_name → variant(value)
    DBusMessageIter root;
    if (!dbus_message_iter_init(reply, &root)) {
        dbus_message_unref(reply);
        return info;
    }

    if (dbus_message_iter_get_arg_type(&root) == DBUS_TYPE_ARRAY) {
        DBusMessageIter propsArray;
        dbus_message_iter_recurse(&root, &propsArray);

        while (dbus_message_iter_get_arg_type(&propsArray) != DBUS_TYPE_INVALID) {
            DBusMessageIter propEntry;
            dbus_message_iter_recurse(&propsArray, &propEntry);

            const char* propName = nullptr;
            dbus_message_iter_get_basic(&propEntry, &propName);
            std::string key = propName ? propName : "";
            dbus_message_iter_next(&propEntry);  // 移到 variant value

            if (key == "Name") {
                extractStringFromIter(&propEntry, &info.name);
            } else if (key == "Alias") {
                extractStringFromIter(&propEntry, &info.alias);
            } else if (key == "RSSI") {
                extractInt16FromIter(&propEntry, &info.rssiDbm);
            } else if (key == "TxPower") {
                extractInt16FromIter(&propEntry, &info.txPower);
            } else if (key == "Connected") {
                extractBoolFromIter(&propEntry, &info.connected);
            } else if (key == "Paired") {
                extractBoolFromIter(&propEntry, &info.paired);
            } else if (key == "Trusted") {
                extractBoolFromIter(&propEntry, &info.trusted);
            } else if (key == "Blocked") {
                extractBoolFromIter(&propEntry, &info.blocked);
            } else if (key == "LegacyPairing") {
                extractBoolFromIter(&propEntry, &info.legacyPairing);
            } else if (key == "Appearance") {
                extractUint16FromIter(&propEntry, &info.appearance);
            } else if (key == "UUIDs") {
                extractStringArrayFromIter(&propEntry, &info.uuids);
            } else if (key == "Icon") {
                extractStringFromIter(&propEntry, &info.icon);
            }

            dbus_message_iter_next(&propsArray);
        }
    }
    dbus_message_unref(reply);

    // 推断设备类型 (基于 UUIDs 和 Appearance)
    // BLE 设备通常有特定的 GATT 服务 UUID
    bool hasClassicUuid = false, hasBleUuid = false;
    for (const auto& uuid : info.uuids) {
        if (uuid.find("1800") != std::string::npos ||  // Generic Access (BLE)
            uuid.find("1801") != std::string::npos ||  // Generic Attribute (BLE)
            uuid.find("0x18") != std::string::npos) {
            hasBleUuid = true;
        }
        if (uuid.find("1101") != std::string::npos ||  // Serial Port (Classic)
            uuid.find("110b") != std::string::npos ||  // Audio Sink (Classic)
            uuid.find("110c") != std::string::npos) { // AVRCP (Classic)
            hasClassicUuid = true;
        }
    }
    if (hasClassicUuid && hasBleUuid) {
        info.deviceType = BtDeviceType::Dual;
    } else if (hasBleUuid) {
        info.deviceType = BtDeviceType::BLE;
    } else if (hasClassicUuid) {
        info.deviceType = BtDeviceType::Classic;
    }

    info.lastUpdated = std::chrono::system_clock::now();
    return info;
}

// ============================================================================
// 设备状态刷新 (核心轮询逻辑)
// ============================================================================

/**
 * @brief 核心轮询函数：枚举所有已知设备、更新 RSSI/连接状态/距离估算、检测新设备/离站
 * @note 同时自动续期扫描（>30s 未启动扫描时重启），并调用 refreshAudioTransports() 刷新 A2DP 状态
 */
void BtMonitor::refreshDeviceStates() {
    if (!sysConn_ || adapterPath_.empty()) return;

    // 确保扫描在运行 (如果适配器开启且未扫描)
    if (adapterState_.powered && !adapterState_.discovering) {
        // 检查扫描是否过期 (超过 30 秒自动重启)
        auto now = std::chrono::system_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - lastDiscoveryStart_).count();
        if (elapsed > DISCOVERY_TIMEOUT_SEC || lastDiscoveryStart_.time_since_epoch().count() == 0) {
            startDiscovery();
        }
    }

    // 获取当前所有设备路径
    auto devicePaths = listDevicePaths(sysConn_);
    std::vector<std::string> currentMacs;

    for (const auto& devPath : devicePaths) {
        BtDeviceInfo info = parseDeviceProperties(sysConn_, devPath);
        if (info.macAddress.empty()) continue;

        currentMacs.push_back(info.macAddress);
        auto now = std::chrono::system_clock::now();

        {
            std::lock_guard<std::mutex> lock(deviceMutex_);
            auto it = devices_.find(info.macAddress);

            if (it == devices_.end()) {
                // ---- 新设备 ----
                info.lastSeen = now;
                info.lastUpdated = now;
                if (info.rssiDbm != 0 && info.rssiDbm > -1000) {
                    info.rssiHistory.push_back(info.rssiDbm);
                }
                devices_[info.macAddress] = info;

                // 生成事件
                BtEvent ev;
                ev.type = BtEvent::Type::DeviceFound;
                ev.adapterMac = adapterState_.macAddress;
                ev.deviceMac = info.macAddress;
                ev.deviceName = info.name.empty() ? info.alias : info.name;
                ev.rssiDbm = info.rssiDbm;
                ev.message = "New device found: " + ev.deviceName +
                             " (" + info.macAddress + ")" +
                             (info.rssiDbm != 0 ? " RSSI:" + std::to_string(info.rssiDbm) + "dBm" : "");
                ev.timestamp = now;
                {
                    std::lock_guard<std::mutex> evLock(eventMutex_);
                    pendingEvents_.push_back(ev);
                }
                LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: " << ev.message);

            } else {
                // ---- 已有设备，更新 ----
                auto& existing = it->second;
                existing.lastSeen = now;

                // 检测连接状态变化
                if (info.connected != existing.connected) {
                    BtEvent ev;
                    ev.adapterMac = adapterState_.macAddress;
                    ev.deviceMac = info.macAddress;
                    ev.deviceName = existing.name.empty() ? existing.alias : existing.name;
                    ev.timestamp = now;

                    if (info.connected) {
                        ev.type = BtEvent::Type::DeviceConnected;
                        ev.message = "Device connected: " + ev.deviceName + " (" + info.macAddress + ")";
                    } else {
                        ev.type = BtEvent::Type::DeviceDisconnected;
                        ev.message = "Device disconnected: " + ev.deviceName + " (" + info.macAddress + ")";
                    }
                    {
                        std::lock_guard<std::mutex> evLock(eventMutex_);
                        pendingEvents_.push_back(ev);
                    }
                    LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: " << ev.message);
                }

                // 检测 RSSI 显著变化 (>6dBm)
                bool rssiChanged = false;
                if (info.rssiDbm != 0 && info.rssiDbm > -1000) {
                    if (existing.rssiDbm != info.rssiDbm) {
                        // 与历史平均值比较而非单次值
                        int16_t prevAvg = existing.averageRssi();
                        if (prevAvg == 0 || std::abs(info.rssiDbm - prevAvg) > 6) {
                            rssiChanged = true;
                        }
                    }
                }

                // 更新字段
                existing.name = info.name.empty() ? existing.name : info.name;
                existing.alias = info.alias.empty() ? existing.alias : info.alias;
                existing.rssiDbm = (info.rssiDbm != 0) ? info.rssiDbm : existing.rssiDbm;
                existing.txPower = (info.txPower != 0) ? info.txPower : existing.txPower;
                existing.connected = info.connected;
                existing.paired = info.paired;
                existing.trusted = info.trusted;
                existing.blocked = info.blocked;
                existing.lastUpdated = now;

                // 更新 RSSI 历史
                if (info.rssiDbm != 0 && info.rssiDbm > -1000) {
                    existing.rssiHistory.push_back(info.rssiDbm);
                    if (existing.rssiHistory.size() > BtDeviceInfo::MAX_RSSI_HISTORY) {
                        existing.rssiHistory.erase(existing.rssiHistory.begin());
                    }
                }

                if (rssiChanged) {
                    BtEvent ev;
                    ev.type = BtEvent::Type::DeviceRssiChanged;
                    ev.adapterMac = adapterState_.macAddress;
                    ev.deviceMac = existing.macAddress;
                    ev.deviceName = existing.name.empty() ? existing.alias : existing.name;
                    ev.rssiDbm = existing.rssiDbm;
                    int16_t avg = existing.averageRssi();
                    ev.message = "RSSI changed: " + ev.deviceName +
                                 " now " + std::to_string(existing.rssiDbm) + "dBm" +
                                 " (avg " + std::to_string(avg) + "dBm, " +
                                 existing.rssiLevel() + ")";
                    ev.timestamp = now;
                    {
                        std::lock_guard<std::mutex> evLock(eventMutex_);
                        pendingEvents_.push_back(ev);
                    }
                }
            }
        }
    }

    // 检测设备离开 (30 秒未见到)
    {
        std::lock_guard<std::mutex> lock(deviceMutex_);
        auto now = std::chrono::system_clock::now();
        for (auto it = devices_.begin(); it != devices_.end(); ) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - it->second.lastSeen).count();
            if (elapsed > 30 && !it->second.connected) {
                // 设备离开
                BtEvent ev;
                ev.type = BtEvent::Type::DeviceLost;
                ev.adapterMac = adapterState_.macAddress;
                ev.deviceMac = it->second.macAddress;
                ev.deviceName = it->second.name.empty() ? it->second.alias : it->second.name;
                ev.message = "Device lost: " + ev.deviceName + " (" + it->second.macAddress + ")";
                ev.timestamp = now;
                {
                    std::lock_guard<std::mutex> evLock(eventMutex_);
                    pendingEvents_.push_back(ev);
                }
                LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: " << ev.message);
                it = devices_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // ---- Phase 1b: 更新设备距离估算 ----
    {
        std::lock_guard<std::mutex> lock(deviceMutex_);
        auto now = std::chrono::system_clock::now();
        for (auto& pair : devices_) {
            auto& info = pair.second;
            if (info.rssiDbm != 0 && info.rssiDbm > -1000) {
                double prevDist = info.estimatedDistance;
                info.estimatedDistance = estimateDistance(info.rssiDbm);
                if (prevDist >= 0.0 && std::abs(info.estimatedDistance - prevDist) > 1.0) {
                    BtEvent ev;
                    ev.type = BtEvent::Type::DeviceRssiChanged;
                    ev.adapterMac = adapterState_.macAddress;
                    ev.deviceMac = info.macAddress;
                    ev.deviceName = info.name.empty() ? info.alias : info.name;
                    std::ostringstream oss;
                    oss << info.name << " distance ~" << static_cast<int>(info.estimatedDistance) << "m";
                    ev.message = oss.str();
                    ev.timestamp = now;
                    {
                        std::lock_guard<std::mutex> evLock(eventMutex_);
                        pendingEvents_.push_back(ev);
                    }
                }
            }
        }
    }

    // ---- Phase 1b: 刷新 A2DP 音频传输状态 ----
    refreshAudioTransports();
}

// ============================================================================
// 设备查询 API (线程安全)
// ============================================================================

/** @brief 获取所有已知设备的拷贝列表 */
std::vector<BtDeviceInfo> BtMonitor::getDevices() const {
    std::lock_guard<std::mutex> lock(deviceMutex_);
    std::vector<BtDeviceInfo> result;
    result.reserve(devices_.size());
    for (const auto& [_, info] : devices_) {
        result.push_back(info);
    }
    return result;
}

/** @brief 按 MAC 地址查找设备 */
bool BtMonitor::getDevice(const std::string& mac, BtDeviceInfo* out) const {
    std::lock_guard<std::mutex> lock(deviceMutex_);
    for (const auto& [addr, info] : devices_) {
        if (macEquals(addr, mac)) {
            if (out) *out = info;
            return true;
        }
    }
    return false;
}

/** @brief 获取所有处于 Connected 状态的设备 */
std::vector<BtDeviceInfo> BtMonitor::getConnectedDevices() const {
    std::lock_guard<std::mutex> lock(deviceMutex_);
    std::vector<BtDeviceInfo> result;
    for (const auto& [_, info] : devices_) {
        if (info.connected) result.push_back(info);
    }
    return result;
}

/** @brief 获取最近 maxAgeSec 秒内见过的设备，按 RSSI 降序（信号强的在前） */
std::vector<BtDeviceInfo> BtMonitor::getNearbyDevices(int maxAgeSec) const {
    std::lock_guard<std::mutex> lock(deviceMutex_);
    auto now = std::chrono::system_clock::now();
    std::vector<BtDeviceInfo> result;
    for (const auto& [_, info] : devices_) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - info.lastSeen).count();
        if (elapsed <= maxAgeSec) result.push_back(info);
    }
    // 按 RSSI 降序 (信号强的在前)
    std::sort(result.begin(), result.end(), [](const BtDeviceInfo& a, const BtDeviceInfo& b) {
        return a.rssiDbm > b.rssiDbm;
    });
    return result;
}

/** @brief 获取指定设备当前 RSSI；未找到返回 -1000 */
int16_t BtMonitor::getDeviceRssi(const std::string& mac) const {
    std::lock_guard<std::mutex> lock(deviceMutex_);
    for (const auto& [addr, info] : devices_) {
        if (macEquals(addr, mac)) return info.rssiDbm;
    }
    return -1000;
}

/** @brief 获取所有设备的 {MAC → RSSI} 快照 */
std::map<std::string, int16_t> BtMonitor::getRssiSnapshot() const {
    std::lock_guard<std::mutex> lock(deviceMutex_);
    std::map<std::string, int16_t> snapshot;
    for (const auto& [mac, info] : devices_) {
        snapshot[mac] = info.rssiDbm;
    }
    return snapshot;
}

/** @brief 消费事件队列：取出所有待处理事件并清空队列 */
std::vector<BtEvent> BtMonitor::fetchEvents() {
    std::lock_guard<std::mutex> lock(eventMutex_);
    std::vector<BtEvent> events = std::move(pendingEvents_);
    pendingEvents_.clear();
    return events;
}

/** @brief 已知设备总数 */
size_t BtMonitor::deviceCount() const {
    std::lock_guard<std::mutex> lock(deviceMutex_);
    return devices_.size();
}

/** @brief 已连接设备数 */
size_t BtMonitor::connectedCount() const {
    std::lock_guard<std::mutex> lock(deviceMutex_);
    size_t count = 0;
    for (const auto& [_, info] : devices_) {
        if (info.connected) ++count;
    }
    return count;
}

// ============================================================================
// D-Bus 辅助方法
// ============================================================================

/** @brief 发送 D-Bus 方法调用并阻塞等待回复 */
DBusMessage* BtMonitor::sendWithReply(DBusConnection* conn, DBusMessage* msg, int timeoutMs) {
    if (!conn || !msg) return nullptr;
    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, timeoutMs, &err);
    if (dbus_error_is_set(&err)) {
        LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: D-Bus error: " << err.name << " - " << err.message);
        dbus_error_free(&err);
    }
    return reply;
}

/** @brief 调用无参数 BlueZ 方法（如 StartDiscovery/StopDiscovery） */
bool BtMonitor::callBlueZMethod(const std::string& objPath,
                                 const std::string& iface,
                                 const std::string& method) {
    if (!sysConn_) return false;

    DBusMessage* msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, objPath.c_str(), iface.c_str(), method.c_str());
    if (!msg) return false;

    DBusMessage* reply = sendWithReply(sysConn_, msg, 5000);
    dbus_message_unref(msg);

    if (reply) {
        dbus_message_unref(reply);
        return true;
    }
    return false;
}

/** @brief 通过 Properties.Get 获取 string 属性值 */
std::string BtMonitor::getStringProperty(DBusConnection* conn,
                                          const std::string& objPath,
                                          const std::string& iface,
                                               const std::string& propName) {
    if (!conn) return "";
    DBusMessage* msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, objPath.c_str(),
        DBUS_PROPS_IFACE, "Get");
    const char* i = iface.c_str();
    const char* p = propName.c_str();
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &i, DBUS_TYPE_STRING, &p, DBUS_TYPE_INVALID);

    DBusMessage* reply = sendWithReply(conn, msg, 2000);
    dbus_message_unref(msg);
    if (!reply) return "";

    std::string result;
    DBusMessageIter iter;
    if (dbus_message_iter_init(reply, &iter)) {
        extractStringFromIter(&iter, &result);
    }
    dbus_message_unref(reply);
    return result;
}

/** @brief 通过 Properties.Get 获取 int16 属性值 */
int16_t BtMonitor::getInt16Property(DBusConnection* conn,
                                     const std::string& objPath,
                                     const std::string& iface,
                                     const std::string& propName) {
    if (!conn) return 0;
    DBusMessage* msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, objPath.c_str(),
        DBUS_PROPS_IFACE, "Get");
    const char* i = iface.c_str();
    const char* p = propName.c_str();
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &i, DBUS_TYPE_STRING, &p, DBUS_TYPE_INVALID);

    DBusMessage* reply = sendWithReply(conn, msg, 2000);
    dbus_message_unref(msg);
    if (!reply) return 0;

    int16_t result = 0;
    DBusMessageIter iter;
    if (dbus_message_iter_init(reply, &iter)) {
        extractInt16FromIter(&iter, &result);
    }
    dbus_message_unref(reply);
    return result;
}

/** @brief 通过 Properties.Get 获取 bool 属性值 */
bool BtMonitor::getBoolProperty(DBusConnection* conn,
                                 const std::string& objPath,
                                 const std::string& iface,
                                 const std::string& propName) {
    if (!conn) return false;
    DBusMessage* msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, objPath.c_str(),
        DBUS_PROPS_IFACE, "Get");
    const char* i = iface.c_str();
    const char* p = propName.c_str();
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &i, DBUS_TYPE_STRING, &p, DBUS_TYPE_INVALID);

    DBusMessage* reply = sendWithReply(conn, msg, 2000);
    dbus_message_unref(msg);
    if (!reply) return false;

    bool result = false;
    DBusMessageIter iter;
    if (dbus_message_iter_init(reply, &iter)) {
        extractBoolFromIter(&iter, &result);
    }
    dbus_message_unref(reply);
    return result;
}

/** @brief 通过 Properties.Get 获取 string[] 属性值 */
std::vector<std::string> BtMonitor::getStringArrayProperty(DBusConnection* conn,
                                                            const std::string& objPath,
                                                            const std::string& iface,
                                                                                   const std::string& propName) {
    std::vector<std::string> result;
    if (!conn) return result;

    DBusMessage* msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, objPath.c_str(),
        DBUS_PROPS_IFACE, "Get");
    const char* i = iface.c_str();
    const char* p = propName.c_str();
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &i, DBUS_TYPE_STRING, &p, DBUS_TYPE_INVALID);

    DBusMessage* reply = sendWithReply(conn, msg, 2000);
    dbus_message_unref(msg);
    if (!reply) return result;

    DBusMessageIter iter;
    if (dbus_message_iter_init(reply, &iter)) {
        extractStringArrayFromIter(&iter, &result);
    }
    dbus_message_unref(reply);
    return result;
}

// ============================================================================
// 蓝牙监测线程入口 (集成到 server.cpp)
// ============================================================================

/**
 * @brief 启动蓝牙监测后台线程：初始化 → Phase 2 eBPF → 主循环（3s 轮询） → 事件转发 → 被动重试
 * @param ctx ServerContext 智能指针容器，线程通过 .get() 使用 monitor，ownership 由 ServerContext 持有
 * @param outMonitor 输出参数（预留）
 */
void start_bt_monitor_thread(ServerContext* ctx, BtMonitor** /*outMonitor*/) {
    // BtMonitor 由 ServerContext 持有 ownership（智能指针），线程通过 .get() 使用。
    ctx->bt_thread = std::thread([ctx]() {
        LOG_INFO(LogModule::BLUETOOTH, "BT monitor thread started");

        auto* monitor = ctx->bt_monitor.get();
        if (!monitor) {
            LOG_ERROR(LogModule::BLUETOOTH, "BT monitor: ctx->bt_monitor is null");
            return;
        }

        if (!monitor->initialize()) {
            LOG_INFO(LogModule::BLUETOOTH, "BT monitor: no Bluetooth adapter available, "
                     "thread will run in passive mode (periodic retry)");
        } else {
            LOG_INFO(LogModule::BLUETOOTH, "BT monitor: initialized successfully");

            // ================================================================
            // Phase 2: 初始化 eBPF 融合层
            // 在蓝牙适配器就绪后，尝试加载 eBPF 程序挂载到内核 L2CAP 钩子
            // 若挂载失败则自动降级为纯 D-Bus 模式，不影响蓝牙监控基础功能
            // ================================================================
            try {
                bool ebpfOk = monitor->initPhase2(ctx->cfg.bluetooth.bpf_obj.get().c_str());
                if (ebpfOk) {
                    LOG_INFO(LogModule::BLUETOOTH, "BT monitor: Phase 2 eBPF fusion enabled ("
                             << (monitor->isPhase2Available() ? "active" : "fallback") << ")");
                } else {
                    LOG_INFO(LogModule::BLUETOOTH, "BT monitor: Phase 2 eBPF unavailable"
                             " — falling back to D-Bus-only audio quality monitoring");
                }
            } catch (const std::exception& e) {
                LOG_ERROR(LogModule::BLUETOOTH, "BT monitor: Phase 2 init failed: " << e.what());
            }
        }

        int loopCount = 0;
        int retryCount = 0;
        static constexpr int RETRY_INTERVAL = 30;  // 每 30 秒重试初始化

        while (ctx->running.load()) {
            loopCount++;

            if (!monitor->isInitialized()) {
                // 被动模式：定期重试初始化 (适用于蓝牙适配器热插拔)
                retryCount++;
                if (retryCount >= RETRY_INTERVAL / 3) {  // ~每 30s 重试
                    retryCount = 0;
                    LOG_INFO(LogModule::BLUETOOTH, "BT monitor: retrying initialization...");
                    if (monitor->initialize()) {
                        LOG_INFO(LogModule::BLUETOOTH, "BT monitor: initialized after retry");
                    } else {
                        // 清理失败的连接
                        monitor->cleanup();
                    }
                }
                std::this_thread::sleep_for(3000ms);
                continue;
            }

            // 刷新适配器状态
            monitor->refreshAdapterState();

            if (monitor->hasAdapter()) {
                // 刷新设备状态 (核心操作)
                monitor->refreshDeviceStates();

                // 获取事件并转发到 EventManager
                auto events = monitor->fetchEvents();
                for (const auto& ev : events) {
                    switch (ev.type) {
                        case BtEvent::Type::DeviceConnected:
                        case BtEvent::Type::DeviceDisconnected:
                        case BtEvent::Type::DeviceFound:
                        case BtEvent::Type::DeviceLost:
                        case BtEvent::Type::DeviceRssiChanged:
                        case BtEvent::Type::DiscoveryStarted:
                        case BtEvent::Type::DiscoveryStopped:
                            // 所有蓝牙事件统一走 EventManager，发射正确的信号
                            getEventManager().emitBluetoothDeviceChanged(
                                ev.message,
                                ev.deviceName.empty() ? ev.deviceMac : ev.deviceName);
                            break;

                        case BtEvent::Type::AdapterAdded:
                        case BtEvent::Type::AdapterRemoved:
                        case BtEvent::Type::AdapterPowered:
                            // 适配器事件直接发射 D-Bus 信号
                            if (ctx->service) {
                                ctx->service->emitSpecificSignal(
                                    kSignalBluetoothDeviceChanged, ev.message, 0);
                            }
                            break;
                    }
                }

                // 每 6 轮 (~18s) 打印蓝牙状态摘要
                if (loopCount % 6 == 0) {
                    auto nearby = monitor->getNearbyDevices(30);
                    auto connected = monitor->getConnectedDevices();

                    LOG_INFO(LogModule::BLUETOOTH, "BT_SUMMARY: "
                        << monitor->deviceCount() << " known, "
                        << connected.size() << " connected, "
                        << nearby.size() << " nearby");

                    for (const auto& dev : connected) {
                        LOG_INFO(LogModule::BLUETOOTH, "BT_CONNECTED: "
                            << dev.name << " (" << dev.macAddress << ") | "
                            << "RSSI:" << dev.rssiDbm << "dBm"
                            << (dev.rssiDbm != 0 ? " (" + dev.rssiLevel() + ")" : ""));
                    }

                    for (size_t i = 0; i < std::min(nearby.size(), size_t(5)); ++i) {
                        const auto& dev = nearby[i];
                        if (!dev.connected) {
                            LOG_INFO(LogModule::BLUETOOTH, "BT_NEARBY: "
                                << (i+1) << ". " << dev.name
                                << " | RSSI:" << dev.rssiDbm << "dBm"
                                << " | " << dev.rssiLevel());
                        }
                    }
                }
            }

            std::this_thread::sleep_for(3000ms);
        }

        monitor->cleanup();
        monitor->stopPhase2();  // Phase 2: 释放 eBPF 内核资源
        // bt_monitor 由 ServerContext 析构时删除，线程不负责 delete
        LOG_INFO(LogModule::BLUETOOTH, "BT monitor thread stopped");
    });
}


// ============================================================================
// A2DP 音频质量监控（Phase 1b） — 依赖 org.bluez.MediaTransport1 接口
// ============================================================================

/** @brief 探测 BlueZ 是否暴露了 MediaTransport1 接口（缓存结果） */
bool BtMonitor::hasMediaTransportInterface() {
    if (mediaTransportProbed_) {
        return hasMediaTransport_;
    }
    if (!sysConn_) {
        mediaTransportProbed_ = true;
        hasMediaTransport_ = false;
        return false;
    }

    DBusMessage* msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, "/",
        DBUS_OBJMGR_IFACE, "GetManagedObjects");
    DBusMessage* reply = sendWithReply(sysConn_, msg, 3000);
    dbus_message_unref(msg);

    if (!reply) {
        mediaTransportProbed_ = true;
        hasMediaTransport_ = false;
        LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: MediaTransport1 not available (no reply)");
        return false;
    }

    bool found = false;
    DBusMessageIter root;
    if (dbus_message_iter_init(reply, &root)) {
        if (dbus_message_iter_get_arg_type(&root) == DBUS_TYPE_ARRAY) {
            DBusMessageIter objArray;
            dbus_message_iter_recurse(&root, &objArray);
            while (dbus_message_iter_get_arg_type(&objArray) != DBUS_TYPE_INVALID && !found) {
                DBusMessageIter objEntry;
                dbus_message_iter_recurse(&objArray, &objEntry);
                dbus_message_iter_next(&objEntry);

                if (dbus_message_iter_get_arg_type(&objEntry) == DBUS_TYPE_ARRAY) {
                    DBusMessageIter ifaceArray;
                    dbus_message_iter_recurse(&objEntry, &ifaceArray);
                    while (dbus_message_iter_get_arg_type(&ifaceArray) != DBUS_TYPE_INVALID) {
                        DBusMessageIter ifaceEntry;
                        dbus_message_iter_recurse(&ifaceArray, &ifaceEntry);
                        const char* ifaceName = nullptr;
                        dbus_message_iter_get_basic(&ifaceEntry, &ifaceName);
                        if (ifaceName && std::string(ifaceName) == BLUEZ_MEDIA_TRANSPORT_IFACE) {
                            found = true;
                            break;
                        }
                        dbus_message_iter_next(&ifaceArray);
                    }
                }
                dbus_message_iter_next(&objArray);
            }
        }
    }
    dbus_message_unref(reply);

    mediaTransportProbed_ = true;
    hasMediaTransport_ = found;
    LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: MediaTransport1 "
             << (found ? "available" : "not available"));
    return found;
}

/** @brief 列出当前适配器下所有 MediaTransport1 D-Bus 对象路径 */
std::vector<std::string> BtMonitor::listMediaTransportPaths() {
    std::vector<std::string> paths;
    if (!sysConn_) return paths;

    DBusMessage* msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, "/",
        DBUS_OBJMGR_IFACE, "GetManagedObjects");
    DBusMessage* reply = sendWithReply(sysConn_, msg, 3000);
    dbus_message_unref(msg);
    if (!reply) return paths;

    DBusMessageIter root;
    if (!dbus_message_iter_init(reply, &root)) {
        dbus_message_unref(reply);
        return paths;
    }

    if (dbus_message_iter_get_arg_type(&root) == DBUS_TYPE_ARRAY) {
        DBusMessageIter objArray;
        dbus_message_iter_recurse(&root, &objArray);
        while (dbus_message_iter_get_arg_type(&objArray) != DBUS_TYPE_INVALID) {
            DBusMessageIter objEntry;
            dbus_message_iter_recurse(&objArray, &objEntry);
            const char* objPath = nullptr;
            dbus_message_iter_get_basic(&objEntry, &objPath);
            std::string pathStr = objPath ? objPath : "";
            dbus_message_iter_next(&objEntry);

            if (pathStr.find(adapterPath_ + "/") != 0) {
                dbus_message_iter_next(&objArray);
                continue;
            }

            if (dbus_message_iter_get_arg_type(&objEntry) == DBUS_TYPE_ARRAY) {
                DBusMessageIter ifaceArray;
                dbus_message_iter_recurse(&objEntry, &ifaceArray);
                while (dbus_message_iter_get_arg_type(&ifaceArray) != DBUS_TYPE_INVALID) {
                    DBusMessageIter ifaceEntry;
                    dbus_message_iter_recurse(&ifaceArray, &ifaceEntry);
                    const char* ifaceName = nullptr;
                    dbus_message_iter_get_basic(&ifaceEntry, &ifaceName);
                    if (ifaceName && std::string(ifaceName) == BLUEZ_MEDIA_TRANSPORT_IFACE) {
                        paths.push_back(pathStr);
                        break;
                    }
                    dbus_message_iter_next(&ifaceArray);
                }
            }
            dbus_message_iter_next(&objArray);
        }
    }
    dbus_message_unref(reply);
    return paths;
}

/**
 * @brief 从 MediaTransport1 对象的 D-Bus 路径解析 BtAudioTransport
 * @param path MediaTransport1 对象路径（如 /org/bluez/hci0/dev_XX_XX_XX_XX_XX_XX/transport1）
 * @return 填充好的 BtAudioTransport（含 State/Delay/Volume/Codec/deviceMac）
 */
BtAudioTransport BtMonitor::parseMediaTransportProperties(const std::string& path) {
    BtAudioTransport transport;
    transport.transportPath = path;
    if (!sysConn_) return transport;

    DBusMessage* msg = dbus_message_new_method_call(
        BLUEZ_SERVICE, path.c_str(),
        DBUS_PROPS_IFACE, "GetAll");
    const char* transportIface = BLUEZ_MEDIA_TRANSPORT_IFACE;
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &transportIface, DBUS_TYPE_INVALID);

    DBusMessage* reply = sendWithReply(sysConn_, msg, 3000);
    dbus_message_unref(msg);
    if (!reply) return transport;

    DBusMessageIter root;
    if (!dbus_message_iter_init(reply, &root)) {
        dbus_message_unref(reply);
        return transport;
    }

    if (dbus_message_iter_get_arg_type(&root) == DBUS_TYPE_ARRAY) {
        DBusMessageIter propsArray;
        dbus_message_iter_recurse(&root, &propsArray);
        while (dbus_message_iter_get_arg_type(&propsArray) != DBUS_TYPE_INVALID) {
            DBusMessageIter propEntry;
            dbus_message_iter_recurse(&propsArray, &propEntry);
            const char* propName = nullptr;
            dbus_message_iter_get_basic(&propEntry, &propName);
            std::string key = propName ? propName : "";
            dbus_message_iter_next(&propEntry);

            if (key == "State") {
                extractStringFromIter(&propEntry, &transport.state);
            } else if (key == "Delay") {
                extractUint16FromIter(&propEntry, &transport.delay);
            } else if (key == "Volume") {
                extractUint16FromIter(&propEntry, &transport.volume);
            } else if (key == "Codec") {
                int32_t codecVal = 0;
                extractInt32FromIter(&propEntry, &codecVal);
                transport.codec = static_cast<uint8_t>(codecVal);
            } else if (key == "Device") {
                std::string devPath;
                extractStringFromIter(&propEntry, &devPath);
                auto pos = devPath.rfind("dev_");
                if (pos != std::string::npos) {
                    transport.deviceMac = devPath.substr(pos + 4);
                    for (auto& c : transport.deviceMac) {
                        if (c == '_') c = ':';
                    }
                }
            }
            dbus_message_iter_next(&propsArray);
        }
    }
    dbus_message_unref(reply);
    return transport;
}

/**
 * @brief 刷新所有 A2DP 音频传输状态
 * @details 检测新 Transport、状态变化、已移除 Transport；对持续 active 的传输累计 activeDurationMs
 */
void BtMonitor::refreshAudioTransports() {
    if (!hasMediaTransportInterface()) {
        return;
    }

    auto paths = listMediaTransportPaths();
    std::lock_guard<std::mutex> lock(audioMutex_);

    std::set<std::string> currentMacs;
    auto now = std::chrono::system_clock::now();

    for (const auto& path : paths) {
        BtAudioTransport transport = parseMediaTransportProperties(path);
        if (transport.deviceMac.empty()) continue;

        currentMacs.insert(transport.deviceMac);

        auto it = audioTransports_.find(transport.deviceMac);
        if (it == audioTransports_.end()) {
            if (transport.state == "active") {
                transport.lastActive = now;
                transport.activeDurationMs = 0;
            }
            audioTransports_[transport.deviceMac] = transport;
            LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: A2DP transport found: "
                     << transport.deviceMac << " state=" << transport.state
                     << " delay=" << transport.delay << " codec=" << static_cast<int>(transport.codec));
        } else {
            auto& existing = it->second;
            if (existing.state != "active" && transport.state == "active") {
                transport.lastActive = now;
                transport.activeDurationMs = 0;
            } else if (transport.state == "active") {
                transport.lastActive = existing.lastActive;
                if (existing.lastActive.time_since_epoch().count() > 0) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - existing.lastActive).count();
                    transport.activeDurationMs = existing.activeDurationMs + elapsed;
                    transport.lastActive = now;
                }
            }
            audioTransports_[transport.deviceMac] = transport;
        }
    }

    for (auto it = audioTransports_.begin(); it != audioTransports_.end(); ) {
        if (currentMacs.find(it->first) == currentMacs.end()) {
            LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: A2DP transport removed: " << it->first);
            it = audioTransports_.erase(it);
        } else {
            ++it;
        }
    }
}

/**
 * @brief 获取指定蓝牙设备的音频质量评估（纯 D-Bus 模式，不依赖 eBPF）
 * @param mac 目标设备 MAC 地址
 * @param out 输出 BtAudioQuality 结构体
 * @return true 找到该设备且有音频传输
 */
bool BtMonitor::getAudioQuality(const std::string& mac, BtAudioQuality* out) const {
    if (!out) return false;

    std::lock_guard<std::mutex> lock(audioMutex_);
    for (const auto& pair : audioTransports_) {
        if (macEquals(pair.first, mac)) {
            const auto& transport = pair.second;
            out->deviceMac = transport.deviceMac;
            out->isActive = (transport.state == "active");
            out->currentDelay = transport.delay;
            out->qualityScore = calculateAudioScore(transport);
            out->level = scoreToLevel(out->qualityScore);

            out->issues.clear();
            if (transport.delay > 2000) {
                out->issues.push_back("High audio latency (>200ms)");
            } else if (transport.delay > 1000) {
                out->issues.push_back("Moderate audio latency (>100ms)");
            }
            if (transport.codec == 0x00) {
                out->issues.push_back("Using basic SBC codec");
            }
            if (transport.state == "idle" || transport.state == "pending") {
                out->issues.push_back("Transport not active");
            }
            return true;
        }
    }
    return false;
}

/** @brief 获取所有当前已知的 A2DP 音频传输 */
std::vector<BtAudioTransport> BtMonitor::getAudioTransports() const {
    std::lock_guard<std::mutex> lock(audioMutex_);
    std::vector<BtAudioTransport> result;
    result.reserve(audioTransports_.size());
    for (const auto& pair : audioTransports_) {
        result.push_back(pair.second);
    }
    return result;
}

/**
 * @brief 基于 D-Bus 属性（Delay/Codec/State）计算音频质量评分（Phase 1b 纯 D-Bus 逻辑）
 * @param transport MediaTransport1 属性集合
 * @return 0~100 分，越大越好
 */
double BtMonitor::calculateAudioScore(const BtAudioTransport& transport) const {
    double score = 100.0;
    if (transport.delay > 2000) {
        score -= 40.0;
    } else if (transport.delay > 1000) {
        score -= 20.0;
    } else if (transport.delay > 500) {
        score -= 10.0;
    }
    if (transport.codec == 0x00) {
        score -= 5.0;
    }
    if (transport.state != "active") {
        score -= 15.0;
    }
    return std::max(0.0, score);
}

/** @brief 将分数映射为质量等级字符串 */
std::string BtMonitor::scoreToLevel(double score) {
    if (score >= 90.0) return "excellent";
    if (score >= 70.0) return "good";
    if (score >= 50.0) return "fair";
    if (score >= 30.0) return "poor";
    return "unknown";
}

// ============================================================================
// 设备距离估算（Phase 1b） — 基于对数路径损耗模型
// ============================================================================

/**
 * @brief RSSI → 距离的对数路径损耗模型
 * @param rssiDbm 接收信号强度
 * @return 估算距离（米）；无效 RSSI 返回 -1
 * @note 公式：d = 10^((TxPower - RSSI) / (10*n)) * d0，默认 n=2.0 (自由空间)，d0=1m
 */
double BtMonitor::estimateDistance(int16_t rssiDbm) const {
    if (rssiDbm == 0 || rssiDbm <= -1000) {
        return -1.0;
    }
    double ratio = std::pow(10.0,
        static_cast<double>(defaultTxPower_ - rssiDbm) / (10.0 * PATH_LOSS_EXPONENT));
    return ratio * REFERENCE_DISTANCE_M;
}

/**
 * @brief 使用已知距离校准 TxPower，提升后续距离估算精度
 * @param mac 目标设备 MAC
 * @param knownMeters 已知的实际距离（米）
 * @return true 校准成功
 */
bool BtMonitor::calibrateDistance(const std::string& mac, double knownMeters) {
    if (knownMeters <= 0.0) return false;
    std::lock_guard<std::mutex> lock(deviceMutex_);
    for (auto& pair : devices_) {
        if (macEquals(pair.first, mac)) {
            auto& info = pair.second;
            if (info.rssiDbm == 0 || info.rssiDbm <= -1000) {
                return false;
            }
            int16_t calibrated = static_cast<int16_t>(
                info.rssiDbm + 10.0 * PATH_LOSS_EXPONENT * std::log10(knownMeters));
            info.calibratedTxPower = calibrated;
            defaultTxPower_ = calibrated;
            LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: distance calibrated for "
                     << mac << " at " << knownMeters << "m, txPower=" << calibrated << "dBm");
            return true;
        }
    }
    return false;
}

/** @brief 设置默认 TxPower 参考值（用于路径损耗模型） */
void BtMonitor::setDefaultTxPower(int16_t txPower) {
    defaultTxPower_ = txPower;
    LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: default TxPower set to " << txPower << "dBm");
}

// ============================================================================
// Phase 2: eBPF 融合层 — 集成 BtAudioAnalyzer + BtAudioFusion
// ============================================================================

/**
 * @brief Phase 2 初始化：创建融合评分器 + eBPF 分析器并尝试挂载内核钩子
 * @param bpfObjectPath 编译好的 eBPF 对象文件路径（如 build/a2dp_media.bpf.o）
 * @return true eBPF 挂载成功；false 挂载失败（自动降级为纯 D-Bus 模式）
 */
bool BtMonitor::initPhase2(const std::string& bpfObjectPath) {
    // 创建融合评估器（始终可用，用于纯 D-Bus 降级模式）
    if (!btAudioFusion_) {
        btAudioFusion_ = std::make_unique<BtAudioFusion>();
    }

    // 创建 eBPF 分析器并尝试挂载
    bool ebpfAttached = btAudioAnalyzer_->init(bpfObjectPath);

    if (ebpfAttached) {
        LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: Phase 2 eBPF initialized successfully — hook="
                 << btAudioAnalyzer_->attachedHookName());
    } else {
        LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: Phase 2 eBPF unavailable — "
                 << btAudioAnalyzer_->lastError() << " — using D-Bus only mode");
    }

    return ebpfAttached;
}

/** @brief 停止 Phase 2：释放 eBPF 内核资源、清空前次统计快照
 *  @note  刻意保留 btAudioAnalyzer_ 对象（只 stop 不 reset）：
 *         GetEbpfMonitorHealth 会遍历 audioAnalyzer()，置空会产生悬空空指针；
 *         停止后的分析器以 Stopped 状态继续如实上报健康快照。 */
void BtMonitor::stopPhase2() {
    if (btAudioAnalyzer_) {
        btAudioAnalyzer_->stop();
    }
    if (btAudioFusion_) {
        btAudioFusion_.reset();
    }
    btPrevStats_.clear();
    LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: Phase 2 stopped");
}

/**
 * @brief 获取融合音频质量评分（D-Bus 传输状态 + eBPF 流量统计 融合）
 * @details 协作链：
 *   1. BtMonitor 从 BlueZ D-Bus 拉取 MediaTransport1 属性 → BtAudioTransport
 *   2. BtMonitor 调用 BtAudioAnalyzer::getStats() 从内核态 bt_traffic map 读取累计流量
 *   3. BtAudioFusion::evaluate() 将 Transport + eBPF 统计融合为单一质量分数
 * @param mac 目标设备 MAC
 * @param out 输出融合结果（含 qualityScore / effectiveActive / suspectedStall / ebpfCorrection）
 * @return true 成功；false 该设备无音频传输
 */
bool BtMonitor::getAudioFusionResult(const std::string& mac, BtAudioFusionResult* out) const {
    if (!out) return false;

    // 获取 D-Bus 音频传输状态
    BtAudioQuality dbQuality;
    if (!getAudioQuality(mac, &dbQuality)) {
        return false;
    }

    // 获取 eBPF 流量统计
    BtTrafficStats stats;
    bool ebpfAvailable = isPhase2Available();
    bool hasStats = false;

    if (ebpfAvailable) {
        // 尝试从 eBPF 读取当前统计
        hasStats = btAudioAnalyzer_->getStats(mac, 0 /*发送方向*/, &stats);
    }

    // 构造融合评估所需的 Transport 对象
    BtAudioTransport transport;
    {
        std::lock_guard<std::mutex> lock(audioMutex_);
        auto it = audioTransports_.find(mac);
        if (it != audioTransports_.end()) {
            transport = it->second;
        } else {
            // 从 BtAudioQuality 构造最小 Transport
            transport.deviceMac = mac;
            transport.state = dbQuality.isActive ? "active" : "inactive";
            transport.delay = dbQuality.currentDelay;
        }
    }

    // 获取前次统计快照用于增量计算（线程安全访问 mutable 成员）
    BtTrafficStats prevStats;
    bool hasPrevStats = false;
    {
        std::lock_guard<std::mutex> lock(audioMutex_);
        auto it = btPrevStats_.find(mac);
        if (it != btPrevStats_.end()) {
            prevStats = it->second;
            hasPrevStats = true;
        }
    }

    // 执行融合评估
    *out = btAudioFusion_->evaluate(
        transport,
        hasStats ? &stats : nullptr,
        hasPrevStats ? &prevStats : nullptr,
        ebpfAvailable && hasStats);

    // 更新前次快照（线程安全写入 mutable 成员）
    if (hasStats) {
        std::lock_guard<std::mutex> lock(audioMutex_);
        btPrevStats_[mac] = stats;
    }

    return true;
}

/** @brief Phase 2 是否可用（eBPF 已挂载且正常工作） */
bool BtMonitor::isPhase2Available() const {
    return btAudioAnalyzer_ && btAudioAnalyzer_->isAvailable();
}

}  // namespace weaknet_dbus
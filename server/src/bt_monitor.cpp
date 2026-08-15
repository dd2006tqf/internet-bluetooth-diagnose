// bt_monitor.cpp
// 蓝牙监测器实现：通过 BlueZ D-Bus API (系统总线) 监测蓝牙设备
// BlueZ 接口文档: https://git.kernel.org/pub/scm/bluetooth/bluez.git/tree/doc

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
// 工具函数
// ============================================================================

// 从迭代器递归提取 variant 容器内的 int16 值 (DBUS_TYPE_INT16)
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

// 从迭代器递归提取 variant 容器内的 bool 值
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

// 从迭代器递归提取 variant 容器内的 string 值
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

// 从迭代器递归提取 variant 容器内的 uint16 值
// 从迭代器递归提取 variant 容器内的 int32 值
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

// 从迭代器递归提取 variant 容器内的字符串数组
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

// MAC 地址比较 (忽略大小写)
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
    LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: instance created");
}

BtMonitor::~BtMonitor() {
    cleanup();
}

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

bool BtMonitor::hasAdapter() const {
    std::lock_guard<std::mutex> lock(adapterMutex_);
    return !adapterPath_.empty() && adapterState_.powered;
}

BtAdapterState BtMonitor::getAdapterState() const {
    std::lock_guard<std::mutex> lock(adapterMutex_);
    return adapterState_;
}

// ============================================================================
// 发现控制
// ============================================================================

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

// 列出 /org/bluez/hci0 下所有 Device1 对象路径
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

std::vector<BtDeviceInfo> BtMonitor::getDevices() const {
    std::lock_guard<std::mutex> lock(deviceMutex_);
    std::vector<BtDeviceInfo> result;
    result.reserve(devices_.size());
    for (const auto& [_, info] : devices_) {
        result.push_back(info);
    }
    return result;
}

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

std::vector<BtDeviceInfo> BtMonitor::getConnectedDevices() const {
    std::lock_guard<std::mutex> lock(deviceMutex_);
    std::vector<BtDeviceInfo> result;
    for (const auto& [_, info] : devices_) {
        if (info.connected) result.push_back(info);
    }
    return result;
}

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

int16_t BtMonitor::getDeviceRssi(const std::string& mac) const {
    std::lock_guard<std::mutex> lock(deviceMutex_);
    for (const auto& [addr, info] : devices_) {
        if (macEquals(addr, mac)) return info.rssiDbm;
    }
    return -1000;
}

std::map<std::string, int16_t> BtMonitor::getRssiSnapshot() const {
    std::lock_guard<std::mutex> lock(deviceMutex_);
    std::map<std::string, int16_t> snapshot;
    for (const auto& [mac, info] : devices_) {
        snapshot[mac] = info.rssiDbm;
    }
    return snapshot;
}

std::vector<BtEvent> BtMonitor::fetchEvents() {
    std::lock_guard<std::mutex> lock(eventMutex_);
    std::vector<BtEvent> events = std::move(pendingEvents_);
    pendingEvents_.clear();
    return events;
}

size_t BtMonitor::deviceCount() const {
    std::lock_guard<std::mutex> lock(deviceMutex_);
    return devices_.size();
}

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

void start_bt_monitor_thread(ServerContext* ctx, BtMonitor** /*outMonitor*/) {
    // BtMonitor* 已改为 atomic 成员，写回由本线程序直接 ctx->bt_monitor.store()；
    // outMonitor 参数保留以兼容调用签名，但不再直接取值。
    std::thread([ctx]() {
        LOG_INFO(LogModule::BLUETOOTH, "BT monitor thread started");

        // 创建独立的 BtMonitor 实例
        auto monitor = std::make_unique<BtMonitor>();
        // 将原始指针写回 ServerContext(atomic)，供 DbusService 查询读取
        ctx->bt_monitor.store(monitor.get());

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
                bool ebpfOk = monitor->initPhase2("build/a2dp_media.bpf.o");
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
        ctx->bt_monitor.store(nullptr);
        LOG_INFO(LogModule::BLUETOOTH, "BT monitor thread stopped");
    }).detach();
}


// ============================================================================
// A2DP 音频质量监控（Phase 1b）
// ============================================================================

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

std::vector<BtAudioTransport> BtMonitor::getAudioTransports() const {
    std::lock_guard<std::mutex> lock(audioMutex_);
    std::vector<BtAudioTransport> result;
    result.reserve(audioTransports_.size());
    for (const auto& pair : audioTransports_) {
        result.push_back(pair.second);
    }
    return result;
}

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

std::string BtMonitor::scoreToLevel(double score) {
    if (score >= 90.0) return "excellent";
    if (score >= 70.0) return "good";
    if (score >= 50.0) return "fair";
    if (score >= 30.0) return "poor";
    return "unknown";
}

// ============================================================================
// 设备距离估算（Phase 1b）
// ============================================================================

double BtMonitor::estimateDistance(int16_t rssiDbm) const {
    if (rssiDbm == 0 || rssiDbm <= -1000) {
        return -1.0;
    }
    double ratio = std::pow(10.0,
        static_cast<double>(defaultTxPower_ - rssiDbm) / (10.0 * PATH_LOSS_EXPONENT));
    return ratio * REFERENCE_DISTANCE_M;
}

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

void BtMonitor::setDefaultTxPower(int16_t txPower) {
    defaultTxPower_ = txPower;
    LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: default TxPower set to " << txPower << "dBm");
}

// ============================================================================
// Phase 2: eBPF 融合层
// ============================================================================

bool BtMonitor::initPhase2(const std::string& bpfObjectPath) {
    // 创建融合评估器（始终可用，用于纯 D-Bus 降级模式）
    if (!btAudioFusion_) {
        btAudioFusion_ = std::make_unique<BtAudioFusion>();
    }

    // 创建 eBPF 分析器并尝试挂载
    btAudioAnalyzer_ = std::make_unique<BtAudioAnalyzer>();
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

void BtMonitor::stopPhase2() {
    if (btAudioAnalyzer_) {
        btAudioAnalyzer_->stop();
        btAudioAnalyzer_.reset();
    }
    if (btAudioFusion_) {
        btAudioFusion_.reset();
    }
    btPrevStats_.clear();
    LOG_INFO(LogModule::BLUETOOTH, "BtMonitor: Phase 2 stopped");
}

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

    // 获取前次统计快照用于增量计算
    const BtTrafficStats* prevPtr = nullptr;
    {
        auto it = btPrevStats_.find(mac);
        if (it != btPrevStats_.end()) {
            prevPtr = &it->second;
        }
    }

    // 执行融合评估
    *out = btAudioFusion_->evaluate(
        transport,
        hasStats ? &stats : nullptr,
        prevPtr,
        ebpfAvailable && hasStats);

    // 更新前次快照
    if (hasStats) {
        btPrevStats_[mac] = stats;
    }

    return true;
}

bool BtMonitor::isPhase2Available() const {
    return btAudioAnalyzer_ && btAudioAnalyzer_->isAvailable();
}

}  // namespace weaknet_dbus

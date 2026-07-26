// common.hpp
// 提供DBus相关的常量与通用定义

#pragma once

#include <string>

namespace weaknet_dbus {

// D-Bus 基本标识
static constexpr const char kBusName[] = "com.example.WeakNet";            // 服务名
static constexpr const char kObjectPath[] = "/com/example/WeakNet";        // 对象路径
static constexpr const char kInterface[] = "com.example.WeakNet";          // 接口名

// 方法与信号名称
static constexpr const char kMethodGet[] = "Get";                          // Get 方法（示例字符串）
static constexpr const char kMethodListInterfaces[] = "ListInterfaces";    // 返回字符串数组
static constexpr const char kMethodGetInterfaces[] = "GetInterfaces";      // 同义方法，返回字符串数组
static constexpr const char kMethodHealthCheck[] = "HealthCheck";          // 网络健康检查
static constexpr const char kMethodPing[] = "Ping";                       // Ping方法（ping指定主机）
static constexpr const char kMethodGetBluetoothDevices[] = "GetBluetoothDevices";   // 获取蓝牙设备列表
static constexpr const char kMethodGetBluetoothAdapter[] = "GetBluetoothAdapter";   // 获取蓝牙适配器信息
static constexpr const char kSignalChanged[] = "Changed";                  // Changed 信号（通用状态变化）
static constexpr const char kSignalInterfaceChanged[] = "InterfaceChanged"; // 网卡变化信号
static constexpr const char kSignalConnectionModeChanged[] = "ConnectionModeChanged"; // 上网方式变化信号
static constexpr const char kSignalNetworkQualityChanged[] = "NetworkQualityChanged"; // 网络质量变化信号
static constexpr const char kSignalBluetoothDeviceChanged[] = "BluetoothDeviceChanged"; // 蓝牙设备变化信号

// 序列化输出文件路径（使用 $XDG_RUNTIME_DIR 私有目录，防符号链接攻击）
// 详见 project_memory: serializeGetReplyToFile/serializeChangedPayloadToFile 必须使用
// $XDG_RUNTIME_DIR 私有目录或 O_NOFOLLOW 标志
static const std::string kSignalSerializedFile = []() {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    return std::string(xdg ? xdg : "/tmp") + "/weaknet/signal_changed.bin";
}();
static const std::string kGetReplySerializedFile = []() {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    return std::string(xdg ? xdg : "/tmp") + "/weaknet/get_reply.bin";
}();

}  // namespace weaknet_dbus


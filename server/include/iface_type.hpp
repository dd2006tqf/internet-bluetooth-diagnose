// iface_type.hpp
// 网络接口类型识别辅助函数：通过 sysfs 标志判断接口是否为 Wi-Fi
//
// 支持 WEXT（wireless 目录）和 cfg80211（phy80211 符号链接）两种标志，
// 并提供接口名前缀后备（wlan/wlp/wlx），兼容现代 Wi-Fi 驱动。

#pragma once

#include <string>

namespace weaknet_dbus {

// 判断指定网络接口是否为无线（Wi-Fi）接口。
// sysfsBase 参数用于单元测试（传入临时目录模拟 sysfs），默认为 /sys/class/net。
bool isWirelessInterface(const std::string& ifname,
                         const std::string& sysfsBase = "/sys/class/net");

}  // namespace weaknet_dbus

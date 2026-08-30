/**
 * @file iface_type.hpp
 * @brief 网络接口类型识别辅助函数
 *
 * 通过 sysfs 标志判断接口是否为无线（Wi-Fi）接口。
 * 支持三种判定依据：
 *   1. WEXT：/sys/class/net/<ifname>/wireless 目录存在
 *   2. cfg80211：/sys/class/net/<ifname>/phy80211 符号链接存在
 *   3. 接口名前缀：wlan / wlp / wlx（兜底，兼容命名规范）
 *
 * 优先级：cfg80211 > WEXT > 接口名前缀。
 */

#pragma once

#include <string>

namespace weaknet_dbus {

/**
 * @brief 判断指定网络接口是否为无线（Wi-Fi）接口
 *
 * @param ifname   接口名（如 "wlan0"、"eth0"、"enp3s0"）
 * @param sysfsBase  sysfs 根目录，默认 "/sys/class/net"；
 *                   传入临时目录可用于单元测试 mock
 * @return true  判定为 Wi-Fi 接口
 * @return false 判定为有线或无法识别
 */
bool isWirelessInterface(const std::string& ifname,
                         const std::string& sysfsBase = "/sys/class/net");

}  // namespace weaknet_dbus

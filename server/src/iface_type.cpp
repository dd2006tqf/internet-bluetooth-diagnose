// iface_type.cpp
// 实现 isWirelessInterface：通过 sysfs 标志判断接口是否为 Wi-Fi
//
// 支持三种识别方式（按优先级）：
// 1. WEXT 标志：/sys/class/net/<iface>/wireless 目录（旧驱动）
// 2. cfg80211 标志：/sys/class/net/<iface>/phy80211 符号链接/目录（新驱动）
// 3. 接口名前缀后备：wlan/wlp/wlx（sysfs 标志缺失时）

#include "iface_type.hpp"
#include <sys/stat.h>

namespace weaknet_dbus {

bool isWirelessInterface(const std::string& ifname, const std::string& sysfsBase) {
    // 1. WEXT 标志：/sys/class/net/<iface>/wireless 目录
    std::string wirelessPath = sysfsBase + "/" + ifname + "/wireless";
    struct stat st;
    if (stat(wirelessPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        return true;
    }
    // 2. cfg80211 标志：/sys/class/net/<iface>/phy80211 符号链接/目录
    std::string phyPath = sysfsBase + "/" + ifname + "/phy80211";
    if (stat(phyPath.c_str(), &st) == 0) {
        return true;
    }
    // 3. 接口名前缀后备（wlan/wlp/wlx）
    if (ifname.rfind("wlan", 0) == 0 ||
        ifname.rfind("wlp", 0) == 0 ||
        ifname.rfind("wlx", 0) == 0) {
        return true;
    }
    return false;
}

}  // namespace weaknet_dbus

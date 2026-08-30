/**
 * @file iface_type.cpp
 * @brief 判断网络接口是否为 Wi-Fi 类型
 *
 * @details 通过 Linux sysfs 文件系统识别接口是否为无线网卡。支持三种识别方式
 *          并按优先级依次尝试：
 *
 *          1. **WEXT 标志**（旧驱动）：
 *             检查 `/sys/class/net/<iface>/wireless` 目录是否存在。
 *             WEXT（Wireless Extension）是 Linux 传统的无线扩展接口，
 *             现代驱动仍可能保留该目录。
 *
 *          2. **cfg80211 标志**（新驱动）：
 *             检查 `/sys/class/net/<iface>/phy80211` 符号链接/目录是否存在。
 *             cfg80211 是 Linux 无线子系统的新标准框架，phy80211 指向
 *             对应的无线 PHY 设备。
 *
 *          3. **接口名前缀后备**：
 *             当 sysfs 标志缺失时，通过接口名前缀（wlan/wlp/wlx）兜底识别，
 *             适用于某些容器/虚拟化环境中 sysfs 信息不完整的情况。
 *
 * @note 本文件不依赖 netlink 或其他 socket 系统调用，仅通过 stat() 查询 sysfs。
 *       头文件 <sys/stat.h> 提供 stat() 和 S_ISDIR/S_ISLNK 宏。
 */

#include "iface_type.hpp"
#include <sys/stat.h>

namespace weaknet_dbus {

/**
 * @brief 判断指定接口是否为 Wi-Fi 无线网卡
 *
 * 按 WEXT → cfg80211 → 接口名前缀 的优先级依次尝试识别，一旦命中即返回 true。
 *
 * @param ifname   接口名称（如 "wlan0"、"eth0"）
 * @param sysfsBase sysfs 网络类目录前缀，默认为 "/sys/class/net"；
 *                  传入不同值可支持非标准 sysfs 挂载点（如容器场景）
 *
 * @return true  - 识别为 Wi-Fi 无线网卡
 *         false - 未识别为 Wi-Fi，或 sysfs 查询失败
 */
bool isWirelessInterface(const std::string& ifname, const std::string& sysfsBase) {
    // 1. WEXT 标志：/sys/class/net/<iface>/wireless 目录存在即判定为无线
    std::string wirelessPath = sysfsBase + "/" + ifname + "/wireless";
    struct stat st;
    // stat() 跟随符号链接，S_ISDIR 要求最终目标是目录
    if (stat(wirelessPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        return true;
    }
    // 2. cfg80211 标志：/sys/class/net/<iface>/phy80211 存在即判定为无线
    //    cfg80211 驱动下该路径是指向 /sys/class/ieee80211/phyX 的符号链接
    std::string phyPath = sysfsBase + "/" + ifname + "/phy80211";
    if (stat(phyPath.c_str(), &st) == 0) {
        return true;
    }
    // 3. 接口名前缀后备（wlan/wlp/wlx）
    //    使用 rfind(prefix, 0) 精确匹配字符串开头，避免 "ethwlan0" 等误判
    if (ifname.rfind("wlan", 0) == 0 ||
        ifname.rfind("wlp", 0) == 0 ||
        ifname.rfind("wlx", 0) == 0) {
        return true;
    }
    return false;
}

}  // namespace weaknet_dbus

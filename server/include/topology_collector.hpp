#pragma once

/**
 * @file topology_collector.hpp
 * @brief 网卡物理拓扑元数据采集（MAC, IP, Gateway, DNS, Wi-Fi SSID/BSSID）
 *
 * 为 EdgeTelemetry 快照补齐底层 L2/L3 拓扑信息，对齐云端 contracts.py 的
 * NetworkExperienceSnapshot 字段。
 *
 * 字段采集原则：
 *   - 每一项独立采集，单项失败优雅回退 nullopt / 空列表，绝不拖垮其他指标；
 *   - 纯函数/无状态工具类，不持有全局状态；
 *   - 字符串结果均经过标准格式化（MAC: 大写 HEX，IP/网关: 点分十进制字符串）。
 */

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace weaknet {

struct TopologySnapshot {
    std::optional<std::string> mac_address;       ///< MAC 地址 ("AA:BB:CC:DD:EE:FF")
    std::optional<std::string> ip_address;        ///< IPv4 地址 ("192.168.1.100")
    std::optional<std::string> gateway_ip;       ///< 默认网关 IPv4 ("192.168.1.1")
    std::vector<std::string>   dns_servers;      ///< 配置的 DNS 解析器列表
    std::optional<std::string> ap_ssid;          ///< Wi-Fi AP SSID（仅无线有效）
    std::optional<std::string> ap_bssid;         ///< Wi-Fi AP BSSID（仅无线有效）
    int                        ap_freq_mhz{0};   ///< Wi-Fi 频段频率 (MHz)，用于精确化 link_type
    bool                       collected{false}; ///< 是否执行过有效采集
};

class TopologyCollector {
public:
    /**
     * @brief 采集指定接口的拓扑元数据
     * @param iface  网络接口名称（如 "wlan0", "eth0"）
     * @param is_wireless 是否为无线接口
     * @return 完整的拓扑快照
     */
    static TopologySnapshot collect(const std::string& iface, bool is_wireless = false);

    /// 解析指定文件中的 nameserver 列表（默认 /etc/resolv.conf）
    static std::vector<std::string> readDnsServers(const std::string& resolv_path = "/etc/resolv.conf");

    /// 获取指定接口的 MAC 地址（优先 sysfs，回退 ioctl）
    static std::optional<std::string> getMacAddress(const std::string& iface);

    /// 获取指定接口的首个非回环 IPv4 地址
    static std::optional<std::string> getIpv4Address(const std::string& iface);

    /// 获取系统默认路由网关 IP
    static std::optional<std::string> getDefaultGateway();
};

}  // namespace weaknet

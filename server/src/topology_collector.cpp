#include "topology_collector.hpp"

#include <arpa/inet.h>
#include <cctype>
#include <cstring>
#include <fstream>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sstream>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "logger.hpp"
#include "net_wifiriss.h"

namespace weaknet {

namespace {

std::string formatMacBytes(const uint8_t bytes[6]) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
    return std::string(buf);
}

}  // namespace

std::vector<std::string> TopologyCollector::readDnsServers(const std::string& resolv_path) {
    std::vector<std::string> servers;
    std::ifstream in(resolv_path);
    if (!in.is_open()) {
        return servers;
    }

    std::string line;
    while (std::getline(in, line)) {
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#' || line[start] == ';') {
            continue;
        }
        if (line.compare(start, 10, "nameserver") == 0) {
            size_t val_start = line.find_first_not_of(" \t", start + 10);
            if (val_start != std::string::npos) {
                size_t val_end = line.find_first_of(" \t\r\n#", val_start);
                std::string ip = line.substr(val_start, (val_end == std::string::npos) ? std::string::npos : val_end - val_start);
                struct in_addr addr4;
                struct in6_addr addr6;
                if (::inet_pton(AF_INET, ip.c_str(), &addr4) == 1 ||
                    ::inet_pton(AF_INET6, ip.c_str(), &addr6) == 1) {
                    servers.push_back(ip);
                    if (servers.size() >= 16) {
                        break;
                    }
                }
            }
        }
    }
    return servers;
}

std::optional<std::string> TopologyCollector::getMacAddress(const std::string& iface) {
    if (iface.empty()) return std::nullopt;

    // 优先读取 sysfs: /sys/class/net/<iface>/address
    std::string sysfs_path = "/sys/class/net/" + iface + "/address";
    std::ifstream in(sysfs_path);
    if (in.is_open()) {
        std::string raw;
        if (std::getline(in, raw) && raw.length() >= 17) {
            std::string mac;
            for (char c : raw) {
                if (c == '\r' || c == '\n' || c == ' ') break;
                mac.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            }
            if (mac.length() == 17 && mac != "00:00:00:00:00:00") {
                return mac;
            }
        }
    }

    // 回退到 ioctl SIOCGIFHWADDR
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0) {
        struct ifreq ifr;
        std::memset(&ifr, 0, sizeof(ifr));
        std::strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
        if (::ioctl(fd, SIOCGIFHWADDR, &ifr) >= 0) {
            const auto* bytes = reinterpret_cast<const uint8_t*>(ifr.ifr_hwaddr.sa_data);
            std::string mac = formatMacBytes(bytes);
            ::close(fd);
            if (mac != "00:00:00:00:00:00") {
                return mac;
            }
        } else {
            ::close(fd);
        }
    }
    return std::nullopt;
}

std::optional<std::string> TopologyCollector::getIpv4Address(const std::string& iface) {
    if (iface.empty()) return std::nullopt;

    struct ifaddrs* ifaddr = nullptr;
    if (::getifaddrs(&ifaddr) == -1 || !ifaddr) {
        return std::nullopt;
    }

    std::optional<std::string> result;
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if (iface == ifa->ifa_name) {
            auto* s = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
            char ip_str[INET_ADDRSTRLEN];
            if (::inet_ntop(AF_INET, &s->sin_addr, ip_str, sizeof(ip_str))) {
                std::string ip(ip_str);
                if (ip != "127.0.0.1") {
                    result = ip;
                    break;
                }
            }
        }
    }
    ::freeifaddrs(ifaddr);
    return result;
}

std::optional<std::string> TopologyCollector::getDefaultGateway() {
    std::ifstream route_file("/proc/net/route");
    if (!route_file.is_open()) {
        return std::nullopt;
    }

    std::string line;
    // 跳过表头
    if (!std::getline(route_file, line)) {
        return std::nullopt;
    }

    while (std::getline(route_file, line)) {
        std::istringstream iss(line);
        std::string iface;
        unsigned long dest = 0, gw = 0;
        if (iss >> iface >> std::hex >> dest >> gw) {
            // 默认路由: Destination 为 0, Gateway 非 0
            if (dest == 0 && gw != 0) {
                struct in_addr gw_addr;
                gw_addr.s_addr = static_cast<in_addr_t>(gw);
                char buf[INET_ADDRSTRLEN];
                if (::inet_ntop(AF_INET, &gw_addr, buf, sizeof(buf))) {
                    return std::string(buf);
                }
            }
        }
    }
    return std::nullopt;
}

TopologySnapshot TopologyCollector::collect(const std::string& iface, bool is_wireless) {
    TopologySnapshot snap;
    if (iface.empty()) {
        return snap;
    }

    snap.mac_address = getMacAddress(iface);
    snap.ip_address = getIpv4Address(iface);
    snap.gateway_ip = getDefaultGateway();
    snap.dns_servers = readDnsServers();

    if (is_wireless) {
        auto wifi_client = ::WiFiRssiClient::getInstance();
        if (wifi_client) {
            auto bssid_bytes = wifi_client->getAssociatedBssid();
            bool is_zero = true;
            for (auto b : bssid_bytes) {
                if (b != 0) { is_zero = false; break; }
            }
            if (!is_zero) {
                snap.ap_bssid = formatMacBytes(bssid_bytes.data());
            }

            snap.ap_freq_mhz = wifi_client->getFrequency();

            std::string ssid = wifi_client->getAssociatedSsid();
            if (!ssid.empty()) {
                snap.ap_ssid = ssid;
            }
        }
    }

    snap.collected = true;
    return snap;
}

}  // namespace weaknet

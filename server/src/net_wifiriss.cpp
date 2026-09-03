/**
 * @file net_wifiriss.cpp
 * @brief 通过 wpa_supplicant 控制接口获取 Wi-Fi RSSI（接收信号强度指示）
 *
 * @details 本文件实现 WiFiRssiClient 单例类，通过 Unix Domain Socket（AF_UNIX,
 *          SOCK_DGRAM）与 wpa_supplicant 守护进程通信，发送 "SIGNAL_POLL" 命令
 *          并解析返回值中的 RSSI 字段。
 *
 *          设计要点：
 *          - 使用 DGRAM 而非 STREAM，因为 wpa_ctrl 协议基于数据报，每条消息独立
 *          - 客户端先 bind 本地临时 socket 地址（/tmp/wpa_ctrl_<pid>_<iface>），
 *            再 connect 到 wpa_supplicant 控制 socket 路径
 *          - 自动探测多个 wpa_supplicant 目录：参数 ctrlDir → 环境变量 → 标准路径
 *            （/run/wpa_supplicant、/var/run/wpa_supplicant）
 *          - 若 wpa_supplicant 未运行，尝试 fork()+execl() 自动拉起（需要 root 权限）
 *
 * @note 关键系统接口：
 *       - socket(AF_UNIX, SOCK_DGRAM, 0)  — 创建 Unix 域数据报 socket
 *       - bind() / connect()              — 绑定本地地址、连接远端控制 socket
 *       - send() / recv()                 — 发送 wpa_ctrl 命令、接收响应
 *       - fork() + execl()                — 拉起 wpa_supplicant 守护进程
 *       - setsockopt(SO_RCVTIMEO)         — 设置接收超时避免永久阻塞
 */

#include "net_wifiriss.h"
#include "logger.hpp"

using namespace weaknet_dbus;

#include <sys/socket.h>
#include <sys/un.h>
#include <linux/genetlink.h>
#include <linux/nl80211.h>
#include <linux/netlink.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <linux/wireless.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <vector>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <fstream>
#include <sstream>
#include <array>
#include <algorithm>

namespace weaknet_dbus {

namespace {

constexpr uint16_t kNlaFNested = NLA_F_NESTED;
constexpr uint8_t kCtrlCmdGetFamily = CTRL_CMD_GETFAMILY;
constexpr uint16_t kCtrlAttrFamilyId = CTRL_ATTR_FAMILY_ID;
constexpr uint16_t kCtrlAttrFamilyName = CTRL_ATTR_FAMILY_NAME;
constexpr uint16_t kNl80211AttrIfindex = NL80211_ATTR_IFINDEX;
constexpr uint16_t kNl80211AttrMac = NL80211_ATTR_MAC;
constexpr uint16_t kNl80211AttrStaInfo = NL80211_ATTR_STA_INFO;
constexpr uint16_t kNl80211StaInfoSignal = NL80211_STA_INFO_SIGNAL;
constexpr uint8_t kNl80211CmdGetStation = NL80211_CMD_GET_STATION;

struct NlAttr {
    uint16_t len;
    uint16_t type;
};

size_t nlaAlign(size_t len) { return (len + 3U) & ~3U; }

bool appendNlAttr(std::vector<uint8_t>& message, uint16_t type,
                  const void* data, size_t length) {
    const size_t offset = message.size();
    const size_t total = nlaAlign(sizeof(NlAttr) + length);
    message.resize(offset + total, 0);
    auto* attr = reinterpret_cast<NlAttr*>(message.data() + offset);
    attr->len = static_cast<uint16_t>(sizeof(NlAttr) + length);
    attr->type = type;
    if (length) std::memcpy(message.data() + offset + sizeof(NlAttr), data, length);
    return true;
}

bool sendNetlinkMessage(int fd, const std::vector<uint8_t>& message) {
    sockaddr_nl kernel{};
    kernel.nl_family = AF_NETLINK;
    return sendto(fd, message.data(), message.size(), 0,
                  reinterpret_cast<sockaddr*>(&kernel), sizeof(kernel)) >= 0;
}

bool getNl80211FamilyId(int fd, uint16_t* familyId) {
    char family[] = "nl80211";
    std::vector<uint8_t> msg(NLMSG_SPACE(sizeof(genlmsghdr)), 0);
    auto* header = reinterpret_cast<nlmsghdr*>(msg.data());
    header->nlmsg_len = NLMSG_LENGTH(sizeof(genlmsghdr));
    header->nlmsg_type = GENL_ID_CTRL;
    header->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    header->nlmsg_seq = 1;
    auto* gen = reinterpret_cast<genlmsghdr*>(NLMSG_DATA(header));
    gen->cmd = kCtrlCmdGetFamily;
    gen->version = 1;
    gen->reserved = 0;
    appendNlAttr(msg, kCtrlAttrFamilyName, family, sizeof(family));
    header->nlmsg_len = static_cast<uint32_t>(msg.size());
    if (!sendNetlinkMessage(fd, msg)) return false;

    std::array<uint8_t, 8192> response{};
    const ssize_t size = recv(fd, response.data(), response.size(), 0);
    if (size < 0) {
        LOG_WARNING(LogModule::RSSI, "nl80211: CTRL family receive failed: " << strerror(errno));
        return false;
    }
    int remainingSize = static_cast<int>(size);
    for (auto* nl = reinterpret_cast<nlmsghdr*>(response.data());
         NLMSG_OK(nl, remainingSize); nl = NLMSG_NEXT(nl, remainingSize)) {
        if (nl->nlmsg_type == NLMSG_ERROR) {
            const auto* error = reinterpret_cast<const nlmsgerr*>(NLMSG_DATA(nl));
            LOG_WARNING(LogModule::RSSI, "nl80211: CTRL_CMD_GETFAMILY errno=" << -error->error);
            return false;
        }
        if (nl->nlmsg_type != GENL_ID_CTRL) {
            LOG_WARNING(LogModule::RSSI, "nl80211: unexpected family reply type=" << nl->nlmsg_type);
            continue;
        }
        auto* attrs = reinterpret_cast<NlAttr*>(reinterpret_cast<uint8_t*>(NLMSG_DATA(nl)) + GENL_HDRLEN);
        int remaining = nl->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;
        while (remaining >= static_cast<int>(sizeof(NlAttr))) {
            if (attrs->len < sizeof(NlAttr) || attrs->len > remaining) break;
            if (attrs->type == kCtrlAttrFamilyId && attrs->len >= sizeof(NlAttr) + sizeof(uint16_t)) {
                std::memcpy(familyId, reinterpret_cast<uint8_t*>(attrs) + sizeof(NlAttr), sizeof(uint16_t));
                return true;
            }
            const size_t step = nlaAlign(attrs->len);
            remaining -= static_cast<int>(step);
            attrs = reinterpret_cast<NlAttr*>(reinterpret_cast<uint8_t*>(attrs) + step);
        }
    }
    return false;
}

bool getInterfaceBssid(const std::string& iface, uint8_t bssid[6]) {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    ifreq request{};
    std::snprintf(request.ifr_name, IFNAMSIZ, "%s", iface.c_str());
    const bool ok = ioctl(fd, SIOCGIWAP, &request) == 0;
    if (ok) std::memcpy(bssid, request.ifr_addr.sa_data, 6);
    close(fd);
    return ok;
}
RssiSample readNl80211RssiWithBssid(const std::string& iface) {
    RssiSample unavailable;
    const int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    if (fd < 0) return unavailable;
    sockaddr_nl local{};
    local.nl_family = AF_NETLINK;
    if (bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
        close(fd); return unavailable;
    }
    timeval timeout{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    uint16_t familyId = 0;
    if (!getNl80211FamilyId(fd, &familyId)) {
        LOG_WARNING(LogModule::RSSI, "nl80211: family lookup failed");
        close(fd); return unavailable;
    }
    const unsigned ifindex = if_nametoindex(iface.c_str());
    if (ifindex == 0) {
        LOG_WARNING(LogModule::RSSI, "nl80211: interface not found: " << iface);
        close(fd); return unavailable;
    }

    std::vector<uint8_t> msg(NLMSG_LENGTH(sizeof(genlmsghdr)), 0);
    auto* header = reinterpret_cast<nlmsghdr*>(msg.data());
    header->nlmsg_len = NLMSG_LENGTH(sizeof(genlmsghdr));
    header->nlmsg_type = familyId;
    header->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    header->nlmsg_seq = 2;
    auto* gen = reinterpret_cast<genlmsghdr*>(NLMSG_DATA(header));
    gen->cmd = kNl80211CmdGetStation;
    gen->version = 0;
    appendNlAttr(msg, kNl80211AttrIfindex, &ifindex, sizeof(ifindex));
    appendNlAttr(msg, kNl80211AttrMac, nullptr, 0);
    header->nlmsg_len = static_cast<uint32_t>(msg.size());
    if (!sendNetlinkMessage(fd, msg)) {
        LOG_WARNING(LogModule::RSSI, "nl80211: GET_STATION send failed: " << strerror(errno));
        close(fd); return unavailable;
    }

    std::array<uint8_t, 16384> response{};
    const ssize_t size = recv(fd, response.data(), response.size(), 0);
    close(fd);
    if (size < 0) return unavailable;
    int remainingSize = static_cast<int>(size);
    for (auto* nl = reinterpret_cast<nlmsghdr*>(response.data());
         NLMSG_OK(nl, remainingSize); nl = NLMSG_NEXT(nl, remainingSize)) {
        if (nl->nlmsg_type == NLMSG_ERROR) return unavailable;
        if (nl->nlmsg_type != familyId) continue;
        auto* attrs = reinterpret_cast<NlAttr*>(reinterpret_cast<uint8_t*>(NLMSG_DATA(nl)) + GENL_HDRLEN);
        int remaining = nl->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;
        while (remaining >= static_cast<int>(sizeof(NlAttr))) {
            if (attrs->len < sizeof(NlAttr) || attrs->len > remaining) break;
            if ((attrs->type & ~kNlaFNested) == kNl80211AttrStaInfo) {
                auto* nested = reinterpret_cast<NlAttr*>(reinterpret_cast<uint8_t*>(attrs) + sizeof(NlAttr));
                int nestedRemaining = attrs->len - sizeof(NlAttr);
                while (nestedRemaining >= static_cast<int>(sizeof(NlAttr))) {
                    if (nested->len < sizeof(NlAttr) || nested->len > nestedRemaining) break;
                    if ((nested->type & ~kNlaFNested) == kNl80211StaInfoSignal && nested->len >= sizeof(NlAttr) + 1) {
                        const int dbm = *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(nested) + sizeof(NlAttr));
                        if (dbm >= 30 && dbm <= 100) return { -dbm, true, false, "nl80211" };
                    }
                    const size_t step = nlaAlign(nested->len);
                    nestedRemaining -= static_cast<int>(step);
                    nested = reinterpret_cast<NlAttr*>(reinterpret_cast<uint8_t*>(nested) + step);
                }
            }
            const size_t step = nlaAlign(attrs->len);
            remaining -= static_cast<int>(step);
            attrs = reinterpret_cast<NlAttr*>(reinterpret_cast<uint8_t*>(attrs) + step);
        }
    }
    return unavailable;
}

}  // namespace

RssiSample readNl80211RssiWithBssid(const std::string& iface, const std::array<uint8_t, 6>& bssid) {
    RssiSample unavailable;
    const int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    if (fd < 0) {
        LOG_WARNING(LogModule::RSSI, "nl80211: socket failed: " << strerror(errno));
        return unavailable;
    }
    sockaddr_nl local{};
    local.nl_family = AF_NETLINK;
    if (bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
        LOG_WARNING(LogModule::RSSI, "nl80211: bind failed: " << strerror(errno));
        close(fd); return unavailable;
    }
    timeval timeout{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    uint16_t familyId = 0;
    if (!getNl80211FamilyId(fd, &familyId)) {
        LOG_WARNING(LogModule::RSSI, "nl80211: family lookup failed");
        close(fd); return unavailable;
    }
    const unsigned ifindex = if_nametoindex(iface.c_str());
    if (ifindex == 0 || std::all_of(bssid.begin(), bssid.end(), [](uint8_t v) { return v == 0; })) {
        LOG_WARNING(LogModule::RSSI, "nl80211: missing interface index or associated BSSID for " << iface);
        close(fd); return unavailable;
    }
    LOG_INFO(LogModule::RSSI, "nl80211: querying station " << iface << " ifindex=" << ifindex
             << " bssid=" << std::hex << static_cast<int>(bssid[0]) << ":"
             << static_cast<int>(bssid[1]) << ":" << static_cast<int>(bssid[2]) << ":"
             << static_cast<int>(bssid[3]) << ":" << static_cast<int>(bssid[4]) << ":"
             << static_cast<int>(bssid[5]) << std::dec);
    std::vector<uint8_t> msg(NLMSG_LENGTH(sizeof(genlmsghdr)), 0);
    auto* header = reinterpret_cast<nlmsghdr*>(msg.data());
    header->nlmsg_len = NLMSG_LENGTH(sizeof(genlmsghdr));
    header->nlmsg_type = familyId;
    header->nlmsg_flags = NLM_F_REQUEST;
    header->nlmsg_seq = 2;
    auto* gen = reinterpret_cast<genlmsghdr*>(NLMSG_DATA(header));
    gen->cmd = NL80211_CMD_GET_STATION;
    gen->version = 0;
    appendNlAttr(msg, NL80211_ATTR_IFINDEX, &ifindex, sizeof(ifindex));
    appendNlAttr(msg, NL80211_ATTR_MAC, bssid.data(), bssid.size());
    header->nlmsg_len = static_cast<uint32_t>(msg.size());
    if (!sendNetlinkMessage(fd, msg)) {
        LOG_WARNING(LogModule::RSSI, "nl80211: GET_STATION send failed: " << strerror(errno));
        close(fd); return unavailable;
    }
    std::array<uint8_t, 16384> response{};
    ssize_t received = recv(fd, response.data(), response.size(), 0);
    close(fd);
    if (received < 0) {
        LOG_WARNING(LogModule::RSSI, "nl80211: GET_STATION receive failed: " << strerror(errno));
        return unavailable;
    }
    int remainingSize = static_cast<int>(received);
    for (auto* nl = reinterpret_cast<nlmsghdr*>(response.data()); NLMSG_OK(nl, remainingSize); nl = NLMSG_NEXT(nl, remainingSize)) {
        if (nl->nlmsg_type == NLMSG_ERROR) {
            auto* error = reinterpret_cast<nlmsgerr*>(NLMSG_DATA(nl));
            LOG_WARNING(LogModule::RSSI, "nl80211: GET_STATION returned errno=" << -error->error);
            return unavailable;
        }
        if (nl->nlmsg_type != familyId) continue;
        auto* attrs = reinterpret_cast<NlAttr*>(reinterpret_cast<uint8_t*>(NLMSG_DATA(nl)) + GENL_HDRLEN);
        int remaining = nl->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;
        while (remaining >= static_cast<int>(sizeof(NlAttr))) {
            if (attrs->len < sizeof(NlAttr) || attrs->len > remaining) break;
            if ((attrs->type & ~kNlaFNested) == NL80211_ATTR_STA_INFO) {
                auto* nested = reinterpret_cast<NlAttr*>(reinterpret_cast<uint8_t*>(attrs) + sizeof(NlAttr));
                int nestedRemaining = attrs->len - sizeof(NlAttr);
                while (nestedRemaining >= static_cast<int>(sizeof(NlAttr))) {
                    if (nested->len < sizeof(NlAttr) || nested->len > nestedRemaining) break;
                    if ((nested->type & ~kNlaFNested) == NL80211_STA_INFO_SIGNAL && nested->len >= sizeof(NlAttr) + 1) {
                        const int dbm = *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(nested) + sizeof(NlAttr));
                        if (dbm >= 30 && dbm <= 100) return {-dbm, true, false, "nl80211"};
                        LOG_WARNING(LogModule::RSSI, "nl80211: station signal out of range: " << -dbm);
                    }
                    const size_t step = nlaAlign(nested->len);
                    nestedRemaining -= static_cast<int>(step);
                    nested = reinterpret_cast<NlAttr*>(reinterpret_cast<uint8_t*>(nested) + step);
                }
            }
            const size_t step = nlaAlign(attrs->len);
            remaining -= static_cast<int>(step);
            attrs = reinterpret_cast<NlAttr*>(reinterpret_cast<uint8_t*>(attrs) + step);
        }
    }
    return unavailable;
}

RssiSample readNl80211Rssi(const std::string& iface) {
    auto client = WiFiRssiClient::getInstance();
    if (!client->connect(iface)) return {};
    const auto bssid = client->getAssociatedBssid();
    return readNl80211RssiWithBssid(iface, bssid);
}

int readProcWirelessRssi(const std::string& iface, const std::string& procPath) {
    std::ifstream in(procPath);
    if (!in) return -1000;

    std::string line;
    while (std::getline(in, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos || line.substr(0, colon).find(iface) == std::string::npos) continue;
        std::istringstream fields(line.substr(colon + 1));
        std::string status;
        double link = 0.0;
        double level = 0.0;
        if (!(fields >> status >> link >> level)) return -1000;
        const int rssi = static_cast<int>(level);
        if (rssi >= -100 && rssi <= -30) return rssi;

        // Some drivers expose only a valid link-quality value (0..70).
        if (link >= 0.0 && link <= 70.0) {
            return static_cast<int>(-100.0 + (link / 70.0) * 70.0);
        }
        return -1000;
    }
    return -1000;
}

int parseWifiRssiResponse(const std::string& resp) {
    size_t pos = resp.find("RSSI=");
    if (pos == std::string::npos) return -1000;

    int rssi = 0;
    if (std::sscanf(resp.c_str() + pos, "RSSI=%d", &rssi) != 1 || rssi < -100 || rssi > -30) {
        LOG_WARNING(LogModule::RSSI, "invalid Wi-Fi RSSI returned by wpa_supplicant: " << rssi);
        return -1000;
    }
    return rssi;
}

}  // namespace weaknet_dbus


// ---------------------------------------------------------------------------
// 匿名命名空间：辅助工具函数（文件存在性检测、目录创建、拉起 wpa_supplicant）
// ---------------------------------------------------------------------------

/** @brief 使用 stat() 检查路径是否存在 */
static bool pathExists(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0;
}

/** @brief 确保目录存在，不存在则尝试创建（权限 0775） */
static bool ensureDir(const std::string& d) {
    struct stat st{};
    if (::stat(d.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    return ::mkdir(d.c_str(), 0775) == 0;
}

/**
 * @brief 尝试 fork+exec 拉起 wpa_supplicant 守护进程
 *
 * 工作流程：
 * 1. 定位 wpa_supplicant 二进制路径（/sbin/ → /usr/sbin/ 后备）
 * 2. 定位配置文件（WPA_SUPPLICANT_CONF 环境变量 → /etc/wpa_supplicant/）
 * 3. 确保 ctrl 目录存在
 * 4. fork 子进程，execl 调用 wpa_supplicant -B（后台模式）-i <iface> -c <conf> -C <ctrlDir>
 * 5. 父进程轮询等待控制 socket 文件出现（最多 2 秒）
 *
 * @param iface   无线网卡接口名（如 "wlan0"）
 * @param ctrlDir wpa_supplicant 控制 socket 所在目录
 *
 * @return true  - 成功拉起且控制 socket 文件已就绪
 *         false - 任意步骤失败
 *
 * @note 需要 root 权限，否则 fork/exec 可能因权限不足失败；
 *       控制 socket 文件路径格式为 <ctrlDir>/<iface>（如 /run/wpa_supplicant/wlan0）
 */
static bool launchWpaSupplicant(const std::string& iface, const std::string& ctrlDir) {
    LOG_INFO(LogModule::RSSI, "launchWpaSupplicant: attempting to start wpa_supplicant for " << iface);
    // 定位二进制路径
    const char* bin = "/sbin/wpa_supplicant";
    if (!pathExists(bin)) bin = "/usr/sbin/wpa_supplicant";
    if (!pathExists(bin)) {
        LOG_ERROR(LogModule::RSSI, "launchWpaSupplicant: wpa_supplicant binary not found");
        return false;
    }
    // 定位配置文件：优先使用环境变量，后备 /etc/wpa_supplicant/wpa_supplicant.conf
    const char* conf = std::getenv("WPA_SUPPLICANT_CONF");
    if (!conf || !*conf) conf = "/etc/wpa_supplicant/wpa_supplicant.conf";
    if (!pathExists(conf)) {
        LOG_ERROR(LogModule::RSSI, "launchWpaSupplicant: config file not found: " << conf);
        return false;
    }
    if (!ensureDir(ctrlDir)) {
        LOG_ERROR(LogModule::RSSI, "launchWpaSupplicant: failed to create ctrl dir: " << ctrlDir);
        return false;
    }

    // fork() 创建子进程
    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR(LogModule::RSSI, "launchWpaSupplicant: fork() failed: " << strerror(errno));
        return false;
    }

    if (pid == 0) {
        // ========== 子进程：重定向 stdout/stderr 到 /dev/null ==========
        // 避免 wpa_supplicant 的启动日志污染父进程输出
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        // execl 拉起 wpa_supplicant：-B 后台模式，-i 指定接口，-C 指定控制 socket 目录
        execl(bin, "wpa_supplicant", "-B", "-i", iface.c_str(),
              "-c", conf, "-C", ctrlDir.c_str(), (char*)nullptr);

        // exec 失败（正常情况下不会到达此处，exec 成功会替换子进程映像）
        _exit(127);
    }

    // ========== 父进程：非阻塞等待子进程状态 ==========
    int status;
    waitpid(pid, &status, WNOHANG);

    // 轮询等待控制 socket 文件出现（最多 20 次 × 100ms = 2 秒）
    const std::string sockPath = ctrlDir + "/" + iface;
    for (int i = 0; i < 20; ++i) {
        if (pathExists(sockPath)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

// ---------------------------------------------------------------------------
// WiFiRssiClient 单例实现
// ---------------------------------------------------------------------------

std::once_flag WiFiRssiClient::s_onceFlag;
std::shared_ptr<WiFiRssiClient> WiFiRssiClient::s_instance;

/** @brief 获取单例实例，线程安全（std::call_once） */
std::shared_ptr<WiFiRssiClient> WiFiRssiClient::getInstance() {
    std::call_once(s_onceFlag, [](){
        LOG_INFO(LogModule::NETWORK, "Creating WiFiRssiClient instance");
        s_instance = std::make_shared<WiFiRssiClient>();
        LOG_INFO(LogModule::NETWORK, "WiFiRssiClient instance created");
    });
    LOG_INFO(LogModule::NETWORK, "Returning WiFiRssiClient instance");
    return s_instance;
}

WiFiRssiClient::WiFiRssiClient() = default;

/**
 * @brief 析构：关闭 socket、清理本地绑定的临时 socket 文件
 */
WiFiRssiClient::~WiFiRssiClient() {
    if (sockfd_ != -1) {
        close(sockfd_);
        sockfd_ = -1;
    }
    // unlink 本地临时 socket 文件，避免 /tmp 残留
    if (!localSockPath_.empty()) {
        unlink(localSockPath_.c_str());
    }
}

/**
 * @brief 连接到指定接口的 wpa_supplicant 控制 socket
 *
 * 工作流程：
 * 1. 创建 AF_UNIX/SOCK_DGRAM socket
 * 2. bind 本地临时路径 /tmp/wpa_ctrl_<pid>_<iface>
 * 3. 按候选列表依次尝试 connect 远端 wpa_supplicant socket
 * 4. 若所有候选均失败，尝试自动拉起 wpa_supplicant 后重试
 *
 * @param ifaceName 无线网卡接口名（如 "wlan0"）
 * @param ctrlDir   可选的 wpa_supplicant 控制目录（可传空串，将自动探测）
 *
 * @return true  - 成功建立连接
 *         false - 创建 socket、bind、connect 全部失败
 */
bool WiFiRssiClient::connect(const std::string& ifaceName, const std::string& ctrlDir) {
    LOG_INFO(LogModule::NETWORK, "connect: starting, iface=" << ifaceName << ", ctrlDir=" << ctrlDir);

    // 先释放上一轮的 socket 与本地绑定路径：本方法每个采集周期都会被调用，
    // 若直接覆盖 sockfd_ 会泄漏旧 fd（约每 10s 每个 Wi-Fi 接口 1 个，
    // 数小时后耗尽 ulimit -n，RSSI 监控永久失效）。
    if (sockfd_ != -1) {
        ::close(sockfd_);
        sockfd_ = -1;
    }
    if (!localSockPath_.empty()) {
        ::unlink(localSockPath_.c_str());
        localSockPath_.clear();
    }

    iface_ = ifaceName;

    // 创建 Unix 域数据报 socket
    sockfd_ = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sockfd_ < 0) {
        LOG_ERROR(LogModule::NETWORK, "socket() failed");
        return false;
    }
    LOG_INFO(LogModule::NETWORK, "connect: socket created, fd=" << sockfd_);

    // 绑定本地临时路径
    if (!bindLocal()) {
        LOG_ERROR(LogModule::NETWORK, "connect: bindLocal() failed");
        return false;
    }
    LOG_INFO(LogModule::NETWORK, "connect: bindLocal() succeeded");

    // 候选目录列表：参数 → 环境变量 → 标准路径
    std::vector<std::string> candidates;
    if (!ctrlDir.empty()) candidates.push_back(ctrlDir);
    const char* envDir = std::getenv("WPA_CTRL_DIR");
    if (envDir && *envDir) candidates.emplace_back(envDir);
    candidates.emplace_back("/run/wpa_supplicant");   // systemd 时代标准路径
    candidates.emplace_back("/var/run/wpa_supplicant"); // SysV 风格后备

    LOG_INFO(LogModule::NETWORK, "connect: trying " << candidates.size() << " candidate directories");
    for (const auto& d : candidates) {
        LOG_INFO(LogModule::NETWORK, "connect: trying directory " << d);
        ctrlDir_ = d;
        if (connectRemote()) {
            LOG_INFO(LogModule::NETWORK, "connect: connected to " << d);
            return true;
        }
        LOG_ERROR(LogModule::NETWORK, "connect: failed to connect to " << d);
    }

    // ==================== 自动拉起 wpa_supplicant 后备方案 ====================
    // 优先 /run/wpa_supplicant（systemd），不存在则尝试 /var/run/wpa_supplicant
    std::string pref = "/run/wpa_supplicant";
    if (!ensureDir(pref)) pref = "/var/run/wpa_supplicant";
    if (ensureDir(pref)) {
        if (launchWpaSupplicant(iface_, pref)) {
            ctrlDir_ = pref;
            if (connectRemote()) {
                return true;
            }
        }
    }

    // 全部失败，清理 socket 资源
    LOG_ERROR(LogModule::NETWORK, "unable to connect to wpa_supplicant control socket for iface '" << iface_ << "' (auto-start may require root)");
    close(sockfd_);
    sockfd_ = -1;
    if (!localSockPath_.empty()) {
        unlink(localSockPath_.c_str());
        localSockPath_.clear();
    }
    return false;
}

/**
 * @brief 绑定本地临时 Unix socket 地址
 *
 * 使用格式 /tmp/wpa_ctrl_<pid>_<iface>，确保多进程、多接口场景下路径唯一。
 * 绑定前先 unlink 以防崩溃后的残留文件。
 *
 * @return true  - bind 成功
 *         false - bind 失败（socket 已在析构中关闭，无需额外处理）
 */
bool WiFiRssiClient::bindLocal() {
    struct sockaddr_un local{};
    local.sun_family = AF_UNIX;
    // 构造唯一的本地临时 socket 路径
    char tmp[108]{}; // Linux sun_path 最大 108 字节
    std::snprintf(tmp, sizeof(tmp), "/tmp/wpa_ctrl_%d_%s", getpid(), iface_.c_str());
    localSockPath_ = tmp;
    std::strncpy(local.sun_path, localSockPath_.c_str(), sizeof(local.sun_path) - 1);

    // 先 unlink，避免上次崩溃后的残留文件导致 bind 失败
    unlink(local.sun_path);
    if (::bind(sockfd_, reinterpret_cast<struct sockaddr*>(&local), sizeof(local)) < 0) {
        LOG_ERROR(LogModule::NETWORK, "bind() failed: " << local.sun_path);
        close(sockfd_);
        sockfd_ = -1;
        return false;
    }
    return true;
}

/**
 * @brief connect 到远端 wpa_supplicant 控制 socket
 *
 * 控制 socket 路径格式为 <ctrlDir>/<iface>，由 wpa_supplicant 在 -C 目录下自动创建。
 * 设置 1 秒 SO_RCVTIMEO 防止后续 recv 永久阻塞。
 *
 * @return true  - connect 成功
 *         false - connect 失败（wpa_supplicant 可能未启动）
 */
bool WiFiRssiClient::connectRemote() {
    struct sockaddr_un dest{};
    dest.sun_family = AF_UNIX;
    std::string destPath = ctrlDir_ + "/" + iface_;
    // sun_path 最大 108 字节，超出则无法绑定
    if (destPath.size() >= sizeof(dest.sun_path)) {
        LOG_ERROR(LogModule::NETWORK, "dest path too long: " << destPath);
        return false;
    }
    std::strcpy(dest.sun_path, destPath.c_str());

    // 设置接收超时（SO_RCVTIMEO），防止 recv 永久阻塞
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    if (::setsockopt(sockfd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        LOG_ERROR(LogModule::NETWORK, "setsockopt() failed");
    }

    if (::connect(sockfd_, reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest)) < 0) {
        LOG_ERROR(LogModule::RSSI, "connectRemote: connect() failed to " << destPath << ": " << strerror(errno));
        return false;
    }
    return true;
}

/**
 * @brief 发送 wpa_ctrl 命令并等待响应
 *
 * 发送指定命令字符串到 wpa_supplicant，阻塞等待（带 1 秒超时）接收响应。
 *
 * @param cmd wpa_ctrl 协议命令（如 "SIGNAL_POLL\n"、"SCAN\n" 等）
 *
 * @return 服务器返回的原始响应字符串；失败或超时返回空串
 */
std::string WiFiRssiClient::sendCommand(const std::string& cmd) {
    if (sockfd_ == -1) return {};
    // 发送命令（使用 SOCK_DGRAM，send 不保证可靠但无需额外处理）
    if (::send(sockfd_, cmd.c_str(), cmd.size(), 0) < 0) {
        LOG_ERROR(LogModule::NETWORK, "send() failed");
        return {};
    }

    // 再次确保接收超时（某些场景下 setsockopt 可能被重置）
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    if (::setsockopt(sockfd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        LOG_ERROR(LogModule::NETWORK, "setsockopt() failed");
    }

    // 接收响应（wpa_supplicant 响应通常小于 1KB，4096 足够）
    char buf[4096];
    ssize_t n = ::recv(sockfd_, buf, sizeof(buf) - 1, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            LOG_ERROR(LogModule::NETWORK, "recv() timeout");
        } else {
            LOG_ERROR(LogModule::NETWORK, "recv() failed: " << strerror(errno));
        }
        return {};
    }
    buf[n] = '\0';
    return std::string(buf, static_cast<size_t>(n));
}

/**
 * @brief 获取当前接口的 Wi-Fi RSSI 值
 *
 * 发送 "SIGNAL_POLL\n" 命令并从响应中解析 "RSSI=<值>" 字段。
 * 响应格式示例：
 *
 *     RSSI=-42
 *     LINKSPEED=65000
 *     NOISE=9999
 *     FREQ=5745
 *
 * @return RSSI 值（单位 dBm，通常范围 [-100, 0]）；
 *         连接失败或非 Wi-Fi 接口返回 -1000（哨兵值）
 */
std::array<uint8_t, 6> WiFiRssiClient::getAssociatedBssid() {
    std::array<uint8_t, 6> bssid{};
    const std::string status = sendCommand("STATUS\n");
    const std::string key = "bssid=";
    const size_t pos = status.find(key);
    if (pos == std::string::npos) {
        LOG_WARNING(LogModule::RSSI, "nl80211 RSSI: wpa STATUS has no associated BSSID for " << iface_);
        return bssid;
    }
    const size_t end = status.find_first_of("\r\n", pos + key.size());
    const std::string value = status.substr(pos + key.size(), end - (pos + key.size()));
    unsigned int octets[6]{};
    if (std::sscanf(value.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x",
                    &octets[0], &octets[1], &octets[2], &octets[3], &octets[4], &octets[5]) != 6) {
        return bssid;
    }
    for (size_t i = 0; i < bssid.size(); ++i) bssid[i] = static_cast<uint8_t>(octets[i]);
    return bssid;
}
int WiFiRssiClient::getRssi() {
    std::string resp = sendCommand("SIGNAL_POLL\n");
    if (resp.empty()) return -1000;
    return parseWifiRssiResponse(resp);
}

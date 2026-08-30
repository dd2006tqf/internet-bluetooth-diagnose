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
int WiFiRssiClient::getRssi() {
    std::string resp = sendCommand("SIGNAL_POLL\n");
    if (resp.empty()) return -1000;

    // 查找 "RSSI=" 子串位置
    size_t pos = resp.find("RSSI=");
    if (pos != std::string::npos) {
        int rssi = 0;
        // sscanf 从 "RSSI=" 后一位开始解析整数
        if (std::sscanf(resp.c_str() + pos, "RSSI=%d", &rssi) == 1) {
            return rssi;
        }
    }
    return -1000;
}

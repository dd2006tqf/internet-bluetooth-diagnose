/**
 * @file net_wifiriss.h
 * @brief Wi-Fi RSSI 客户端（通过 wpa_supplicant UNIX DGRAM socket）
 *
 * 通过连接 wpa_supplicant 控制接口（/var/run/wpa_supplicant/<iface>），
 * 发送 "SIGNAL_POLL" 命令并解析返回的 BSS 参数中的 RSSI 值。
 *
 * 替代方案：
 *   - nl80211（cfg80211 netlink）：无需 wpa_supplicant，但更复杂
 *   - iw 命令行：简单但 fork 开销大，不适合高频采样
 *
 * 线程安全：getInstance() 使用 std::once_flag 保证线程安全懒汉初始化。
 *           getRssi() 内部使用 sendCommand，依赖 socket fd 的线程安全。
 */

#pragma once

#include <string>
#include <memory>
#include <mutex>
#include <array>
#include <cstdint>

namespace weaknet_dbus {

struct RssiSample {
    int dbm = -1000;
    bool valid = false;
    bool estimated = false;
    std::string source;
};

RssiSample readNl80211Rssi(const std::string& iface);

/// Parse and validate a SIGNAL_POLL response; invalid driver values return -1000.
int parseWifiRssiResponse(const std::string& response);

/// Read Linux wireless statistics and return a plausible dBm estimate.
/// Invalid driver level falls back to link-quality conversion; unavailable returns -1000.
int readProcWirelessRssi(const std::string& iface,
                         const std::string& procPath = "/proc/net/wireless");
}  // namespace weaknet_dbus

/**
 * @brief wpa_supplicant RSSI 查询客户端
 *
 * 连接流程：bindLocal() → connectRemote()；
 * 每次查询：sendCommand("SIGNAL_POLL") → 解析返回文本 → 提取 RSSI 行。
 *
 * 哨兵值约定：getRssi() 失败时返回 -1000（与 NetInfo::rssi_dbm_ 哨兵一致）。
 */
class WiFiRssiClient {
public:
    WiFiRssiClient();
    ~WiFiRssiClient();

    /// 线程安全懒汉单例（可选，也可不通过单例直接构造）
    static std::shared_ptr<WiFiRssiClient> getInstance();

    /**
     * @brief 连接到指定接口的 wpa_supplicant 控制通道
     *
     * @param ifaceName  网卡名（如 "wlan0"）
     * @param ctrlDir    控制目录，默认 "/var/run/wpa_supplicant"
     * @return true  连接成功
     * @return false 路径不存在 / 权限不足 / wpa_supplicant 未运行
     */
    bool connect(const std::string& ifaceName, const std::string& ctrlDir = "/var/run/wpa_supplicant");

    /**
     * @brief 发送 SIGNAL_POLL 并解析 RSSI
     *
     * @return RSSI（dBm，如 -45）；失败返回哨兵值 -1000
     */
    int getRssi();

    /// 读取当前关联 AP 的 BSSID；失败返回全零地址。
    std::array<uint8_t, 6> getAssociatedBssid();

    /// 读取当前关联 AP 的频率（MHz，如 2462、5180）；失败返回 0。
    int getFrequency();

    /**
     * @brief 在**单次持锁**内完成 connect + 取 BSSID。
     *
     * readNl80211Rssi 需要"连上某张网卡并读取它的 BSSID"这一原子组合。
     * 若拆成 connect() 再 getAssociatedBssid() 两次调用，中间会被另一线程
     * （逐网卡采集时对别的网卡调 connect）插入，于是取到的是另一张网卡的
     * BSSID——RSSI 采样会被归到错误的 AP 上。
     *
     * @param ifaceName  网卡名
     * @param ctrlDir    控制目录
     * @param bssid_out  成功时写入 BSSID；失败时不修改
     * @return true 连接成功（BSSID 可能仍为全零，表示尚未关联）
     */
    bool connectAndGetBssid(const std::string& ifaceName, const std::string& ctrlDir,
                            std::array<uint8_t, 6>* bssid_out);

private:
    /**
     * @brief 串行化对 wpa_supplicant 控制通道的所有访问。
     *
     * 本类是**进程级单例**，但有两个线程同时使用它：rssi 采集线程
     * （readNl80211Rssi → connect）与网络质量线程（getFrequency）。
     * connect() 每次都会 close 并重建 sockfd_，若不加锁，另一个线程会在
     * fd 被关闭的窗口里 send/recv——轻则失败，重则写入一个已被其它子系统
     * 复用的 fd；iface_/ctrlDir_/localSockPath_ 的 std::string 竞争同理。
     *
     * 约定：仅在公共入口（connect/getRssi/getAssociatedBssid/getFrequency）
     * 加锁；sendCommand/bindLocal/connectRemote 等私有 helper 假定调用方已持锁，
     * 因此本互斥无需可重入。
     */
    mutable std::mutex mutex_;

    /// 不加锁实现：调用方必须已持 mutex_（供 connectAndGetBssid 组合调用）。
    bool connectLocked(const std::string& ifaceName, const std::string& ctrlDir);
    std::array<uint8_t, 6> getAssociatedBssidLocked();

    int sockfd_ = -1;              ///< UNIX DGRAM socket fd（连接后有效）
    std::string iface_;            ///< 当前绑定的网卡名
    std::string ctrlDir_;          ///< 控制目录
    std::string localSockPath_;    ///< 本地 socket 路径（bind 创建的临时文件）

    static std::once_flag s_onceFlag;
    static std::shared_ptr<WiFiRssiClient> s_instance;

    /// 创建本地 UNIX DGRAM socket 并 bind（生成唯一路径避免冲突）
    bool bindLocal();
    /// connect 到 wpa_supplicant 的控制 socket（/var/run/wpa_supplicant/<iface>）
    bool connectRemote();
    /**
     * @brief 发送命令并返回响应文本
     * @param cmd  wpa_supplicant 命令（如 "SIGNAL_POLL"、"STATUS"）
     * @return 响应文本；失败返回空字符串
     */
    std::string sendCommand(const std::string& cmd);
};
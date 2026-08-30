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

private:
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

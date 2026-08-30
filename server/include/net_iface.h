/**
 * @file net_iface.h
 * @brief 网络接口管理器（单例）
 *
 * 通过读取系统路由表和接口标志，确定当前具备上网能力的网卡。
 * 判断标准：接口 UP 且存在 IPv4 或 IPv6 默认路由。
 *
 * 实现依据：
 *   - Netlink RTM_GETROUTE：查询默认路由（目标 = 0.0.0.0/0 或 ::/0）
 *   - /sys/class/net/<iface>/operstate：判断接口是否 UP
 *
 * 线程安全：getInstance() 使用 std::once_flag 保证线程安全懒汉初始化。
 *           getInternetInterfaces() 内部无状态修改，可多线程并发调用。
 */

#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <memory>

/**
 * @brief 网络接口管理器
 *
 * 单例模式，通过 getInstance() 获取共享实例。
 * 用于替代硬编码网卡名（如 "wlan0"），实现多网卡环境自适应。
 */
class NetInterfaceManager {
public:
    /// 线程安全懒汉单例（std::once_flag 保证只构造一次）
    static std::shared_ptr<NetInterfaceManager> getInstance();

    /**
     * @brief 获取当前具备上网能力的网卡名列表
     *
     * 判断条件：
     *   1. 接口 operational state = "up"
     *   2. 存在该接口的默认 IPv4 或 IPv6 路由
     *
     * @return 网卡名向量（如 ["wlan0"]）；无上网能力时返回空
     */
    std::vector<std::string> getInternetInterfaces();

    ~NetInterfaceManager() = default;

private:
    NetInterfaceManager() = default;
    NetInterfaceManager(const NetInterfaceManager&) = delete;
    NetInterfaceManager& operator=(const NetInterfaceManager&) = delete;

    static std::once_flag s_onceFlag;
    static std::shared_ptr<NetInterfaceManager> s_instance;
};

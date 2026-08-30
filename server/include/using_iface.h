/**
 * @file using_iface.h
 * @brief 当前上网网卡管理器（单例，后台监听 + 快速查询）
 *
 * 通过 Netlink 监听路由变化事件（RTM_NEWROUTE / RTM_DELROUTE），
 * 实时跟踪"哪个网卡具备默认路由"，供 WeakNetMgr 和 D-Bus 服务查询。
 *
 * 对比 NetInterfaceManager：
 *   - NetInterfaceManager：被动查询（按需扫路由表）
 *   - UsingInterfaceManager：主动推送（后台监听 + 缓存状态）
 *   两者可独立使用，本类更适合高频查询场景。
 *
 * 线程安全：stateMutex_ 保护 currentIfName_ / methodFlags_；
 *           Impl 内部的 Netlink 监听线程独立运行，通过 mutex 写状态。
 */

#pragma once

#include <string>
#include <memory>
#include <mutex>
#include <cstdint>

/// 方法标志位：当前上网方式（可按位组合）
namespace UsingMethodFlag {
    static constexpr uint32_t IPv4Default = 0x1;  ///< 存在 IPv4 默认路由
    static constexpr uint32_t IPv6Default = 0x2;  ///< 存在 IPv6 默认路由
}

/**
 * @brief 当前上网网卡管理器
 *
 * 单例模式。start() 后内部启动 Netlink 监听线程，
 * 每当路由变化时自动更新 currentIfName_。
 */
class UsingInterfaceManager {
public:
    /// 线程安全懒汉单例
    static std::shared_ptr<UsingInterfaceManager> getInstance();

    /**
     * @brief 启动后台监听（可重复调用，幂等）
     *
     * 内部会启动一个 Netlink 监听线程，阻塞在 recv() 上等待路由事件。
     * 如果已经启动则直接返回。
     */
    void start();

    /**
     * @brief 获取当前用于上网的网卡名
     * @return 网卡名（如 "wlan0"）；无默认路由时返回空字符串
     */
    std::string getCurrentInterface();

    /**
     * @brief 获取当前上网方法标志位
     * @return UsingMethodFlag 组合（如 IPv4Default | IPv6Default）
     */
    uint32_t getMethodFlags();

    ~UsingInterfaceManager();

private:
    UsingInterfaceManager();
    UsingInterfaceManager(const UsingInterfaceManager&) = delete;
    UsingInterfaceManager& operator=(const UsingInterfaceManager&) = delete;

    static std::once_flag s_onceFlag;
    static std::shared_ptr<UsingInterfaceManager> s_instance;

    std::mutex stateMutex_;     ///< 保护 currentIfName_ / methodFlags_
    std::string currentIfName_;
    uint32_t methodFlags_ = 0;

    // 平台相关实现细节（隐藏 Netlink 监听线程）
    struct Impl;
    Impl* impl_ = nullptr;
};

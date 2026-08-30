/**
 * @file using_iface.cpp
 * @brief 通过 netlink rtnetlink 异步事件跟踪"当前上网网卡"
 *
 * @details 本文件实现 UsingInterfaceManager 单例类，通过 pimpl 模式在后台线程中
 *          持续监听 netlink（AF_NETLINK + NETLINK_ROUTE）接口变更和路由变更事件，
 *          实时维护"当前用于上网的接口"及其选择方法标记（IPv4 还是 IPv6 默认路由）。
 *
 *          与 net_iface.cpp（一次性快照 SnapshotCollector）的区别：
 *          - 本文件是**事件驱动**的：open socket → dumpInitial（获取基准状态）→
 *            eventLoop（阻塞式 recvmsg + 超时轮询，持续处理内核推送的 RTM_NEWLINK/
 *            RTM_NEWROUTE 等事件）
 *          - 接口状态（ifindexToName / upIfaces / v4Default / v6Default）由事件循环
 *            持续更新，状态变化后 publishState 刷新对外暴露的 currentIfName_
 *
 *          选择"当前上网接口"的策略（publishState）：
 *          1. 优先选有 IPv4 默认路由且处于 UP 状态的接口
 *          2. 若无 IPv4 默认路由，则选有 IPv6 默认路由且处于 UP 状态的接口
 *          3. 记录 methodFlags_（UsingMethodFlag::IPv4Default / IPv6Default）
 *
 *          析构安全：析构时先置 running=false → join 后台线程 → delete impl_，
 *          确保事件循环线程在访问 impl_ 成员前安全退出，不会出现 use-after-free。
 *
 * @note 关键系统接口：
 *       - socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE) — 创建 rtnetlink socket
 *       - bind() 订阅 RTMGRP_LINK / RTMGRP_IPV4_ROUTE / RTMGRP_IPV6_ROUTE 组播
 *       - setsockopt(SO_RCVTIMEO) — 设置 1 秒超时，避免 recvmsg 永久阻塞
 *       - recvmsg() — 循环接收内核推送的事件消息（RTM_NEWLINK/DEL LINK/NEWROUTE/DELROUTE）
 *       - sendmsg() + NLM_F_DUMP — 启动时请求初始状态基准
 *
 *       非 Linux 平台提供空实现（start()/getCurrentInterface() 返回空）。
 */

#include "using_iface.h"
#include "logger.hpp"

using namespace weaknet_dbus;

#include <atomic>
#include <thread>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cstring>
#include <iostream>

#if defined(__linux__)
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <net/if.h>
#include <stdexcept>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// UsingInterfaceManager 单例静态成员
// ---------------------------------------------------------------------------

std::once_flag UsingInterfaceManager::s_onceFlag;
std::shared_ptr<UsingInterfaceManager> UsingInterfaceManager::s_instance;

/** @brief 获取单例实例，线程安全（std::call_once） */
std::shared_ptr<UsingInterfaceManager> UsingInterfaceManager::getInstance() {
    std::call_once(s_onceFlag, [](){ s_instance = std::shared_ptr<UsingInterfaceManager>(new UsingInterfaceManager()); });
    return s_instance;
}

UsingInterfaceManager::UsingInterfaceManager() = default;
// 析构体在下方 Impl 完整定义之后提供（pimpl 需要完整类型才能 delete/join impl_）

// ===========================================================================
// Impl（pimpl 完整类型）：封装 netlink 事件循环的所有内部状态
// ===========================================================================

struct UsingInterfaceManager::Impl {
#if defined(__linux__)
    int nlSocket = -1;                // rtnetlink socket fd
    std::thread worker;               // 后台事件循环线程
    std::atomic<bool> running{false};  // 线程运行标志（析构时置 false 触发优雅退出）
    std::unordered_map<int, std::string> ifindexToName;  // ifindex → 接口名
    std::unordered_set<int> upIfaces;                     // UP 状态的非 loopback 接口
    std::unordered_set<int> v4Default;                    // 有 IPv4 默认路由的接口 ifindex
    std::unordered_set<int> v6Default;                    // 有 IPv6 默认路由的接口 ifindex

    /** @brief 将 fd 设置为非阻塞模式（fcntl F_GETFL / F_SETFL + O_NONBLOCK） */
    static void setNonBlocking(int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags == -1) {
            LOG_ERROR(LogModule::INTERFACE, "setNonBlocking: fcntl(F_GETFL) failed: " << strerror(errno));
            throw std::runtime_error("fcntl(F_GETFL) failed");
        }
        if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            LOG_ERROR(LogModule::INTERFACE, "setNonBlocking: fcntl(F_SETFL) failed: " << strerror(errno));
            throw std::runtime_error("fcntl(F_SETFL) failed");
        }
    }

    /**
     * @brief 创建 rtnetlink socket 并绑定事件组播
     *
     * 与 net_iface.cpp 中 openSocket 类似，但本 Impl 还会在 eventLoop 中
     * 通过 SO_RCVTIMEO 设置超时，以便响应 running=false 的停止请求。
     */
    void openSocket() {
        nlSocket = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
        if (nlSocket < 0) {
            LOG_ERROR_F(LogModule::INTERFACE, "openSocket: socket(AF_NETLINK) failed: %s", strerror(errno));
            throw std::runtime_error("socket(AF_NETLINK) failed");
        }
        sockaddr_nl addr{};
        addr.nl_family = AF_NETLINK;
        // 订阅接口变更 + 路由变更组播，内核会主动推送 RTM_NEWLINK/DEL/NEWROUTE/DELROUTE 事件
        addr.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE;
        if (bind(nlSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            throw std::runtime_error("bind(AF_NETLINK) failed");
        }
        setNonBlocking(nlSocket);
    }

    /**
     * @brief 发送一条 netlink dump 请求（rtgenmsg 通用格式）
     *
     * @param type   nlmsg_type：RTM_GETLINK 或 RTM_GETROUTE
     * @param flags  额外 nlmsg_flags（通常 NLM_F_DUMP）
     * @param family AF_PACKET（link）/ AF_INET / AF_INET6
     */
    void sendReq(uint16_t type, uint16_t flags, uint8_t family) {
        struct { nlmsghdr nlh; rtgenmsg gen; } req{};
        req.nlh.nlmsg_len = sizeof(req);
        req.nlh.nlmsg_type = type;
        req.nlh.nlmsg_flags = flags | NLM_F_REQUEST;
        req.nlh.nlmsg_seq = static_cast<uint32_t>(::time(nullptr));
        req.nlh.nlmsg_pid = 0;
        req.gen.rtgen_family = family;
        sockaddr_nl nladdr{}; nladdr.nl_family = AF_NETLINK;
        struct iovec iov{ &req, sizeof(req) };
        struct msghdr msg{}; msg.msg_name = &nladdr; msg.msg_namelen = sizeof(nladdr); msg.msg_iov = &iov; msg.msg_iovlen = 1;
        if (sendmsg(nlSocket, &msg, 0) < 0) {
            LOG_ERROR_F(LogModule::INTERFACE, "sendReq: sendmsg failed: %s", strerror(errno));
            throw std::runtime_error("sendmsg failed");
        }
    }

    /** @brief 启动时 dump 初始状态：link + IPv4 route + IPv6 route，填充 upIfaces / v4Default / v6Default */
    void dumpInitial() {
        sendReq(RTM_GETLINK, NLM_F_DUMP, AF_PACKET);
        recvDump();
        sendReq(RTM_GETROUTE, NLM_F_DUMP, AF_INET);
        recvDump();
        sendReq(RTM_GETROUTE, NLM_F_DUMP, AF_INET6);
        recvDump();
    }

    /**
     * @brief 通用 netlink rtattr 属性解析器（模板，自动推导 attrs 数组大小）
     * 遍历 RTA_OK / RTA_NEXT，将每个属性按 rta_type 填入对应槽位。
     */
    template <typename T, size_t N>
    static void parseAttrs(struct rtattr* rta, int len, T (&attrs)[N]) {
        std::fill(std::begin(attrs), std::end(attrs), nullptr);
        while (RTA_OK(rta, len)) {
            if (rta->rta_type < N) attrs[rta->rta_type] = rta;
            rta = RTA_NEXT(rta, len);
        }
    }

    /**
     * @brief 处理一条 RTM_NEWLINK 事件
     *
     * 与 net_iface::SnapshotCollector::handleLink 类似：解析 IFLA_IFNAME 填充
     * ifindexToName，根据 IFF_LOOPBACK / IFF_UP 维护 upIfaces 集合。
     * 若接口整体 down（ifi_change==~0U 或 flags==0），则同时清除其在默认路由集合中的记录。
     */
    void handleLink(ifinfomsg* info, void* attrHead, int attrLen) {
        struct rtattr* attrs[IFLA_MAX + 1];
        parseAttrs(reinterpret_cast<struct rtattr*>(attrHead), attrLen, attrs);
        int ifindex = info->ifi_index;
        // IFLA_IFNAME：接口名
        if (attrs[IFLA_IFNAME]) {
            char name[IFNAMSIZ]{};
            std::snprintf(name, sizeof(name), "%s", reinterpret_cast<char*>(RTA_DATA(attrs[IFLA_IFNAME])));
            ifindexToName[ifindex] = name;
        }
        bool isLoopback = (info->ifi_flags & IFF_LOOPBACK) != 0;
        bool isUp = (info->ifi_flags & IFF_UP) != 0;
        if (isUp && !isLoopback) upIfaces.insert(ifindex); else upIfaces.erase(ifindex);
        // 接口整体 down/消失：同时从默认路由集合中移除
        if (info->ifi_change == ~0U || (info->ifi_flags == 0)) {
            v4Default.erase(ifindex);
            v6Default.erase(ifindex);
        }
    }

    /** @brief 判断一条路由是否为默认路由（dst_len==0 + main/default table + universe scope） */
    static bool isDefaultRoute(const rtmsg* rtm) {
        return rtm->rtm_dst_len == 0 &&
               (rtm->rtm_table == RT_TABLE_MAIN || rtm->rtm_table == RT_TABLE_DEFAULT || rtm->rtm_table == RT_TABLE_UNSPEC) &&
               (rtm->rtm_scope == RT_SCOPE_UNIVERSE || rtm->rtm_scope == RT_SCOPE_NOWHERE || rtm->rtm_scope == RT_SCOPE_SITE);
    }

    /**
     * @brief 处理一条 RTM_NEWROUTE / RTM_DELROUTE 事件
     *
     * 过滤非默认路由，提取 RTA_OIF（出站 ifindex）和 RTA_GATEWAY。
     * 按协议族更新 v4Default 或 v6Default 集合。
     */
    void handleRoute(rtmsg* rtm, void* attrHead, int attrLen, int nlmsgType) {
        struct rtattr* attrs[RTA_MAX + 1];
        parseAttrs(reinterpret_cast<struct rtattr*>(attrHead), attrLen, attrs);
        if (!isDefaultRoute(rtm)) return;
        int oif = -1; bool hasGw = false;
        if (attrs[RTA_OIF]) oif = *reinterpret_cast<int*>(RTA_DATA(attrs[RTA_OIF]));
        if (attrs[RTA_GATEWAY]) hasGw = true;
        if (oif <= 0 || !hasGw) return;
        auto& target = (rtm->rtm_family == AF_INET) ? v4Default : v6Default;
        if (nlmsgType == RTM_NEWROUTE) target.insert(oif); else target.erase(oif);
    }

    /** @brief 根据 nlmsg_type 分发事件消息到 handleLink 或 handleRoute */
    void dispatch(nlmsghdr* hdr) {
        switch (hdr->nlmsg_type) {
            case RTM_NEWLINK:
            case RTM_DELLINK: {
                auto* msg = reinterpret_cast<ifinfomsg*>(NLMSG_DATA(hdr));
                void* attrsHead = IFLA_RTA(msg);
                int attrsLen = IFLA_PAYLOAD(hdr);
                handleLink(msg, attrsHead, attrsLen);
                break;
            }
            case RTM_NEWROUTE:
            case RTM_DELROUTE: {
                auto* msg = reinterpret_cast<rtmsg*>(NLMSG_DATA(hdr));
                void* attrsHead = RTM_RTA(msg);
                int attrsLen = RTM_PAYLOAD(hdr);
                handleRoute(msg, attrsHead, attrsLen, hdr->nlmsg_type);
                break;
            }
            default:
                break;
        }
    }

    /**
     * @brief 同步接收并处理一次 dump 响应（一次性 dump 专用）
     *
     * 与 eventLoop 中的 recvmsg 不同：此处不检查 running 标志，
     * 直到收到 NLMSG_DONE（dump 结束）或 EAGAIN 才返回。
     */
    void recvDump() {
        std::vector<char> buf(64 * 1024);
        while (true) {
            sockaddr_nl nladdr{};
            struct iovec iov{ buf.data(), buf.size() };
            struct msghdr msg{}; msg.msg_name = &nladdr; msg.msg_namelen = sizeof(nladdr); msg.msg_iov = &iov; msg.msg_iovlen = 1;
            ssize_t len = recvmsg(nlSocket, &msg, 0);
            if (len < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                throw std::runtime_error("recvmsg failed");
            }
            if (len == 0) break;
            for (nlmsghdr* hdr = reinterpret_cast<nlmsghdr*>(buf.data()); NLMSG_OK(hdr, static_cast<unsigned>(len)); hdr = NLMSG_NEXT(hdr, len)) {
                if (hdr->nlmsg_type == NLMSG_DONE) return;
                if (hdr->nlmsg_type == NLMSG_ERROR) continue;
                dispatch(hdr);
            }
        }
    }

    /**
     * @brief 根据当前 upIfaces + v4Default + v6Default 计算并发布"当前上网接口"
     *
     * 选择策略：
     * 1. 优先在有 IPv4 默认路由的 ifindex 中找处于 UP 状态的
     * 2. 若无 IPv4 默认路由，在有 IPv6 默认路由的 ifindex 中找处于 UP 状态的
     * 3. 同时命中则 methodFlags 标记两个 flag
     *
     * 发布时用 owner->stateMutex_ 保护 currentIfName_ / methodFlags_，
     * 避免 eventLoop 线程与 getCurrentInterface() 调用方之间的数据竞争。
     */
    void publishState(UsingInterfaceManager* owner, bool printLog) {
        uint32_t methodFlags = 0;
        int chosen = -1;
        // 优先 IPv4 默认路由
        if (!v4Default.empty()) {
            for (int idx : v4Default) { if (upIfaces.count(idx)) { chosen = idx; methodFlags |= UsingMethodFlag::IPv4Default; break; } }
        }
        // 再看 IPv6 默认路由（若 IPv4 未命中，则 IPv6 独立选；若 IPv4 已命中，IPv6 只标记 flag）
        if (!v6Default.empty()) {
            for (int idx : v6Default) { if (upIfaces.count(idx)) { if (chosen == -1) chosen = idx; methodFlags |= UsingMethodFlag::IPv6Default; break; } }
        }
        std::string ifname;
        if (chosen != -1) {
            auto it = ifindexToName.find(chosen);
            if (it != ifindexToName.end()) ifname = it->second; else ifname = std::string("ifindex=") + std::to_string(chosen);
        }
        // 加锁写入对外暴露的状态
        {
            std::lock_guard<std::mutex> lk(owner->stateMutex_);
            owner->currentIfName_ = ifname;
            owner->methodFlags_ = methodFlags;
        }
        if (printLog) {
            if (!ifname.empty()) {
                LOG_INFO(LogModule::INTERFACE, "当前上网网卡: " << ifname
                          << " flags=" << ((methodFlags & UsingMethodFlag::IPv4Default) ? "IPv4" : "")
                          << (((methodFlags & UsingMethodFlag::IPv4Default) && (methodFlags & UsingMethodFlag::IPv6Default)) ? "+" : "")
                          << ((methodFlags & UsingMethodFlag::IPv6Default) ? "IPv6" : ""));
            } else {
                LOG_INFO(LogModule::INTERFACE, "当前上网网卡: (none)");
            }
        }
    }

    /**
     * @brief 后台事件循环线程主体
     *
     * 工作流程：
     * 1. openSocket() + dumpInitial() + publishState() 建立基准状态
     * 2. 设置 SO_RCVTIMEO=1s 使 recvmsg 最多阻塞 1 秒
     * 3. 循环：recvmsg → 处理每条事件（RTM_NEWLINK/DEL/NEWROUTE/DELROUTE）→ publishState
     *    直到 running.load() == false（析构时由主协程置 false）
     * 4. 捕获所有异常，记录日志后退出
     */
    void eventLoop(UsingInterfaceManager* owner) {
        try {
            openSocket();
            dumpInitial();
            // 发布一次初始状态，避免刚启动阶段为空
            publishState(owner, /*printLog=*/true);
            std::vector<char> buf(64 * 1024);
            running.store(true);
            // 设置 socket 超时，避免 recvmsg 永久阻塞导致无法响应 running=false
            struct timeval tv{};
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            setsockopt(nlSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            while (running.load()) {
                sockaddr_nl nladdr{};
                struct iovec iov{ buf.data(), buf.size() };
                struct msghdr msg{}; msg.msg_name = &nladdr; msg.msg_namelen = sizeof(nladdr); msg.msg_iov = &iov; msg.msg_iovlen = 1;
                ssize_t len = recvmsg(nlSocket, &msg, 0);
                if (len < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(1000 * 50); continue; }
                    throw std::runtime_error("recvmsg failed");
                }
                if (len == 0) continue;
                bool seenDone = false;
                for (nlmsghdr* hdr = reinterpret_cast<nlmsghdr*>(buf.data()); NLMSG_OK(hdr, static_cast<unsigned>(len)); hdr = NLMSG_NEXT(hdr, len)) {
                    if (hdr->nlmsg_type == NLMSG_DONE) { seenDone = true; break; }
                    if (hdr->nlmsg_type == NLMSG_ERROR) continue;
                    dispatch(hdr);
                }
                if (seenDone) continue;
                // 每批处理后发布最新状态
                publishState(owner, /*printLog=*/true);
            }
        } catch (const std::exception& ex) {
            LOG_ERROR(LogModule::INTERFACE, "loop error: " << ex.what());
        }
        if (nlSocket >= 0) close(nlSocket);
        nlSocket = -1;
        running.store(false);
    }
#else
    void eventLoop(UsingInterfaceManager*) {}
#endif
};

// pimpl 完整定义处提供析构：结束 netlink 事件循环线程（置停标志 + join），
// 避免 detached 线程在单例析构后野访问；再释放 Impl。
#if defined(__linux__)
UsingInterfaceManager::~UsingInterfaceManager() {
    if (impl_) {
        impl_->running.store(false);       // 通知事件循环退出
        if (impl_->worker.joinable()) impl_->worker.join();  // 等待线程结束，避免 use-after-free
        delete impl_;
        impl_ = nullptr;
    }
}
#else
UsingInterfaceManager::~UsingInterfaceManager() = default;
#endif

/**
 * @brief 启动后台 netlink 事件监听线程
 *
 * 幂等调用：若线程已在运行则不重复启动；若线程已退出但未 join 则先 join。
 * 线程在析构时安全停止（detach 不被调用，始终保持 joinable）。
 */
void UsingInterfaceManager::start() {
#if defined(__linux__)
    std::lock_guard<std::mutex> lk(stateMutex_);
    if (impl_ == nullptr) impl_ = new Impl();
    if (!impl_->running.load()) {
        // 先 join 旧线程（如果线程退出但未 join，worker 仍 joinable）
        if (impl_->worker.joinable()) impl_->worker.join();
        impl_->worker = std::thread([this]{ impl_->eventLoop(this); });
        // 不 detach：保持 joinable，析构时 join 等待线程安全退出
    }
#else
    (void)stateMutex_;
#endif
}

/** @brief 获取当前上网接口名（可能为空串表示无上网接口） */
std::string UsingInterfaceManager::getCurrentInterface() {
    std::lock_guard<std::mutex> lk(stateMutex_);
    return currentIfName_;
}

/** @brief 获取当前上网接口的选择方法标记（UsingMethodFlag::IPv4Default / IPv6Default） */
uint32_t UsingInterfaceManager::getMethodFlags() {
    std::lock_guard<std::mutex> lk(stateMutex_);
    return methodFlags_;
}

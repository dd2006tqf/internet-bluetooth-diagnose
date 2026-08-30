/**
 * @file net_iface.cpp
 * @brief 通过 netlink rtnetlink 协议枚举具备互联网访问能力的网络接口
 *
 * @details 本文件实现 NetInterfaceManager 单例类，提供 getInternetInterfaces() 方法
 *          返回当前所有"受管接口"（up 且有默认网关、可上网的接口）的名称列表。
 *
 *          核心判定逻辑（在匿名命名空间 SnapshotCollector 中）：
 *          1. RTM_GETLINK dump：枚举所有网卡接口，过滤掉 loopback，按 ifindex 记录
 *             处于 UP 状态的接口集合（upInterfaces_）
 *          2. RTM_GETROUTE dump(AF_INET / AF_INET6)：枚举所有路由，识别默认路由
 *             （dst_len==0 且有 RTA_GATEWAY），按协议族分别记录对应的出站 ifindex
 *             （defaultRouteIfacesV4_ / defaultRouteIfacesV6_）
 *          3. 受管接口 = upInterfaces_ ∩ (defaultRouteIfacesV4_ ∪ defaultRouteIfacesV6_)
 *
 *          设计要点：
 *          - 每次调用都是一次性同步快照：open socket → dump link → dump route → 计算 → close
 *          - 带最多 2 次重试，每次重试 resetState 后从头开始
 *          - 使用非阻塞 socket + recvmsg 循环，处理 EINTR / EAGAIN 等信号
 *          - 通过 ifindex→name 映射表在最后输出接口名向量
 *
 * @note 关键系统接口：
 *       - socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE) — 创建 rtnetlink socket
 *       - bind(nlSocket, RTMGRP_LINK | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE)
 *         — 订阅接口和路由变更组播（本文件只做一次性 dump，不持续监听事件）
 *       - sendmsg() 发送 nlmsghdr+rtgenmsg 构造的 RTM_GETLINK/RTM_GETROUTE 请求
 *       - recvmsg() 接收内核返回的多消息 dump（NLMSG_OK / NLMSG_NEXT 遍历）
 *       - 属性格式：RTA_OK / RTA_NEXT 遍历 rtattr，IFLA_RTA / RTM_RTA 定位属性区
 *
 *       非 Linux 平台提供空实现。
 */

#if defined(__linux__)
#include "net_iface.h"
#include "logger.hpp"
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

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace weaknet_dbus;

// 如果内核头缺失 IFF_LOWER_UP/IFF_DORMANT，手动定义（常用但部分旧内核无）
#ifndef IFF_LOWER_UP
#define IFF_LOWER_UP 0x10000
#endif
#ifndef IFF_DORMANT
#define IFF_DORMANT 0x20000
#endif

namespace {

/** @brief netlink 消息接收缓冲区（64KB，足够容纳大型 dump） */
struct NetlinkMessageBuffer {
    std::vector<char> data;
    NetlinkMessageBuffer() : data(64 * 1024) {}
};

/**
 * @brief 将 fd 设置为非阻塞模式（fcntl F_GETFL / F_SETFL + O_NONBLOCK）
 * @param fd 要修改的文件描述符
 * @throws std::runtime_error 若 fcntl 失败
 */
void setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        throw std::runtime_error("fcntl(F_GETFL) failed");
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        throw std::runtime_error("fcntl(F_SETFL) failed");
    }
}

/**
 * @brief 通用 netlink rtattr 属性解析器
 *
 * 遍历属性链表（RTA_OK / RTA_NEXT），将每个属性按 rta_type 填入 attrs 数组对应槽位。
 * 遍历完成后，attrs[type] 即为该类型的 rtattr 指针（未出现的类型保持 nullptr）。
 *
 * @tparam T  rtattr 指针类型
 * @tparam N 属性数组大小（通常为 IFLA_MAX+1 或 RTA_MAX+1）
 * @param rta     属性链表起始位置
 * @param len     整个属性区字节长度（用于防止 RTA_NEXT 越界）
 * @param attrs   [out] 预分配的属性指针数组
 *
 * @note 使用模板 + constexpr array 避免多份代码，属性类型被强制限制在数组范围内。
 */
template <typename T, size_t N>
void parseRtAttributes(struct rtattr* rta, int len, T (&attrs)[N]) {
    std::fill(std::begin(attrs), std::end(attrs), nullptr);
    while (RTA_OK(rta, len)) {
        if (rta->rta_type < N) {
            attrs[rta->rta_type] = rta;
        }
        rta = RTA_NEXT(rta, len);
    }
}

/**
 * @brief 将网卡 flags 位域转为可读字符串（如 "UP|RUNNING|LOWER_UP"）
 * @param flags ifinfomsg.ifi_flags 位掩码
 * @return 以 '|' 连接的标志名序列，空字符串表示无标志
 *
 * @note 包含自定义定义的 IFF_LOWER_UP 和 IFF_DORMANT（若内核头缺失）。
 */
std::string ifFlagsToString(unsigned int flags) {
    std::vector<std::string> names;
    if (flags & IFF_UP) names.emplace_back("UP");
    if (flags & IFF_BROADCAST) names.emplace_back("BROADCAST");
    if (flags & IFF_DEBUG) names.emplace_back("DEBUG");
    if (flags & IFF_LOOPBACK) names.emplace_back("LOOPBACK");
    if (flags & IFF_POINTOPOINT) names.emplace_back("P2P");
    if (flags & IFF_RUNNING) names.emplace_back("RUNNING");
    if (flags & IFF_NOARP) names.emplace_back("NOARP");
    if (flags & IFF_PROMISC) names.emplace_back("PROMISC");
    if (flags & IFF_ALLMULTI) names.emplace_back("ALLMULTI");
    if (flags & IFF_MULTICAST) names.emplace_back("MULTICAST");
    if (flags & IFF_LOWER_UP) names.emplace_back("LOWER_UP");
    if (flags & IFF_DORMANT) names.emplace_back("DORMANT");
    std::string out;
    for (size_t i = 0; i < names.size(); ++i) {
        out += names[i];
        if (i + 1 < names.size()) out += '|';
    }
    return out;
}

// ===========================================================================
// SnapshotCollector：一次性 netlink 快照采集器
// ===========================================================================
// 负责完整执行一次 netlink dump 流程并输出受管接口名列表。
// 采用"状态机"设计：dump link → dump IPv4 route → dump IPv6 route → 计算受管 → 返回结果。

class SnapshotCollector {
public:
    /**
     * @brief 执行一次完整快照采集
     *
     * 工作流程：resetState → openSocket → RTM_GETLINK dump → RTM_GETROUTE(AF_INET) dump →
     * RTM_GETROUTE(AF_INET6) dump → recomputeManagedInterfaces → namesOfManaged
     * 带最多 maxRetries 次重试，任何异常都重试或返回空列表。
     *
     * @return 受管接口名向量（已按字典序排序），空列表表示采集失败或无受管接口
     */
    std::vector<std::string> collect() {
        LOG_INFO(LogModule::INTERFACE, "SnapshotCollector::collect begin");
        const int maxRetries = 2;
        for (int attempt = 0; attempt <= maxRetries; ++attempt) {
            resetState();
            try {
                openSocket();
                sendGetLinkDump();
                receiveDump();
                sendGetRouteDump(AF_INET);
                receiveDump();
                sendGetRouteDump(AF_INET6);
                receiveDump();
                recomputeManagedInterfaces(false);
                auto result = namesOfManaged();
                LOG_INFO(LogModule::INTERFACE,
                         "collect success: up=" << upInterfaces_.size()
                         << " v4_default=" << defaultRouteIfacesV4_.size()
                         << " v6_default=" << defaultRouteIfacesV6_.size()
                         << " managed=" << result.size());
                if (nlSocket_ >= 0) { ::close(nlSocket_); nlSocket_ = -1; }
                return result;
            } catch (const std::exception& e) {
                LOG_ERROR(LogModule::INTERFACE,
                          "collect attempt " << (attempt + 1) << "/" << (maxRetries + 1)
                          << " failed: " << e.what());
                if (nlSocket_ >= 0) { ::close(nlSocket_); nlSocket_ = -1; }
                if (attempt < maxRetries) {
                    continue;
                }
                LOG_ERROR(LogModule::INTERFACE,
                          "collect exhausted retries, returning empty list");
                return {};
            }
        }
        return {};
    }

private:
    int nlSocket_ = -1;                // rtnetlink socket fd

    std::unordered_map<int, std::string> ifindexToName_;  // ifindex → 接口名
    std::unordered_set<int> upInterfaces_;                 // UP 状态的非 loopback 接口
    std::unordered_set<int> defaultRouteIfacesV4_;        // 有 IPv4 默认路由的接口 ifindex
    std::unordered_set<int> defaultRouteIfacesV6_;        // 有 IPv6 默认路由的接口 ifindex
    // 受管接口 = upInterfaces_ ∩ (v4默认网关 ∪ v6默认网关)
    std::unordered_set<int> managedIfaces_;

    /** @brief 重置所有内部状态（每次重试前调用） */
    void resetState() {
        ifindexToName_.clear();
        upInterfaces_.clear();
        defaultRouteIfacesV4_.clear();
        defaultRouteIfacesV6_.clear();
        managedIfaces_.clear();
    }

    /**
     * @brief 创建并初始化 rtnetlink socket
     *
     * 关键步骤：
     * 1. socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE) 创建协议 socket
     * 2. bind 时订阅 RTMGRP_LINK / RTMGRP_IPV4_ROUTE / RTMGRP_IPV6_ROUTE 组播
     *    （本文件只做 dump，但订阅组播不影响 dump 操作）
     * 3. setNonBlocking 避免后续 recvmsg 因 EAGAIN 阻塞主线程
     */
    void openSocket() {
        // 创建 rtnetlink 协议 socket
        nlSocket_ = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
        if (nlSocket_ < 0) {
            LOG_ERROR(LogModule::INTERFACE, "socket(AF_NETLINK) failed: errno=" << errno);
            throw std::runtime_error("socket(AF_NETLINK) 失败");
        }

        sockaddr_nl addr{};
        addr.nl_family = AF_NETLINK;
        // 订阅接口/路由变更组播（对 dump 非必需，但保留以便将来扩展事件监听）
        addr.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE;

        if (bind(nlSocket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            LOG_ERROR(LogModule::INTERFACE, "bind(AF_NETLINK) failed: errno=" << errno);
            throw std::runtime_error("bind(AF_NETLINK) 失败");
        }

        setNonBlocking(nlSocket_);
    }


    /**
     * @brief 构造并发送一条 netlink dump 请求
     *
     * 构造 nlmsghdr + rtgenmsg（路由类请求的通用消息体），通过 sendmsg() 发送。
     *
     * @param nlmsgType RTM_GETLINK / RTM_GETROUTE 等
     * @param flags     额外 nlmsg_flags（通常含 NLM_F_DUMP 要求内核返回完整列表）
     * @param family    AF_PACKET（link）/ AF_INET / AF_INET6（route）/ AF_UNSPEC
     */
    void sendNetlinkRequest(uint16_t nlmsgType, uint16_t flags, uint8_t family) {
        // netlink 请求消息头 + 通用路由消息体（rtgenmsg 只有一个 rtgen_family 字段）
        struct {
            nlmsghdr nlh;
            rtgenmsg gen;
        } req{};

        req.nlh.nlmsg_len = sizeof(req);
        req.nlh.nlmsg_type = nlmsgType;
        // NLM_F_REQUEST 标记为请求，NLM_F_DUMP 要求返回全部对象
        req.nlh.nlmsg_flags = flags | NLM_F_REQUEST;
        req.nlh.nlmsg_seq = static_cast<uint32_t>(::time(nullptr)); // 用时间戳作序列号
        req.nlh.nlmsg_pid = 0;
        req.gen.rtgen_family = family;

        // sendmsg 方式：构造 msghdr + iovec，支持散列/聚集，且可携带 sockaddr_nl 目标地址
        sockaddr_nl nladdr{};
        nladdr.nl_family = AF_NETLINK;

        struct iovec iov{ &req, sizeof(req) };
        struct msghdr msg{};
        msg.msg_name = &nladdr;
        msg.msg_namelen = sizeof(nladdr);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;

        if (sendmsg(nlSocket_, &msg, 0) < 0) {
            LOG_ERROR(LogModule::INTERFACE,
                      "sendmsg failed: nlmsg_type=" << nlmsgType << " family=" << static_cast<int>(family)
                      << " errno=" << errno);
            throw std::runtime_error("sendmsg 失败");
        }
    }

    /** @brief 发送 RTM_GETLINK dump 请求（枚举所有网卡接口） */
    void sendGetLinkDump() { sendNetlinkRequest(RTM_GETLINK, NLM_F_DUMP, AF_PACKET); }
    /** @brief 发送 RTM_GETROUTE dump 请求（枚举指定地址族的所有路由表项） */
    void sendGetRouteDump(int family) { sendNetlinkRequest(RTM_GETROUTE, NLM_F_DUMP, static_cast<uint8_t>(family)); }

    /**
     * @brief 循环接收并处理一批 netlink dump 响应
     *
     * recvmsg 循环处理多条消息直到：
     * - 收到 NLMSG_DONE（dump 结束标记）→ return
     * - 非阻塞 socket 的 EAGAIN/EWOULDBLOCK（无更多数据）→ break
     * - 其他错误 → throw
     * - 收到 NLMSG_ERROR → 记警告后继续（某条消息出错不影响其他）
     *
     * 每条有效消息通过 dispatchNetlinkMessage() 分发到具体处理器。
     */
    void receiveDump() {
        NetlinkMessageBuffer buffer;
        while (true) {
            sockaddr_nl nladdr{};
            struct iovec iov{ buffer.data.data(), buffer.data.size() };
            struct msghdr msg{};
            msg.msg_name = &nladdr;
            msg.msg_namelen = sizeof(nladdr);
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;

            ssize_t len = recvmsg(nlSocket_, &msg, 0);
            if (len < 0) {
                if (errno == EINTR) continue;     // 信号中断，重试
                if (errno == EAGAIN || errno == EWOULDBLOCK) break; // 非阻塞：无更多数据
                LOG_ERROR(LogModule::INTERFACE, "recvmsg failed: errno=" << errno);
                throw std::runtime_error("recvmsg 失败");
            }
            if (len == 0) break;

            // 遍历一个 recvmsg 返回的所有 netlink 消息（多消息拼接是 rtnetlink 常见行为）
            for (nlmsghdr* hdr = reinterpret_cast<nlmsghdr*>(buffer.data.data());
                 NLMSG_OK(hdr, static_cast<unsigned>(len));
                 hdr = NLMSG_NEXT(hdr, len)) {
                if (hdr->nlmsg_type == NLMSG_DONE) return;       // dump 结束
                if (hdr->nlmsg_type == NLMSG_ERROR) {
                    LOG_WARNING(LogModule::INTERFACE, "netlink reported NLMSG_ERROR");
                    continue;
                }
                dispatchNetlinkMessage(hdr);
            }
        }
    }

    /** @brief 根据 nlmsg_type 将消息分发到 handleLink 或 handleRoute */
    void dispatchNetlinkMessage(struct nlmsghdr* hdr) {
        switch (hdr->nlmsg_type) {
            case RTM_NEWLINK:
            case RTM_DELLINK:
                handleLink(
                    reinterpret_cast<ifinfomsg*>(NLMSG_DATA(hdr)),         // 消息体指针
                    IFLA_RTA(reinterpret_cast<ifinfomsg*>(NLMSG_DATA(hdr))), // 属性区起始（跳过 ifinfomsg 头）
                    IFLA_PAYLOAD(hdr),                                      // 属性区长度
                    hdr->nlmsg_type);
                break;
            case RTM_NEWROUTE:
            case RTM_DELROUTE:
                handleRoute(
                    reinterpret_cast<rtmsg*>(NLMSG_DATA(hdr)),
                    RTM_RTA(reinterpret_cast<rtmsg*>(NLMSG_DATA(hdr))),
                    RTM_PAYLOAD(hdr),
                    hdr->nlmsg_type);
                break;
            default:
                break;
        }
    }

    /**
     * @brief 处理一条 RTM_NEWLINK / RTM_DELLINK 消息
     *
     * 解析 ifinfomsg 头部 + IFLA_* 属性，提取接口名、标志位，
     * 维护 ifindexToName_ 映射和 upInterfaces_ 集合。
     * RTM_DELLINK 消息直接从所有集合中移除对应 ifindex。
     */
    void handleLink(ifinfomsg* info, void* attrHead, int attrLen, int nlmsgType) {
        struct rtattr* attrs[IFLA_MAX + 1];
        parseRtAttributes(reinterpret_cast<struct rtattr*>(attrHead), attrLen, attrs);

        int ifindex = info->ifi_index;
        std::string ifname;
        // IFLA_IFNAME 属性：接口名（以 NUL 结尾的字符串）
        if (attrs[IFLA_IFNAME]) {
            char name[IFNAMSIZ]{};
            std::snprintf(name, sizeof(name), "%s", reinterpret_cast<char*>(RTA_DATA(attrs[IFLA_IFNAME])));
            ifname = name;
            ifindexToName_[ifindex] = ifname;
        } else {
            // dump 期间若 DELLINK 先到，可能属性区缺失；从已建立的映射表 best-effort 查找
            auto it = ifindexToName_.find(ifindex);
            if (it != ifindexToName_.end()) ifname = it->second;
        }

        bool isLoopback = (info->ifi_flags & IFF_LOOPBACK) != 0;
        bool isUp = (info->ifi_flags & IFF_UP) != 0;

        // 直接依据消息类型判断链路删除，避免依赖内核 ifi_change/flags 启发式
        if (nlmsgType == RTM_DELLINK) {
            upInterfaces_.erase(ifindex);
            defaultRouteIfacesV4_.erase(ifindex);
            defaultRouteIfacesV6_.erase(ifindex);
            ifindexToName_.erase(ifindex);
            LOG_INFO(LogModule::INTERFACE,
                     "Link removed: ifindex=" << ifindex << " name=" << ifname);
            return;
        }

        // RTM_NEWLINK：非 loopback 且 UP 的接口加入 upInterfaces_
        if (isUp && !isLoopback) {
            upInterfaces_.insert(ifindex);
        } else {
            upInterfaces_.erase(ifindex);
        }

        LOG_INFO(LogModule::INTERFACE,
                 "Link update: ifindex=" << ifindex << " name=" << ifname
                 << " up=" << (isUp ? 1 : 0)
                 << " loopback=" << (isLoopback ? 1 : 0)
                 << " flags=0x" << std::hex << info->ifi_flags << std::dec);
    }

    /**
     * @brief 判断一条路由是否为默认路由
     *
     * 默认路由三要素：
     * - 目的前缀长度为 0（覆盖所有目标）
     * - 路由表是 RT_TABLE_MAIN / RT_TABLE_DEFAULT / RT_TABLE_UNSPEC
     * - scope 为 RT_SCOPE_UNIVERSE / RT_SCOPE_NOWHERE / RT_SCOPE_SITE
     *
     * @param rtm rtmsg 消息体指针
     * @return true  - 是默认路由
     */
    static bool isDefaultRoute(const rtmsg* rtm) {
        return rtm->rtm_dst_len == 0 &&
               (rtm->rtm_table == RT_TABLE_MAIN || rtm->rtm_table == RT_TABLE_DEFAULT || rtm->rtm_table == RT_TABLE_UNSPEC) &&
               (rtm->rtm_scope == RT_SCOPE_UNIVERSE || rtm->rtm_scope == RT_SCOPE_NOWHERE || rtm->rtm_scope == RT_SCOPE_SITE);
    }

    /**
     * @brief 处理一条 RTM_NEWROUTE / RTM_DELROUTE 消息
     *
     * 解析 rtmsg 头部 + RTA_* 属性，提取 RTA_OIF（出站接口索引）和 RTA_GATEWAY（网关地址）。
     * 非默认路由、无出站接口索引、或无网关的路由均被忽略。
     */
    void handleRoute(rtmsg* rtm, void* attrHead, int attrLen, int nlmsgType) {
        struct rtattr* attrs[RTA_MAX + 1];
        parseRtAttributes(reinterpret_cast<struct rtattr*>(attrHead), attrLen, attrs);

        if (!isDefaultRoute(rtm)) {
            return;
        }

        int oif = -1;
        bool hasGateway = false;
        // RTA_OIF：出站接口索引（int 类型）
        if (attrs[RTA_OIF]) {
            oif = *reinterpret_cast<int*>(RTA_DATA(attrs[RTA_OIF]));
        }
        // RTA_GATEWAY：网关地址（存在即表示路由有下一跳，具备上网能力）
        if (attrs[RTA_GATEWAY]) {
            hasGateway = true;
        }

        // 没有明确网关或未绑定接口，视为无上网能力
        if (oif <= 0 || !hasGateway) {
            LOG_WARNING(LogModule::INTERFACE,
                        "Default route ignored: oif=" << oif
                        << " has_gateway=" << (hasGateway ? 1 : 0)
                        << " family=" << (rtm->rtm_family == AF_INET ? "IPv4" : "IPv6"));
            return;
        }

        const char* family = (rtm->rtm_family == AF_INET) ? "IPv4" : "IPv6";
        // 根据协议族选择目标集合
        auto& targetSet = (rtm->rtm_family == AF_INET) ? defaultRouteIfacesV4_ : defaultRouteIfacesV6_;
        std::string ifname;
        if (auto it = ifindexToName_.find(oif); it != ifindexToName_.end()) {
            ifname = it->second;
        }

        if (nlmsgType == RTM_NEWROUTE) {
            targetSet.insert(oif);
            LOG_INFO(LogModule::INTERFACE,
                     "Default route added: " << family << " ifindex=" << oif
                     << " name=" << (ifname.empty() ? "(unknown)" : ifname));
        } else if (nlmsgType == RTM_DELROUTE) {
            targetSet.erase(oif);
            LOG_INFO(LogModule::INTERFACE,
                     "Default route removed: " << family << " ifindex=" << oif
                     << " name=" << (ifname.empty() ? "(unknown)" : ifname));
        }
    }

    /**
     * @brief 重算受管接口集合
     *
     * 受管接口 = upInterfaces_ ∩ (defaultRouteIfacesV4_ ∪ defaultRouteIfacesV6_)
     * 即：UP 状态、非 loopback、且至少有一个协议族的默认路由指向它。
     *
     * @param announceChanges 保留参数（当前实现不使用）
     */
    void recomputeManagedInterfaces(bool announceChanges) {
        (void)announceChanges;
        std::unordered_set<int> newManaged;
        for (int ifindex : upInterfaces_) {
            if (defaultRouteIfacesV4_.count(ifindex) || defaultRouteIfacesV6_.count(ifindex)) {
                newManaged.insert(ifindex);
            }
        }
        managedIfaces_.swap(newManaged);
        LOG_INFO(LogModule::INTERFACE,
                 "recomputeManaged: up=" << upInterfaces_.size()
                 << " v4_default=" << defaultRouteIfacesV4_.size()
                 << " v6_default=" << defaultRouteIfacesV6_.size()
                 << " managed=" << managedIfaces_.size());
    }

    /**
     * @brief 将受管接口的 ifindex 列表转换为接口名向量（按字典序排序）
     * @return 排序后的接口名列表；若某 ifindex 在映射表中找不到名字则跳过
     */
    std::vector<std::string> namesOfManaged() const {
        std::vector<std::string> names;
        names.reserve(managedIfaces_.size());
        for (int idx : managedIfaces_) {
            auto it = ifindexToName_.find(idx);
            if (it != ifindexToName_.end()) {
                names.push_back(it->second);
            } else {
                LOG_WARNING(LogModule::INTERFACE,
                            "managed ifindex=" << idx << " has no name mapping, skipping");
            }
        }
        std::sort(names.begin(), names.end());
        return names;
    }
};

} // namespace

// ===========================================================================
// NetInterfaceManager 单例实现
// ===========================================================================

std::once_flag NetInterfaceManager::s_onceFlag;
std::shared_ptr<NetInterfaceManager> NetInterfaceManager::s_instance;

/** @brief 获取单例实例，线程安全（std::call_once） */
std::shared_ptr<NetInterfaceManager> NetInterfaceManager::getInstance() {
    std::call_once(s_onceFlag, [](){ s_instance = std::shared_ptr<NetInterfaceManager>(new NetInterfaceManager()); });
    return s_instance;
}

/**
 * @brief 获取当前所有具备互联网访问能力的网卡接口名
 *
 * 内部创建 SnapshotCollector 执行一次 netlink dump 快照采集，
 * 任何异常（包括 std::exception 和非 std 异常）都被捕获并返回空列表。
 *
 * @return 接口名向量（按字典序排序），如 {"eth0", "wlan0"}；空列表表示采集失败
 */
std::vector<std::string> NetInterfaceManager::getInternetInterfaces() {
    SnapshotCollector collector;
    try {
        return collector.collect();
    } catch (const std::exception& e) {
        LOG_ERROR(LogModule::INTERFACE, "getInternetInterfaces unexpected failure: " << e.what());
        return {};
    } catch (...) {
        LOG_ERROR(LogModule::INTERFACE, "getInternetInterfaces unknown failure");
        return {};
    }
}

#else

#include "net_iface.h"
#include <string>
#include <vector>

// Thread-safe lazy singleton members
std::once_flag NetInterfaceManager::s_onceFlag;
std::shared_ptr<NetInterfaceManager> NetInterfaceManager::s_instance;

std::shared_ptr<NetInterfaceManager> NetInterfaceManager::getInstance() {
    std::call_once(s_onceFlag, [](){ s_instance = std::shared_ptr<NetInterfaceManager>(new NetInterfaceManager()); });
    return s_instance;
}

std::vector<std::string> NetInterfaceManager::getInternetInterfaces() {
    return {};
}

#endif // defined(__linux__)

/**
 * @file net_tcp.cpp
 * @brief 基于 netlink SOCK_DIAG（inet_diag）接口的 TCP 丢包率监测
 *
 * @details 本文件实现 TcpLossMonitor 单例类，通过 netlink 协议的 NETLINK_SOCK_DIAG
 *          子通道向内核请求所有（或指定接口的）TCP socket 诊断信息，
 *          从 tcp_info 结构中提取重传计数，从而估算 TCP 层面的丢包率。
 *
 *          核心接口：
 *          - sample()           — 采样系统全局 TCP 统计
 *          - sampleForInterface — 采样指定接口的 TCP 统计（过滤 idiag_if）
 *          - compute()          — 基于两次采样的差值计算丢包率百分比 + 等级
 *
 *          丢包率计算逻辑（compute）：
 *          - ratePercent = deltaRetrans / deltaOutSegs × 100%
 *          - 阈值可配置：degradedThresholdPct / poorThresholdPct
 *          - 等级划分：good / degraded / poor / insufficient（数据不足）
 *
 *          近似分母策略（segsOutApprox）：
 *          tcp_info.tcpi_segs_out 字段在部分旧内核/某些内核配置下不可用，
 *          此处使用 tcpi_unacked + tcpi_retrans + tcpi_sacked 三项和作为
 *          近似分母；若仍为 0，则回退到接口 L2 层 tx_packets（包含非 TCP）
 *          作为兜底分母，避免除零。
 *
 * @note 关键系统接口：
 *       - socket(AF_NETLINK, SOCK_DGRAM, NETLINK_SOCK_DIAG)
 *         — 创建 sock_diag 专用 netlink socket（与 NETLINK_ROUTE 不同子通道）
 *       - SOCK_DIAG_BY_FAMILY 消息类型 + inet_diag_req_v2 请求体
 *         — 请求所有 TCP socket 的诊断信息，可按 idiag_if 过滤接口
 *       - INET_DIAG_INFO 属性 → tcp_info 结构
 *         — 内核返回的 TCP 连接详细统计（重传统计在 tcpi_total_retrans）
 *       - RTM_GETLINK + IFLA_STATS64/STATS 属性
 *         — 回退方案：获取接口 L2 层 tx_packets 作为兜底分母
 *       - if_nametoindex() — 接口名转 ifindex（<net/if.h>）
 */

#include "net_tcp.h"
#include "logger.hpp"

#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <algorithm>
#include <vector>
#include <netinet/in.h>
#include <net/if.h>
#include <fstream>
#include <sstream>

#include <linux/netlink.h>
#include <linux/sock_diag.h>
#include <linux/inet_diag.h>
#include <linux/rtnetlink.h>
#include <netinet/tcp.h>

using namespace weaknet_dbus;

// ---------------------------------------------------------------------------
// TcpLossMonitor 单例静态成员
// ---------------------------------------------------------------------------

std::once_flag TcpLossMonitor::s_onceFlag;
std::shared_ptr<TcpLossMonitor> TcpLossMonitor::s_instance;

/** @brief 获取单例实例，线程安全（std::call_once） */
std::shared_ptr<TcpLossMonitor> TcpLossMonitor::getInstance() {
    std::call_once(s_onceFlag, [](){ s_instance = std::shared_ptr<TcpLossMonitor>(new TcpLossMonitor()); });
    return s_instance;
}

/**
 * @brief 向内核请求指定地址族的所有 TCP socket 诊断信息（inet_diag dump）
 *
 * 工作流程：
 * 1. 构造 nlmsghdr + inet_diag_req_v2 请求体：sdiag_protocol=IPPROTO_TCP,
 *    idiag_states=0xFFFFFFFF（所有状态）, idiag_ext 包含 INET_DIAG_INFO（tcp_info）
 * 2. sendmsg 发送 SOCK_DIAG_BY_FAMILY + NLM_F_DUMP 请求
 * 3. recvmsg 循环接收内核返回的 inet_diag_msg 序列直到 NLMSG_DONE
 * 4. 解析 INET_DIAG_INFO 属性 → tcp_info，累加 tcpi_total_retrans 到 totalRetrans
 *
 * @param nlSock       sock_diag netlink socket fd（已创建好，可复用多次调用）
 * @param family       AF_INET 或 AF_INET6
 * @param filterIfindex 接口过滤：>0 时只统计 idiag_if 匹配的 socket；-1 时不做过滤
 * @param[out] segsOutApprox 近似发出段数（分母）
 * @param[out] segsInApprox  近似接收段数
 * @param[out] totalRetrans  累计重传段数（分子）
 *
 * @return true  — 至少成功处理了部分消息或内核正常返回 NLMSG_DONE
 *         false — sendmsg/recvmsg 失败或内核返回 NLMSG_ERROR
 */
static bool diagDumpFamilyIface(int nlSock, int family, int filterIfindex,
                                uint64_t& segsOutApprox, uint64_t& segsInApprox, uint64_t& totalRetrans) {
    segsOutApprox = segsInApprox = totalRetrans = 0;

    // ========== 构造 inet_diag_req_v2 请求 ==========
    struct {
        nlmsghdr nlh;
        inet_diag_req_v2 req;
    } msg{};

    msg.nlh.nlmsg_len = sizeof(msg);
    msg.nlh.nlmsg_type = SOCK_DIAG_BY_FAMILY;     // sock_diag 专用请求类型
    msg.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;  // 请求 + dump 全部
    msg.nlh.nlmsg_seq = 1;
    msg.nlh.nlmsg_pid = 0;

    msg.req.sdiag_family = static_cast<uint8_t>(family);
    msg.req.sdiag_protocol = IPPROTO_TCP;
    msg.req.idiag_states = 0xFFFFFFFFu; // 所有连接状态（LISTEN/SYN_SENT/ESTABLISHED 等）
    // idiag_ext 请求 INET_DIAG_INFO 属性（tcp_info 结构体）
    msg.req.idiag_ext = (1 << (INET_DIAG_INFO - 1));

    sockaddr_nl nladdr{};
    nladdr.nl_family = AF_NETLINK;

    iovec iov{};
    iov.iov_base = &msg;
    iov.iov_len = sizeof(msg);

    msghdr msgh{};
    msgh.msg_name = &nladdr;
    msgh.msg_namelen = sizeof(nladdr);
    msgh.msg_iov = &iov;
    msgh.msg_iovlen = 1;

    if (sendmsg(nlSock, &msgh, 0) < 0) {
        LOG_ERROR_F(LogModule::TCP_LOSS, "diagDumpFamilyIface: sendmsg failed: %s", strerror(errno));
        return false;
    }

    // ========== 循环接收响应 ==========
    std::vector<char> buf(256 * 1024);  // 256KB 足够容纳大型系统的 socket dump
    while (true) {
        iovec riov{ buf.data(), buf.size() };
        msghdr rmsg{};
        rmsg.msg_name = &nladdr;
        rmsg.msg_namelen = sizeof(nladdr);
        rmsg.msg_iov = &riov;
        rmsg.msg_iovlen = 1;

        ssize_t len = recvmsg(nlSock, &rmsg, 0);
        if (len < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR_F(LogModule::TCP_LOSS, "diagDumpFamilyIface: recvmsg failed: %s", strerror(errno));
            return false;
        }
        if (len == 0) break;

        for (nlmsghdr* h = reinterpret_cast<nlmsghdr*>(buf.data()); NLMSG_OK(h, (unsigned)len); h = NLMSG_NEXT(h, len)) {
            if (h->nlmsg_type == NLMSG_DONE) return true;
            if (h->nlmsg_type == NLMSG_ERROR) return false;
            if (h->nlmsg_type != SOCK_DIAG_BY_FAMILY) continue;

            inet_diag_msg* im = reinterpret_cast<inet_diag_msg*>(NLMSG_DATA(h));
            // 若指定了接口过滤，跳过 idiag_if 不匹配的 socket
            if (filterIfindex > 0 && im->id.idiag_if != static_cast<uint32_t>(filterIfindex)) {
                continue;
            }
            // 遍历 inet_diag_msg 之后的属性区，找到 INET_DIAG_INFO 属性
            int rtalen = h->nlmsg_len - NLMSG_LENGTH(sizeof(*im));
            for (rtattr* attr = (rtattr*)(((char*)im) + NLMSG_ALIGN(sizeof(*im)));
                 RTA_OK(attr, rtalen);
                 attr = RTA_NEXT(attr, rtalen)) {
                if (attr->rta_type == INET_DIAG_INFO) {
                    // tcp_info：内核返回的每个连接的 TCP 细粒度统计
                    tcp_info* ti = reinterpret_cast<tcp_info*>(RTA_DATA(attr));
                    totalRetrans += ti->tcpi_total_retrans;
                    // 近似分母：无法使用 tcpi_segs_out 时，采用未确认+已重传+已SACK 估算
                    segsOutApprox += static_cast<uint64_t>(ti->tcpi_unacked)
                                   + static_cast<uint64_t>(ti->tcpi_retrans)
                                   + static_cast<uint64_t>(ti->tcpi_sacked);
                }
            }
        }
    }
    return true;
}

/** @brief 接口名转 ifindex（使用标准 POSIX 接口 if_nametoindex），失败返回 -1 */
static int ifnameToIndex(const std::string& name) {
    unsigned ifi = if_nametoindex(name.c_str());
    return ifi == 0 ? -1 : (int)ifi;
}

/**
 * @brief 回退方案：通过 RTM_GETLINK netlink 查询指定接口的 L2 层 tx_packets
 *
 * 当 diagDumpFamilyIface 返回的 segsOutApprox 为 0（接口上无活跃 TCP 连接）时，
 * 调用此函数获取 L2 tx_packets 作为最小可用分母（包含非 TCP 流量，精度较低但可用）。
 *
 * @param ifindex 接口索引
 * @param[out] txPackets tx_packets 值
 *
 * @return true  - 查询成功
 *         false - netlink 调用失败或未找到接口
 */
static bool rtnlGetIfTxPackets(int ifindex, uint64_t& txPackets) {
    txPackets = 0;
    int nl = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (nl < 0) return false;

    // ========== 构造 RTM_GETLINK 请求 ==========
    struct {
        nlmsghdr nlh;
        ifinfomsg ifm;
    } req{};
    req.nlh.nlmsg_len = sizeof(req);
    req.nlh.nlmsg_type = RTM_GETLINK;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.ifm.ifi_family = AF_UNSPEC;
    req.ifm.ifi_index = ifindex;  // 精确指定接口，内核返回单条响应

    sockaddr_nl nladdr{}; nladdr.nl_family = AF_NETLINK;
    iovec iov{ &req, sizeof(req) };
    msghdr msg{}; msg.msg_name = &nladdr; msg.msg_namelen = sizeof(nladdr); msg.msg_iov = &iov; msg.msg_iovlen = 1;
    if (sendmsg(nl, &msg, 0) < 0) { close(nl); return false; }

    // ========== 接收并解析 RTM_NEWLINK 响应 ==========
    std::vector<char> buf(16 * 1024);
    iov = { buf.data(), buf.size() };
    msghdr rmsg{}; rmsg.msg_name = &nladdr; rmsg.msg_namelen = sizeof(nladdr); rmsg.msg_iov = &iov; rmsg.msg_iovlen = 1;
    ssize_t len = recvmsg(nl, &rmsg, 0);
    close(nl);
    if (len <= 0) return false;

    for (nlmsghdr* h = reinterpret_cast<nlmsghdr*>(buf.data()); NLMSG_OK(h, (unsigned)len); h = NLMSG_NEXT(h, len)) {
        if (h->nlmsg_type == NLMSG_ERROR) return false;
        if (h->nlmsg_type != RTM_NEWLINK) continue;
        ifinfomsg* ifm = reinterpret_cast<ifinfomsg*>(NLMSG_DATA(h));
        if ((int)ifm->ifi_index != ifindex) continue;
        // 遍历 IFLA_* 属性区，优先尝试 IFLA_STATS64（新内核），回退 IFLA_STATS（旧内核）
        int attrlen = h->nlmsg_len - NLMSG_LENGTH(sizeof(*ifm));
        for (rtattr* attr = IFLA_RTA(ifm); RTA_OK(attr, attrlen); attr = RTA_NEXT(attr, attrlen)) {
            if (attr->rta_type == IFLA_STATS64) {
                // 内联定义 rtnl_link_stats64 结构，与内核布局一致
                struct rtnl_link_stats64 { uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes; };
                if (RTA_PAYLOAD(attr) >= sizeof(rtnl_link_stats64)) {
                    auto* st = reinterpret_cast<const rtnl_link_stats64*>(RTA_DATA(attr));
                    txPackets = st->tx_packets;
                    return true;
                }
            } else if (attr->rta_type == IFLA_STATS) {
                // 旧内核：IFLA_STATS 使用 32 位字段
                struct rtnl_link_stats { uint32_t rx_packets, tx_packets; };
                if (RTA_PAYLOAD(attr) >= sizeof(rtnl_link_stats)) {
                    auto* st = reinterpret_cast<const rtnl_link_stats*>(RTA_DATA(attr));
                    txPackets = st->tx_packets;
                    return true;
                }
            }
        }
    }
    return false;
}

/**
 * @brief 采样指定接口的 TCP 统计（IPv4 + IPv6 两地址族）
 *
 * 调用 diagDumpFamilyIface 分别查询 AF_INET 和 AF_INET6，
 * 若接口上无活跃 TCP 连接导致分母为 0，则回退到 rtnlGetIfTxPackets。
 *
 * @param iface 接口名（如 "eth0"、"wlan0"）
 * @param[out] segsOutApprox 近似发出段数
 * @param[out] segsInApprox  近似接收段数
 * @param[out] totalRetrans  累计重传段数
 *
 * @return true  — 至少一个地址族成功采样
 *         false — 接口不存在或两个地址族都失败
 */
static bool diagSampleIfaceAll(const std::string& iface, uint64_t& segsOutApprox, uint64_t& segsInApprox, uint64_t& totalRetrans) {
    segsOutApprox = segsInApprox = totalRetrans = 0;
    int ifidx = ifnameToIndex(iface);
    if (ifidx <= 0) return false;

    // sock_diag socket：用 SOCK_DGRAM 而非 SOCK_RAW（与 NETLINK_ROUTE 不同）
    int nl = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_SOCK_DIAG);
    if (nl < 0) return false;

    uint64_t so4 = 0, si4 = 0, r4 = 0, so6 = 0, si6 = 0, r6 = 0;
    bool ok4 = diagDumpFamilyIface(nl, AF_INET, ifidx, so4, si4, r4);
    bool ok6 = diagDumpFamilyIface(nl, AF_INET6, ifidx, so6, si6, r6);

    close(nl);
    if (!(ok4 || ok6)) return false;
    // 累加两个地址族的统计
    segsOutApprox = so4 + so6;
    segsInApprox = si4 + si6;
    totalRetrans = r4 + r6;
    // 若近似分母为0（接口上无活跃 TCP 连接），尝试用该接口 L2 层 tx_packets 作为最小可用分母（会包含非TCP）
    if (segsOutApprox == 0) {
        uint64_t txp = 0;
        if (rtnlGetIfTxPackets(ifidx, txp)) segsOutApprox = txp;
    }
    return true;
}

// ===========================================================================
// TcpLossMonitor 单例方法实现
// ===========================================================================

/**
 * @brief 采样系统全局所有 TCP socket 的重统计
 *
 * 不做接口过滤（filterIfindex=-1），累加全系统所有 AF_INET + AF_INET6 TCP 连接。
 *
 * @param[out] outStats TcpStats 输出结构
 * @return true  — 至少一个地址族成功
 */
bool TcpLossMonitor::sample(TcpStats& outStats) {
    LOG_INFO(LogModule::TCP_LOSS, "sample: collecting system-wide TCP stats");
    // system-wide: 纯 netlink 近似统计
    int nl = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_SOCK_DIAG);
    if (nl < 0) {
        LOG_ERROR_F(LogModule::TCP_LOSS, "sample: socket creation failed: %s", strerror(errno));
        return false;
    }
    uint64_t so4 = 0, si4 = 0, r4 = 0, so6 = 0, si6 = 0, r6 = 0;
    bool ok4 = diagDumpFamilyIface(nl, AF_INET, -1, so4, si4, r4);
    bool ok6 = diagDumpFamilyIface(nl, AF_INET6, -1, so6, si6, r6);
    close(nl);
    if (!(ok4 || ok6)) return false;
    outStats.retransSegs = r4 + r6;
    outStats.outSegs = so4 + so6;  // 近似分母
    outStats.inSegs = si4 + si6;
    outStats.valid = true;
    return true;
}

/**
 * @brief 采样指定接口的 TCP 统计
 * @param ifaceName 接口名
 * @param[out] outStats TcpStats 输出结构
 * @return true — 采样成功
 */
bool TcpLossMonitor::sampleForInterface(const std::string& ifaceName, TcpStats& outStats) {
    uint64_t so = 0, si = 0, r = 0;
    if (!diagSampleIfaceAll(ifaceName, so, si, r)) return false;
    outStats.outSegs = so;
    outStats.inSegs = si;
    outStats.retransSegs = r;
    outStats.valid = true;
    return true;
}

/**
 * @brief 基于两次采样的差值计算 TCP 丢包率和等级
 *
 * 计算条件：
 * - prev 和 curr 都必须 valid
 * - curr 的计数器不能小于 prev（计数器溢出/重置则无效）
 * - deltaOut ≥ minSent（避免低流量窗口内的高比率误判）
 *
 * 等级划分：
 * - rate ≥ poorThresholdPct         → "poor"
 * - rate ≥ degradedThresholdPct     → "degraded"
 * - 否则                             → "good"
 * 数据不足时返回 level="insufficient"。
 *
 * @param prev                上一次采样结果
 * @param curr                当前采样结果
 * @param minSent             最小采样间隔内的发送段数阈值（低于此值认为样本不足）
 * @param degradedThresholdPct degraded 等级的丢包率阈值（%）
 * @param poorThresholdPct     poor 等级的丢包率阈值（%）
 *
 * @return TcpLossResult：包含 ratePercent（丢包率百分比）、level（等级字符串）、
 *                        sentDelta / retransDelta（本次间隔的增量）
 */
TcpLossResult TcpLossMonitor::compute(const TcpStats& prev,
                                      const TcpStats& curr,
                                      uint64_t minSent,
                                      double degradedThresholdPct,
                                       double poorThresholdPct) {
    TcpLossResult r;
    if (!prev.valid || !curr.valid) {
        LOG_INFO(LogModule::TCP_LOSS, "compute: insufficient data (prev.valid=" << prev.valid << " curr.valid=" << curr.valid << ")");
        r.level = "insufficient"; return r;
    }
    // 计数器回退（溢出/重置）则无效
    if (curr.outSegs < prev.outSegs || curr.retransSegs < prev.retransSegs) {
        r.level = "insufficient";
        return r;
    }
    uint64_t deltaOut = curr.outSegs - prev.outSegs;
    uint64_t deltaRetrans = curr.retransSegs - prev.retransSegs;
    r.sentDelta = deltaOut;
    r.retransDelta = deltaRetrans;
    if (deltaOut < minSent) {
        r.level = "insufficient";
        return r;
    }
    // 计算丢包率百分比（重传增量 / 发出段增量 × 100）
    r.ratePercent = (deltaRetrans * 100.0) / (double)deltaOut;
    if (r.ratePercent >= poorThresholdPct) r.level = "poor";
    else if (r.ratePercent >= degradedThresholdPct) r.level = "degraded";
    else r.level = "good";
    return r;
}

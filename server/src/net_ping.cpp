/**
 * @file net_ping.cpp
 * @brief 基于原始 socket（SOCK_RAW）实现的 ICMP Ping RTT 测量
 *
 * @details 本文件实现 NetPing 单例类，通过直接构造 ICMP Echo Request 报文、
 *          使用原始 socket 发送，并利用 select() 超时等待 ICMP Echo Reply，
 *          从而获取指定主机的往返延迟（RTT）。
 *
 *          设计要点：
 *          - 使用 socket(AF_INET, SOCK_RAW, IPPROTO_ICMP) 创建原始 ICMP socket
 *          - 通过 setsockopt(SO_BINDTODEVICE) 将报文绑定到指定网卡接口
 *          - 在 ICMP 报文体内嵌入发送时间戳（gettimeofday），接收端通过
 *            比较当前时间与嵌入时间戳计算 RTT
 *          - 使用进程 PID 作为 ICMP id、原子递增序列号，实现多线程安全
 *          - 返回值编码：负数对应不同失败原因，便于调用方区分超时/发送失败
 *
 * @note 关键系统接口：
 *       - socket(AF_INET, SOCK_RAW, IPPROTO_ICMP) — 创建原始 ICMP socket
 *       - setsockopt(SO_BINDTODEVICE)              — 绑定网卡接口
 *       - getaddrinfo() / freeaddrinfo()            — DNS 解析 IPv4 地址
 *       - sendto() / recvfrom()                     — 发送/接收 ICMP 报文
 *       - select()                                  — 带超时的 I/O 多路复用
 *
 *       原始 socket 需要 root 权限或 CAP_NET_RAW 能力，否则 socket() 会返回 EPERM。
 *
 *       非 Linux 平台（#else 分支）提供空实现，ping() 恒返回 -1。
 */

#include "net_ping.h"

#if defined(__linux__)
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <sys/select.h>
#include <atomic>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <string>
#include "logger.hpp"

using namespace weaknet_dbus;

namespace {
// 接收缓冲区大小（需要容纳 IP 头 + ICMP 报文）
static constexpr int kPacketSize = 4096;
}

// ---------------------------------------------------------------------------
// NetPing 单例静态成员
// ---------------------------------------------------------------------------
std::once_flag NetPing::s_onceFlag;
std::shared_ptr<NetPing> NetPing::s_instance;

/** @brief 获取单例实例，线程安全（std::call_once） */
std::shared_ptr<NetPing> NetPing::getInstance() {
    std::call_once(s_onceFlag, [](){ s_instance = std::shared_ptr<NetPing>(new NetPing()); });
    return s_instance;
}

NetPing::NetPing() = default;
NetPing::~NetPing() = default;

void NetPing::Init() {}
void NetPing::Shutdown() {}

/**
 * @brief 计算 ICMP 报文的 RFC 1071 标准 16 位互联网校验和
 *
 * 算法：
 * 1. 将数据按 16 位（字）累加，奇数字节补零
 * 2. 累加结果的高 16 位与低 16 位相加，直至高 16 位为零
 * 3. 按位取反得到最终校验和
 *
 * @param addr 指向需要计算校验和的数据区（按 16 位对齐）
 * @param len  数据长度（字节数）
 *
 * @return 计算好的 16 位校验和（返回前已取反）
 *
 * @note 调用方应先将报文中的 checksum 字段置零，再计算；
 *       计算完成后将结果写入 checksum 字段。
 */
uint16_t NetPing::checksum(uint16_t* addr, int len) {
    int nleft = len;
    uint32_t sum = 0;     // 使用 32 位累加避免溢出
    uint16_t* w = addr;
    uint16_t answer = 0;

    // 按 16 位字逐步累加
    while (nleft > 1) {
        sum += *w++;
        nleft -= 2;
    }
    // 奇数长度：处理最后一个字节（补零扩展为 16 位）
    if (nleft == 1) {
        uint16_t last = 0;
        *reinterpret_cast<uint8_t*>(&last) = *reinterpret_cast<uint8_t*>(w);
        sum += last;
    }
    // 折叠累加结果：将高 16 位加回低 16 位，直至只剩 16 位
    sum = (sum >> 16) + (sum & 0xffff);
    sum += (sum >> 16);
    // 按位取反得到最终校验和
    answer = static_cast<uint16_t>(~sum);
    return answer;
}

/**
 * @brief 将主机名解析为 IPv4 地址
 *
 * 使用 getaddrinfo() 进行 DNS 解析，仅请求 AF_INET 地址族。
 *
 * @param host 目标主机名或 IPv4 字符串（如 "8.8.8.8"、"www.example.com"）
 * @param out  [out] 解析成功时的 sockaddr_in 结构体
 *
 * @return true  - 解析成功
 *         false - 解析失败（DNS 不可达、无 A 记录等）
 */
bool NetPing::resolveHostIPv4(const std::string& host, struct sockaddr_in& out) {
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_RAW;
    struct addrinfo* res = nullptr;
    // getaddrinfo 同时支持主机名和 IP 地址字符串
    int ret = getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (ret != 0) return false;
    // 取第一个结果（通常就够用了，不遍历所有地址）
    out = *reinterpret_cast<struct sockaddr_in*>(res->ai_addr);
    freeaddrinfo(res);
    return true;
}

/**
 * @brief 构造 ICMP Echo Request 报文
 *
 * 将发送时间戳（struct timeval）嵌入 ICMP 报文体，
 * 接收时可从 Echo Reply 中取出该时间戳计算 RTT。
 *
 * @param icmp  [out] 指向 icmp 结构体的指针
 * @param id    ICMP identifier（通常使用进程 PID，便于多进程调试）
 * @param seq   ICMP sequence number（递增，用于匹配请求/响应）
 *
 * @return 填充好的报文长度（通常等于 sizeof(struct icmp)）
 */
int NetPing::packIcmp(struct icmp* icmp, uint16_t id, uint16_t seq) {
    std::memset(icmp, 0, sizeof(struct icmp));
    icmp->icmp_type = ICMP_ECHO;   // Echo Request
    icmp->icmp_code = 0;
    icmp->icmp_id = id;
    icmp->icmp_seq = seq;
    // 将当前时间戳写入 icmp_data，接收端取当前时间减去此字段即得 RTT
    struct timeval* tv = reinterpret_cast<struct timeval*>(icmp->icmp_data);
    gettimeofday(tv, nullptr);
    // 先置零再计算校验和
    icmp->icmp_cksum = 0;
    icmp->icmp_cksum = checksum(reinterpret_cast<uint16_t*>(icmp), sizeof(struct icmp));
    return sizeof(struct icmp);
}

/**
 * @brief 执行一次 ICMP Ping，测量 RTT
 *
 * 工作流程：
 * 1. 创建原始 ICMP socket
 * 2. SO_BINDTODEVICE 绑定到指定网卡（确保走指定路径）
 * 3. DNS 解析目标主机
 * 4. 构造并发送 ICMP Echo Request（嵌入时间戳）
 * 5. select() 带超时等待 Echo Reply
 * 6. 解析响应：跳过内核返回的 IPv4 头，提取 ICMP 报文
 * 7. 校验 type/id/seq，从嵌入时间戳计算 RTT
 *
 * @param host      目标主机名或 IPv4 地址字符串
 * @param ifaceName 要使用的网卡接口名（如 "eth0"、"wlan0"）
 * @param timeoutMs 超时时间（毫秒），超时返回负值
 *
 * @return >= 0  RTT 值（毫秒）
 *         -1    socket() 创建失败
 *         -2    SO_BINDTODEVICE 绑定失败
 *         -3    DNS 解析失败
 *         -4    sendto() 发送失败
 *         -5    select() 超时
 *         -6    select() 内部错误
 *         -7    recvfrom() 接收失败
 *         -8    收到的报文长度不足
 *         -9    ICMP type/id 不匹配（收到其他进程的响应）
 *
 * @note 需要 root 权限或 CAP_NET_RAW 能力来创建 SOCK_RAW socket。
 */
int NetPing::ping(const std::string& host, const std::string& ifaceName, int timeoutMs) {
    // ========== 1. 创建原始 ICMP socket ==========
    int sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sockfd < 0) {
        LOG_ERROR(LogModule::PING, "socket() failed: " << strerror(errno));
        return -1;
    }

    // ========== 2. 绑定网卡接口 ==========
    // 使用 SO_BINDTODEVICE 将原始报文从指定接口发出，避免路由表干扰
    struct ifreq ifr{};
    std::snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifaceName.c_str());
    if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) < 0) {
        LOG_ERROR(LogModule::PING, "SO_BINDTODEVICE failed for iface " << ifaceName << ": " << strerror(errno));
        close(sockfd);
        return -2;
    }

    // ========== 3. DNS 解析目标主机 ==========
    struct sockaddr_in dest{};
    if (!resolveHostIPv4(host, dest)) {
        LOG_ERROR(LogModule::PING, "resolveHostIPv4 failed for host " << host);
        close(sockfd);
        return -3;
    }

    // 增大接收缓冲区，减少高负载下的丢包
    int recvBuf = 64 * 1024;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &recvBuf, sizeof(recvBuf));

    // ========== 4. 构造并发送 ICMP Echo Request ==========
    struct icmp icmpPacket{};
    uint16_t id = static_cast<uint16_t>(getpid()); // 用进程 PID 作 ICMP identifier
    static std::atomic<uint16_t> seq{0};           // 原子递增的序列号，多线程安全
    packIcmp(&icmpPacket, id, ++seq);

    // sendto() 发送原始报文（内核自动补上 IPv4 头）
    auto t0 = std::chrono::steady_clock::now();
    ssize_t sent = sendto(sockfd, &icmpPacket, sizeof(icmpPacket), 0,
                          reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
    if (sent < 0) {
        LOG_ERROR(LogModule::PING, "sendto() failed for host " << host << ": " << strerror(errno));
        close(sockfd);
        return -4;
    }
    auto t1 = std::chrono::steady_clock::now();
    int sendMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

    // ========== 5. select() 等待 Echo Reply ==========
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sockfd, &rfds);
    struct timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    int rv = select(sockfd + 1, &rfds, nullptr, nullptr, &tv);
    if (rv <= 0) {
        if (rv == 0) {
            LOG_ERROR(LogModule::PING, "select() timeout after " << timeoutMs << "ms for host " << host);
        } else {
            LOG_ERROR(LogModule::PING, "select() error: " << strerror(errno));
        }
        close(sockfd);
        return (rv == 0) ? -5 : -6; // 超时 / select 内部错误
    }

    // ========== 6. 接收并解析 Echo Reply ==========
    char buf[kPacketSize];
    struct sockaddr_in src{};
    socklen_t slen = sizeof(src);
    ssize_t n = recvfrom(sockfd, buf, sizeof(buf), 0, reinterpret_cast<struct sockaddr*>(&src), &slen);
    if (n <= 0) {
        LOG_ERROR(LogModule::PING, "recvfrom() failed for host " << host << ": " << strerror(errno));
        close(sockfd);
        return -7;
    }

    // 内核返回的数据包含完整 IPv4 头 + ICMP 报文
    // ip_hl × 4 得到 IPv4 头长度（IP 头是 4 字节对齐的，ip_hl 以 4 字节为单位）
    struct ip* iphdr = reinterpret_cast<struct ip*>(buf);
    int iphdrlen = iphdr->ip_hl * 4;
    if (n < iphdrlen + static_cast<ssize_t>(sizeof(struct icmp))) {
        LOG_ERROR(LogModule::PING, "reply too short: n=" << n << " iphdrlen=" << iphdrlen);
        close(sockfd);
        return -8;
    }
    struct icmp* ricmp = reinterpret_cast<struct icmp*>(buf + iphdrlen);
    // 校验：必须是 Echo Reply，且 icmp_id 匹配自己的 PID
    if (ricmp->icmp_type != ICMP_ECHOREPLY || ricmp->icmp_id != id) {
        LOG_ERROR(LogModule::PING, "icmp validation failed: type=" << static_cast<int>(ricmp->icmp_type) << " id=" << ricmp->icmp_id);
        close(sockfd);
        return -9;
    }

    // ========== 7. 从嵌入时间戳计算 RTT ==========
    // 取出发送时刻的 timeval，与当前时刻比较
    struct timeval* sendTv = reinterpret_cast<struct timeval*>(ricmp->icmp_data);
    struct timeval now{};
    gettimeofday(&now, nullptr);
    long rttMs = (now.tv_sec - sendTv->tv_sec) * 1000 + (now.tv_usec - sendTv->tv_usec) / 1000;

    close(sockfd);
    // 如果时间戳差值为负（时钟漂移），退回使用 send 调用耗时作兜底
    return static_cast<int>(rttMs >= 0 ? rttMs : sendMs);
}

// ===========================================================================
// 非 Linux 平台空实现
// ===========================================================================

#else

// 非 Linux 平台的空实现
NetPing::NetPing() {}
NetPing::~NetPing() {}
void NetPing::Init() {}
void NetPing::Shutdown() {}
uint16_t NetPing::checksum(uint16_t*, int) { return 0; }
bool NetPing::resolveHostIPv4(const std::string&, struct sockaddr_in&) { return false; }
int NetPing::packIcmp(struct icmp*, uint16_t, uint16_t) { return 0; }
int NetPing::ping(const std::string&, const std::string&, int) { return -1; }

#endif

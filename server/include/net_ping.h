/**
 * @file net_ping.h
 * @brief ICMP Echo 客户端（单例）
 *
 * 通过原始 socket（AF_INET, SOCK_RAW, IPPROTO_ICMP）发送 ICMP Echo 请求，
 * 测量 RTT（往返时间）。内部实现参考 GNU ping 的核心逻辑。
 *
 * 使用要求：
 *   - 需要 CAP_NET_RAW 或 root 权限（创建 SOCK_RAW socket）
 *   - 权限不足时 ping() 返回负值（哨兵值）
 *
 * 线程安全：getInstance() 使用 std::once_flag 保证线程安全懒汉初始化。
 *           ping() 内部 socket 是每次调用时创建/销毁的，可多线程并发。
 */

#pragma once

#include <string>
#include <cstdint>
#include <memory>
#include <mutex>

/**
 * @brief ICMP Ping 客户端
 *
 * 单例模式，通过 getInstance() 获取。
 * 每次 ping() 内部创建新 socket，调用方无需关心 socket 生命周期。
 */
class NetPing {
public:
    NetPing();
    ~NetPing();

    /// 线程安全懒汉单例
    static std::shared_ptr<NetPing> getInstance();

    /**
     * @brief 执行一次 ICMP Echo，返回 RTT
     *
     * @param host       目标主机（IP 或域名）
     * @param ifaceName  绑定的本地网卡名（可选，空字符串则由内核选择）
     * @param timeoutMs  超时（毫秒），默认 1000ms
     * @return RTT（毫秒，整数）；失败或超时返回负值（通常 -1）
     */
    int ping(const std::string& host, const std::string& ifaceName, int timeoutMs = 1000);

    /// 可选初始化（预留扩展点，目前 ping() 会自动按需创建 socket）
    void Init();

    /// 可选关闭（预留扩展点）
    void Shutdown();

private:
    // ---- ICMP 协议辅助 ----
    /// 计算 ICMP 校验和（RFC 1071）
    static uint16_t checksum(uint16_t* addr, int len);

    /**
     * @brief 解析主机名/域名为 IPv4 地址
     * @param host  主机名或 IP 字符串
     * @param out   输出 sockaddr_in 结构体（sin_addr 字段填充）
     * @return true 解析成功
     */
    static bool resolveHostIPv4(const std::string& host, struct sockaddr_in& out);

    /**
     * @brief 构造 ICMP Echo 请求包
     * @param icmp  输出缓冲区
     * @param id    标识符（pid & 0xFFFF）
     * @param seq   序列号（递增，用于匹配响应）
     * @return 打包后的数据长度
     */
    static int packIcmp(struct icmp* icmp, uint16_t id, uint16_t seq);

    static std::once_flag s_onceFlag;
    static std::shared_ptr<NetPing> s_instance;
};

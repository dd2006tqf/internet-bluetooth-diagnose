/**
 * @file l2_probe_engine.hpp
 * @brief 以太网二层回流探测引擎（AF_PACKET 收发）
 *
 * ============================================================================
 * 这个模块只做一件事
 * ============================================================================
 *
 * 构造自定义 EtherType 的探测帧发出去，把回来的帧收回来，按 seq 配对，
 * 产出一份**收发账本**。它不下结论——结论在 attributeL2Loop()。
 *
 *     L2ProbeEngine  →  L2ProbeRoundRecord（事实）
 *                            ↓
 *                       attributeL2Loop()（推断）
 *
 * ============================================================================
 * 为什么用自定义 EtherType 0x88B5
 * ============================================================================
 *
 * 0x88B5 是 IEEE 802 Local Experimental Ethertype 1，标准留给实验用途，
 * 不会与任何现实协议冲突。选它的决定性理由不是"不冲突"，而是**隔离**：
 *
 * 本项目已经踩过一次主动流量污染被动证据的事故（见
 * docs/网络体验评价体系.md 的 BPF-side Port Tagging 一节：主动探测的
 * DNS 报文被被动 DNS 探针捕获，导致 queue_emitted 虚增）。那次教训的产物
 * 是「Active/Passive 绝不共享捕获路径」这条纪律。
 *
 * 用专用 EtherType 让隔离发生在**内核过滤层**：所有现有 eBPF 程序都不解析
 * 0x88B5，所以探测帧在 BPF 里根本不存在。这是比"记得加 PID 过滤"更硬的保证。
 *
 * ============================================================================
 * 为什么是 SOCK_RAW 而不是 SOCK_DGRAM
 * ============================================================================
 *
 * SOCK_DGRAM 会剥离以太头，我们就拿不到 dst MAC，也就无法排除
 * 「探测帧被自己的 IP 栈收下又吐回来」这个假阳性。必须看到完整以太头。
 *
 * ============================================================================
 * 安全上界（每一条都在代码里硬钳制，不依赖配置正确）
 * ============================================================================
 *
 * - 速率上限 20 pps（配置想超也超不过）
 * - 单轮帧数上限 64
 * - 不发广播、不发组播：dst MAC 必须是单播地址
 * - 默认关闭，且 probe_peers 为空时拒绝启动（不半启用）
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "assurance/l2_probe_plan.hpp"

namespace weaknet_dbus {

using weaknet::L2ProbeTarget;

/**
 * @brief 探测引擎的单轮结果（事实账本，不含任何判断）
 */
struct L2ProbeRoundRecord {
    std::string round_id;       ///< 全局唯一（含实例随机码，跨重启不碰撞）
    uint64_t ts_start_ms = 0;   ///< 墙钟开始时刻
    uint64_t ts_end_ms = 0;     ///< 墙钟结束时刻

    struct FrameResult {
        uint32_t seq = 0;
        std::string egress_ifname;
        int32_t egress_ifindex = 0;
        int32_t vlan_id = -1;
        bool sent = false;
        /// 收到该帧的入口接口（空 = 未回收）。
        /// **注意**：这是内核报告的 sll_ifindex 解析出的接口名，
        /// 不是我们推测的。
        std::string ingress_ifname;
        int32_t ingress_ifindex = -1;
        uint64_t sent_ms = 0;
        uint64_t recv_ms = 0;
        int64_t rtt_us = -1;    ///< 单调时钟差；-1 = 未回收
    };
    std::vector<FrameResult> frames;

    bool covered = false;          ///< 本轮是否完整覆盖所有目标
    std::string degraded_reason;   ///< 非空 → 本轮不可用于归因

    /// 有帧从**非出口**回来 → 存在跨口回流这一事实
    bool has_multi_return = false;
    /// 观察到的回流端口对，形如 "eth0->eth1"
    std::vector<std::string> return_pairs;
};

/**
 * @brief 探测引擎
 *
 * 生命周期：
 *   configure() → start() → [runRound() ...] → stop()
 *
 * 线程模型：runRound() 由**单个**后台线程调用（见 server.cpp 的
 * l2_probe_thread）。本类内部不建线程——这样测试可以直接同步调用，
 * 不需要等线程调度。
 */
class L2ProbeEngine {
public:
    L2ProbeEngine();
    ~L2ProbeEngine();

    L2ProbeEngine(const L2ProbeEngine&) = delete;
    L2ProbeEngine& operator=(const L2ProbeEngine&) = delete;

    /**
     * @brief 配置并校验
     * @return false 表示配置不可用（日志已写明原因），调用方应保持 disabled
     *
     * 拒绝启动的情形（不半启用）：
     *   - egress_ifaces 为空
     *   - egress_ifaces 只配了 1 个接口（单口永远观察不到跨口回流，
     *     每轮必然得出"未回流"这个**假的否定结论**）
     *   - probe_peers 为空或含非单播 MAC
     *   - 任一出口接口不存在或不是以太网卡
     */
    bool configure(const std::string& egress_ifaces,
                   const std::string& probe_peers,
                   const std::string& vlan_ids,
                   uint32_t max_frames_per_round,
                   uint32_t rate_limit_pps,
                   uint32_t timeout_ms);

    /// 开 AF_PACKET socket。返回 false 时 isUsable() 为 false。
    bool start();

    /// 关 socket（幂等）
    void stop();

    /**
     * @brief 在线更新"不改目标集合"的三个参数（帧数上限 / 速率 / 超时）
     *
     * 为什么只有这三个能在线改：它们只影响"每轮发多少、发多快、等多久"，
     * 不改变哪些 ifindex 参与、帧长是否带 802.1Q tag。
     * 出口网卡/对端/VLAN 变更必须重建引擎（见 applyL2ProbeConfig），
     * 否则会跑出"一半新目标一半旧目标"的轮次，那一轮的回流证据无法解释。
     *
     * 线程安全：参数在 runRound() 开头于 mutex 内整体快照，本方法同样持锁写入，
     * 因此不会出现"半新半旧"的中间态被一轮读到。
     *
     * @return false 表示引擎未配置或未启动（无目标），调用方应改走重建路径
     */
    bool updateLimits(uint32_t max_frames_per_round, uint32_t rate_limit_pps,
                      uint32_t timeout_ms);

    /// @return 已配置且 socket 可用
    bool isUsable() const { return socket_fd_ >= 0 && !targets_.empty(); }

    /**
     * @brief 跑一轮探测（同步阻塞，最长 timeout_ms）
     *
     * @param now_ms 墙钟时刻（由调用方提供，便于测试注入）
     * @return 本轮账本。即使内部失败也返回记录（degraded_reason 非空），
     *         不返回空——「这轮失败了」本身是一条要落库的事实。
     */
    L2ProbeRoundRecord runRound(uint64_t now_ms);

    /// 最近一次 configure 的拒绝原因（空 = 配置有效）
    const std::string& lastError() const { return last_error_; }

    /// 已配置的目标（供归因侧解释"为什么没结论"）
    std::vector<std::string> egressIfaces() const;

    /// 硬上限（配置无法突破）
    static constexpr uint32_t kMaxRatePps = 20;
    static constexpr uint32_t kMaxFramesPerRound = 64;

private:
    bool parseIfaces(const std::string& spec);
    bool parsePeers(const std::string& spec);
    bool applyVlanSpec(const std::string& spec);

    /// 解析接口名 → ifindex（0 = 不存在）
    static int32_t resolveIfindex(const std::string& name);
    /// 解析 ifindex → 接口名（空 = 未知）
    static std::string resolveIfname(int32_t ifindex);
    /// 判断该接口是否为以太网类型（ARPHRD_ETHER）
    static bool isEthernetDevice(const std::string& name);

    int socket_fd_ = -1;
    std::vector<L2ProbeTarget> targets_;
    std::vector<std::string> peers_;      ///< 已解析成 "XX:XX:..." 大写形式
    uint32_t max_frames_per_round_ = 8;
    uint32_t rate_limit_pps_ = 4;
    uint32_t timeout_ms_ = 2000;
    std::string last_error_;
    mutable std::mutex mutex_;            ///< 保护 targets_/peers_ 的读
};

}  // namespace weaknet_dbus

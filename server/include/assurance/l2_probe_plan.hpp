/**
 * @file l2_probe_plan.hpp
 * @brief 二层回流探测的调度计划器（纯逻辑，无 socket、无系统调用）
 *
 * ============================================================================
 * 这个文件为什么存在
 * ============================================================================
 *
 * 探测本身（AF_PACKET 收发）必须有网络和权限才能测，但**调度决策**不需要：
 * 「这一轮该发几帧」「哪些帧因限速没发出去」「本轮覆盖是否完整」全是纯算术。
 * 把它们从 socket 里拆出来，就能在没有 CAP_NET_RAW 的 x86 CI 上锁死
 * 整个方案里最容易错的部分。
 *
 * 这不是为了"测试覆盖率好看"。真机上最危险的一类缺陷是「限速悄悄吞掉了
 * 一半帧，但结论照常给出」——它不会报错、不会崩，只会让运维看到一份
 * 基于残缺样本的自信结论。纯逻辑层可以精确断言这件事。
 *
 * ============================================================================
 * 与 LoopGuard 原始调度器的差异（刻意简化）
 * ============================================================================
 *
 * LoopGuard 面向「数千受管端口 × 大量 VLAN」，因此有最早到期优先、每端口
 * 独立上下文、多级转控制。WeakNet 的规模是「几个口」，直接移植是过度设计。
 *
 * 保留的只有三条真正承载安全性的约定：
 *   1. 令牌桶限速  —— 防止配置错误把大量帧打进客户网络
 *   2. 有界批次    —— 保证单轮耗时有上界
 *   3. 覆盖状态    —— 少发了帧就必须记 DEGRADED，不许给结论
 *
 * 明确不保留：最早到期优先、每端口上下文、多级转控制。
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace weaknet {

/**
 * @brief 一次探测的发送目标（一个 egress 口 × 一个 VLAN 语境）
 *
 * vlan_id 用 -1 表示「不带 tag」，**不是 0**。
 * VLAN 0 是合法的 priority-tagged 语义，用 0 冒充"无 tag"会直接违反项目
 * 「未采集 != 零值」的硬约束（见 wireless_event.hpp 设计原则 4）。
 */
struct L2ProbeTarget {
    std::string ifname;        ///< 出口接口名（如 "eth0"）
    int32_t     ifindex = 0;   ///< 出口接口索引（由引擎填充；计划器不解析）
    int32_t     vlan_id = -1;  ///< -1 = 不带 802.1Q tag；0..4094 = 该 VLAN
};

/**
 * @brief 计划好的一帧
 */
struct L2ProbeFramePlan {
    L2ProbeTarget target;
    uint32_t      seq = 0;        ///< 轮内递增序号（回流匹配的唯一键）
    uint64_t      due_at_ms = 0;  ///< 相对轮开始的发送时刻（受令牌桶节流）
};

/**
 * @brief 单轮计划的结果
 */
struct L2ProbeRoundPlan {
    std::vector<L2ProbeFramePlan> frames;

    /// 因令牌桶或批次上限而**未能**排入的帧数。
    /// 非零 → 本轮覆盖不完整 → 引擎必须记 DEGRADED，不得给出结论。
    uint32_t dropped_by_limit = 0;

    /// 计划是否完整覆盖了所有目标。
    /// false 时引擎**仍可发送已计划的帧**（部分证据优于零证据），
    /// 但归因时该轮必须计入 degraded_rounds。
    bool covered = false;

    /// 覆盖不完整的具体原因（空串 = 完整）。
    /// 取值与 L2ProbeDegradedReason 的字符串形式一致，便于直接落库。
    std::string degraded_reason;
};

/**
 * @brief 令牌桶
 *
 * 单向操作、无隐藏状态：每次 refill 后 tryConsume 若干次。
 * 容量 = rate_pps，即最坏情况下单轮可突发一整秒的额度。
 *
 * 为什么容量等于速率而不是更大：容量大于一秒钟的额度，意味着配置 4pps
 * 时仍可能瞬间打出 8 帧。工业现场对突发最敏感，宁可让探测慢一点。
 */
class L2ProbeTokenBucket {
public:
    /// @param rate_pps 每秒令牌数（必须 > 0）
    explicit L2ProbeTokenBucket(uint32_t rate_pps);

    /**
     * @brief 按经过时间补充令牌
     * @param elapsed_ms 距上次 refill 的毫秒数
     *
     * 不足 1 个令牌的部分**保留**（用毫秒累计），否则 4pps 在 100ms 粒度的
     * 调度下永远补不满一个令牌——这是令牌桶最常见的实现缺陷。
     */
    void refill(uint64_t elapsed_ms);

    /// @return 成功取得令牌返回 true 并扣除；否则 false
    bool tryConsume();

    /// 当前可用令牌数（向下取整），仅用于测试与日志
    uint32_t available() const { return tokens_; }

private:
    uint32_t rate_pps_ = 1;
    uint32_t tokens_ = 0;
    uint64_t carry_ms_ = 0;  ///< 未满一个令牌的毫秒累计
};

/**
 * @brief 生成一轮探测计划（纯函数）
 *
 * @param targets              本轮要探测的所有 (口, VLAN) 组合
 * @param max_frames_per_round 单轮帧数硬上限（调用方负责钳制到允许范围）
 * @param bucket               令牌桶（会被消费，调用方每轮 refill 一次）
 * @param now_ms               本轮起始时刻（用于计算 due_at_ms）
 *
 * 行为：
 *   - 按 targets 顺序轮转分配 seq（0,1,2,...），保证每个 target 至少尝试一帧；
 *   - 每帧必须取得令牌才能排入，取不到即计入 dropped_by_limit；
 *   - 达到 max_frames_per_round 后停止，剩余帧同样计入 dropped_by_limit；
 *   - dropped_by_limit > 0 → covered=false，degraded_reason="send_throttled"。
 *
 * 为什么每个 target 至少一帧：探测的意义是"每个口都试过"。若某个口因限速
 * 一帧都没发出去，本轮对它一无所知，却会因为它"没产生回流"而被算作无回流，
 * 那是把「没测」当成「测了没问题」。
 */
L2ProbeRoundPlan planL2ProbeRound(const std::vector<L2ProbeTarget>& targets,
                                  uint32_t max_frames_per_round,
                                  L2ProbeTokenBucket* bucket,
                                  uint64_t now_ms);

/**
 * @brief 判断这组出口是否具备「跨口回流」的判定资格（纯函数）
 *
 * 返回空串 = 有资格；非空 = 拒绝原因（可直接作为配置错误日志）。
 *
 * **为什么必须至少两个不同的接口**：`has_multi_return` 的定义是
 * 「帧从**非出口**接口回来」。只有 1 个出口时，任何回流帧的 ingress
 * 必然等于 egress，于是每轮 `MULTI_RETURN` 恒为 0，`attributeL2Loop()`
 * 会给出 **NO_LOOP**——一个"确证没有环路"的结论，而证据根本不存在。
 *
 * 这是本仓库最警惕的那类缺陷：不报错、不崩溃、日志里轮次和收发数字都正常，
 * 只有结论永久错。宁可拒绝启动并写明原因。
 *
 * 允许同一接口配多个 VLAN：那仍然是**一个**出口，跨口回流依旧不可观察。
 * 故判据看的是**去重后的接口数**，不是 target 数。
 */
std::string checkL2ProbeEgressEligibility(const std::vector<L2ProbeTarget>& targets);

}  // namespace weaknet

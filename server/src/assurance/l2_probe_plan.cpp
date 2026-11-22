/**
 * @file l2_probe_plan.cpp
 * @brief L2 探测调度计划器实现（纯逻辑，无 I/O）
 */

#include "assurance/l2_probe_plan.hpp"

#include <algorithm>
#include <set>

namespace weaknet {

L2ProbeTokenBucket::L2ProbeTokenBucket(uint32_t rate_pps)
    : rate_pps_(rate_pps == 0 ? 1 : rate_pps) {
    // 初始给满一桶：第一轮不该因为"还没攒够令牌"而一帧都发不出去，
    // 那会让首次探测在没有前序数据时直接判 DEGRADED，看起来像配置错误。
    tokens_ = rate_pps_;
}

void L2ProbeTokenBucket::refill(uint64_t elapsed_ms) {
    if (elapsed_ms == 0) return;
    // carry_ms_ 保存"不足一个令牌的零头"，避免按整毫秒丢弃导致长期补不满。
    // 例如 4pps → 250ms/令牌；若调度粒度是 100ms，不累计零头的话
    // 每轮都只攒到 100ms 然后丢弃，永远攒不满 250ms。
    carry_ms_ += elapsed_ms;
    const uint64_t ms_per_token = 1000ULL / rate_pps_;
    if (ms_per_token == 0) {
        // rate_pps_ > 1000 时理论上会出现；本项目硬上限远低于此，
        // 但仍按"每秒一令牌"退化处理，绝不除零。
        tokens_ = rate_pps_;
        carry_ms_ = 0;
        return;
    }
    const uint64_t gained = carry_ms_ / ms_per_token;
    if (gained > 0) {
        const uint64_t capped = std::min<uint64_t>(tokens_ + gained, rate_pps_);
        tokens_ = static_cast<uint32_t>(capped);
        carry_ms_ -= gained * ms_per_token;
        // 桶满时零头也没有意义了，清掉避免"存了半桶额度却用不出去"的假象
        if (tokens_ >= rate_pps_) carry_ms_ = 0;
    }
}

bool L2ProbeTokenBucket::tryConsume() {
    if (tokens_ == 0) return false;
    --tokens_;
    return true;
}

L2ProbeRoundPlan planL2ProbeRound(const std::vector<L2ProbeTarget>& targets,
                                  uint32_t max_frames_per_round,
                                  L2ProbeTokenBucket* bucket,
                                  uint64_t now_ms) {
    L2ProbeRoundPlan plan;

    if (targets.empty()) {
        plan.covered = true;  // 没有目标 = 无事可做，不是覆盖不全
        return plan;
    }
    if (max_frames_per_round == 0 || bucket == nullptr) {
        plan.dropped_by_limit = static_cast<uint32_t>(targets.size());
        plan.degraded_reason = "send_throttled";
        return plan;
    }

    // 轮转分配：先保证每个 target 拿到第 1 帧，再回头发第 2 轮。
    // 这样即使额度只够一部分，也是"每个口都测了一下"，而不是"第一个口测透了，
    // 其余口一无所知"。后者会让未探测的口被误算进"无回流"。
    size_t round_index = 0;
    bool exhausted = false;
    while (!exhausted && plan.frames.size() < max_frames_per_round) {
        bool progressed = false;
        for (const auto& t : targets) {
            if (plan.frames.size() >= max_frames_per_round) {
                exhausted = true;
                break;
            }
            if (!bucket->tryConsume()) {
                exhausted = true;
                break;
            }
            L2ProbeFramePlan f;
            f.target = t;
            f.seq = static_cast<uint32_t>(plan.frames.size());
            // due_at_ms 按已排帧数均匀铺开，避免同一毫秒打出整批。
            // 1000/rate_pps 是令牌间隔的近似；这里只用于发送节奏，
            // 精确的速率控制由令牌桶负责（二者冗余但方向一致）。
            f.due_at_ms = now_ms + static_cast<uint64_t>(plan.frames.size()) * 10ULL;
            plan.frames.push_back(f);
            progressed = true;
        }
        if (!progressed) break;
        ++round_index;
        // 防止 target 极少但额度极大时无限循环（理论上 max_frames 已封顶，
        // 这里只是双保险）
        if (round_index > max_frames_per_round) break;
    }

    // 覆盖判据：**每个 target 是否至少拿到一帧**。
    //
    // 为什么不是"是否用满了批次"或"是否发满了 N 轮"：
    //   - 用满批次不代表覆盖完整（4 个目标、批次上限 2 → 只有前两个口测到）；
    //   - "发满 N 轮"没有定义（发几轮取决于剩余额度，不是配置）。
    // 真正的语义要求是：**没有哪个口是"一帧没发却按无回流计"的**。
    // 一个 target 若一帧都没发出去，本轮对它一无所知，把它算进
    // NO_LOOP 就是把「没测」当成「测了没问题」。
    //
    // 注意：这里不把"没发满预期轮数"算作降级。轮数不是契约，
    // 只要求每个口都被探到过。额度不足导致只跑了一轮，仍是有效样本。
    std::vector<bool> seen(targets.size(), false);
    for (const auto& f : plan.frames) {
        for (size_t i = 0; i < targets.size(); ++i) {
            if (f.target.ifname == targets[i].ifname &&
                f.target.vlan_id == targets[i].vlan_id) {
                seen[i] = true;
                break;
            }
        }
    }
    uint32_t unseen = 0;
    for (bool s : seen) {
        if (!s) ++unseen;
    }
    plan.dropped_by_limit = unseen;
    plan.covered = (unseen == 0);
    plan.degraded_reason = plan.covered ? "" : "send_throttled";
    return plan;
}

std::string checkL2ProbeEgressEligibility(const std::vector<L2ProbeTarget>& targets) {
    // 用 set 而不是 "targets.size() >= 2"：同一接口配多个 VLAN 仍是**一个**
    // 出口，跨口回流依旧不可观察。判据是去重后的接口数。
    std::set<std::string> ifaces;
    for (const auto& t : targets) {
        if (!t.ifname.empty()) ifaces.insert(t.ifname);
    }

    if (ifaces.empty()) {
        return "l2_probe.egress_ifaces 为空：不探测任何接口";
    }
    if (ifaces.size() < 2) {
        return "l2_probe.egress_ifaces 只包含 " + std::to_string(ifaces.size()) +
               " 个接口（" + *ifaces.begin() +
               "）：跨口回流至少需要两个不同出口才能观察。"
               "只配一个口时每轮必然得出「未回流」，"
               "那是把「不知道」包装成「一切正常」";
    }
    return "";
}

}  // namespace weaknet

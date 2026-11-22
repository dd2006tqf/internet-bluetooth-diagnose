/**
 * @file l2_loop_attribution.cpp
 * @brief 二层回流归因实现（纯函数）
 */

#include "assurance/l2_loop_attribution.hpp"

namespace weaknet {

L2LoopResult attributeL2Loop(const L2ProbeWindow& window) {
    L2LoopResult r;

    // ---- 边界 1：根本没数据 ----
    if (window.total_rounds == 0) {
        r.attribution = L2LoopAttribution::NONE;
        r.reason = "no_probe_data";
        // 区分"没配置出口"与"配置了但没跑起来"：两者对运维的意义完全不同。
        if (window.egress_ifaces.empty()) {
            r.explanation = "未配置二层探测出口接口";
        } else {
            r.explanation = "窗口内没有完成的探测轮次";
        }
        return r;
    }

    const int valid_rounds = window.total_rounds - window.degraded_rounds;

    // ---- 边界 2：全部轮次降级 ----
    // 有轮次但一轮都不可信，等价于没数据。绝不能因为"跑过很多轮"就给出结论。
    if (valid_rounds <= 0) {
        r.attribution = L2LoopAttribution::NONE;
        r.reason = "all_rounds_degraded";
        r.explanation = "全部 " + std::to_string(window.total_rounds) +
                        " 轮探测均降级，无可信样本";
        return r;
    }

    // ---- 边界 3：有效样本不足 ----
    // 单轮无法区分"稳定存在"与"STP 收敛期瞬时现象"。
    if (valid_rounds < 2) {
        r.attribution = L2LoopAttribution::NONE;
        r.reason = "insufficient_valid_rounds";
        r.explanation = "仅 " + std::to_string(valid_rounds) +
                        " 轮有效探测，不足以判断二层通路";
        return r;
    }

    // ---- 主判定 ----
    if (window.multi_return_rounds <= 0) {
        r.attribution = L2LoopAttribution::NO_LOOP;
        r.explanation = std::to_string(valid_rounds) + " 轮有效探测均未见跨口回流";
        return r;
    }

    // 回流轮次占比 >= 1/2 才给结论。
    // 用整数比较避免浮点："回流轮次 * 2 >= 有效轮次"。
    if (window.multi_return_rounds * 2 >= valid_rounds) {
        r.attribution = L2LoopAttribution::LOOP_SUSPECTED;
        r.issues.push_back("MULTI_PORT_RETURN");
        r.explanation = std::to_string(window.multi_return_rounds) + "/" +
                        std::to_string(valid_rounds) + " 轮观察到跨口回流";
        if (!window.return_pairs.empty()) {
            r.explanation += "（";
            for (size_t i = 0; i < window.return_pairs.size(); ++i) {
                if (i) r.explanation += ", ";
                r.explanation += window.return_pairs[i];
            }
            r.explanation += "）";
        }
        return r;
    }

    // ---- 边界 4：有回流但不够一致 ----
    // 少数轮次看到回流，可能是拓扑抖动或 STP 收敛。既不能说"有环路"，
    // 也不能说"没环路"——因为确实观测到了。
    r.attribution = L2LoopAttribution::NONE;
    r.reason = "insufficient_valid_rounds";
    r.explanation = "仅 " + std::to_string(window.multi_return_rounds) + "/" +
                    std::to_string(valid_rounds) + " 轮出现回流，不足以判定二层通路";
    return r;
}

}  // namespace weaknet

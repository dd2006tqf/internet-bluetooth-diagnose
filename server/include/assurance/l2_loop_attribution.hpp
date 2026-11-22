/**
 * @file l2_loop_attribution.hpp
 * @brief 二层回流归因：把「探测帧从哪个口回来」变成一句可解释的结论
 *
 * ============================================================================
 * 与 thermal / observer / video_qoe 三个归因同构
 * ============================================================================
 *
 * 四个归因的形状完全一样：**拿一路确定性证据，产出一个不参与评分的旁证结论**。
 *
 *   ObserverAttribution：eBPF 观测质量  ← 校验 → CPU 压力
 *   ThermalAttribution ：采集端能力    ← 校验 → 温度/风扇
 *   VideoQoeAttribution：帧级体验      ← 校验 → 网络指标
 *   L2LoopAttribution  ：二层拓扑事实  ← 校验 → 探测帧回流
 *
 * 四者共同纪律：**不修改**任何 OverallPolicy / SLE 的 state/coverage/reason，
 * 只在一旁追加一个独立归因结论。上层可以同时看到两句话。
 *
 * ============================================================================
 * 语义边界（逐条有单测守护）
 * ============================================================================
 *
 * - **「没有结论」不得伪装成「没有回流」。** DEGRADED / 无数据 → NONE，
 *   绝不返回 NO_LOOP。这与 VideoQoeAttribution::NONE 是同一件事：
 *   一次把"不知道"说成"一切正常"，会毁掉这个能力长期的可信度。
 * - **单轮回流不构成结论。** STP 收敛期会短暂出现跨口回传，那是正常行为。
 *   只有多数有效轮次都看到回流，才足以说"存在二层通路"。
 * - **措辞是 LOOP_SUSPECTED 而不是 LOOP_CONFIRMED。** 从"帧从另一个口回来"
 *   到"这是环路"仍是推断。命名成 CONFIRMED 会把推断伪装成事实，
 *   违反项目「事实与推断必须分开」的总原则。
 * - **本结论不是根因。** 它不得写入 site_incidents.suspected_cause——
 *   那是 Phase 4b 诊断器的权力。它可以作为诊断器的输入证据，但接入是独立议题。
 *
 * ============================================================================
 * 为什么是纯函数而不是塞进探测引擎
 * ============================================================================
 *
 * 若把判定塞进引擎，引擎就会同时承担"发帧"和"下结论"两个职责，
 * 而后者需要独立可测。更实际的理由：纯函数可以在没有 CAP_NET_RAW、
 * 没有第二块网卡的 x86 CI 上穷举所有退化组合——那恰恰是最容易出错的部分。
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace weaknet {

/**
 * @brief 二层回流归因结论
 *
 * 值域刻意保持窄：上层只需要知道「要不要怀疑这个现场存在非预期二层通路」。
 */
enum class L2LoopAttribution {
    /// 无法归因（无探测数据 / 全部轮次降级 / 有效轮次不足）
    ///
    /// 注意它**不是**一个"好"的结论，而是"没有结论"。
    /// 上游必须把 NONE 呈现为 unavailable，而不是 no loop。
    NONE,

    /// 有效测量，未见跨口回流（唯一一个可以这么说出口的否定结论）
    NO_LOOP,

    /// 有效测量，多轮观察到跨口回流 → 该现场可能存在非预期二层通路
    ///
    /// 措辞是 SUSPECTED 不是 CONFIRMED：见文件头语义边界第三条。
    LOOP_SUSPECTED
};

inline const char* l2LoopAttributionToString(L2LoopAttribution a) {
    switch (a) {
        case L2LoopAttribution::NONE: return "NONE";
        case L2LoopAttribution::NO_LOOP: return "NO_LOOP";
        case L2LoopAttribution::LOOP_SUSPECTED: return "LOOP_SUSPECTED";
        default: return "NONE";
    }
}

/**
 * @brief 归因窗口内聚合后的探测事实（由引擎装配，不含任何 socket 类型）
 *
 * 它是「已经发生过的事实」的快照，不是实时状态。这样归因函数
 * 与引擎的生命周期完全解耦。
 */
struct L2ProbeWindow {
    uint64_t window_start_ms = 0;
    uint64_t window_end_ms = 0;

    /// 窗口内已完成的探测轮次总数（含降级轮）
    int total_rounds = 0;

    /// 其中被判定为 DEGRADED 的轮次数
    int degraded_rounds = 0;

    /// 其中观察到跨口回流的轮次数（仅统计非降级轮）
    int multi_return_rounds = 0;

    /// 观察到的回流端口对，形如 "eth0->eth1"（去重后）
    std::vector<std::string> return_pairs;

    /// 参与探测的出口接口名（用于解释为什么没结论，如空 = 没配置）
    std::vector<std::string> egress_ifaces;
};

/**
 * @brief 归因结果
 */
struct L2LoopResult {
    L2LoopAttribution attribution = L2LoopAttribution::NONE;

    /// 人类可读解释。NONE 时**必须**写明拒绝归因的具体原因，
    /// 不能是空字符串——运维要据此区分「没配置」（正常）与
    /// 「配置了但测不准」（要修）。
    std::string explanation;

    /// 机器可读的拒绝原因（空串 = 已归因）
    /// 取值：no_probe_data / disabled / no_egress_iface /
    ///       all_rounds_degraded / insufficient_valid_rounds
    std::string reason;

    /// 机器可读问题清单（仅 LOOP_SUSPECTED 时非空）
    std::vector<std::string> issues;
};

/**
 * @brief 从探测窗口事实归因（纯函数，确定性，无副作用）
 *
 * 判定规则（不使用任何需要校准的魔法阈值）：
 *
 *   total_rounds == 0                              → NONE  (no_probe_data)
 *   非降级轮次 == 0                                 → NONE  (all_rounds_degraded)
 *   非降级轮次 < 2                                  → NONE  (insufficient_valid_rounds)
 *   回流轮次 == 0                                   → NO_LOOP
 *   回流轮次 / 非降级轮次 >= 1/2                    → LOOP_SUSPECTED
 *   否则                                            → NONE  (insufficient_valid_rounds)
 *
 * 为什么非降级轮次至少要 2：单轮是样本量 1，无法区分"稳定存在"
 * 与"STP 收敛期的瞬时现象"。这一条与 QualifyingAnomalyPolicy 要求
 * 「至少 2 台设备同时异常」是同一种谨慎——用一个现实中可见的规则，
 * 而不是引入需要长期数据才能确定的百分比阈值。
 */
L2LoopResult attributeL2Loop(const L2ProbeWindow& window);

}  // namespace weaknet

/**
 * @file observer_attribution.hpp
 * @brief 观测者降级归因：区分「网络真的坏了」与「采集端自己过载」
 *
 * ============================================================================
 * 要解决的漏洞
 * ============================================================================
 *
 * 现有 Observer 质量判定（dns_service_evaluator.hpp 的 checkObserverQuality）
 * 只看 eBPF 的传输层失败率：capture_emit_failure_ratio / perf_delivery_loss_ratio /
 * ambiguity_ratio / insert_failure_ratio。任一项超阈值就判 observer unreliable。
 *
 * 但 observer unreliable 这件事本身有**两个完全不同的成因**：
 *
 *   (a) 环境/网络侧：内核拥塞、perf buffer 来不及消费、跟踪表被打满
 *       → 此时网络很可能真的有问题，UNKNOWN 是老实交代
 *
 *   (b) 采集端自身过载：板子 CPU 被打满（比如别的进程吃满 CPU），
 *       采集线程抢不到 CPU，于是本该产出的事件没能产出
 *       → 此时网络可能是好的，UNKNOWN 掩盖了一个**本机性能问题**
 *
 * 旧证据对 (a) 和 (b) 完全同形。后果是双向的：
 *   - 把 (b) 报成网络故障 → 现场人员去查网络，查不出问题
 *   - 把 (b) 报成"观测不可靠" → 真实原因（板子过载）永远不被暴露
 *
 * 本模块把 CPU 压力作为**第二个独立维度**叠上去，让这两种情况可以被区分。
 *
 * ============================================================================
 * 为什么单独一个模块，而不是改 DnsServiceEvaluator
 * ============================================================================
 *
 * DnsServiceEvaluator 的设计契约（见其类注释）是"无状态纯函数，只吃 DNS 事实"。
 * 把 CPU 压力塞进去会让**观测者的状态污染被观测对象的判定**：
 * 同一个 DNS 窗口，在板子忙时和空闲时会得出不同结论。那会让 SR 系列
 * 约束（同源纪律、证据可复现）全部失效，也让单元测试失去可判定性。
 *
 * 正确分层：
 *   第 1 层 DnsServiceEvaluator —— 只回答"DNS 自身观测到了什么"
 *   第 2 层 ObserverAttribution（本模块）—— 回答"那份观测为什么不可靠"
 *
 * 本模块**不修改**第 1 层的任何 state/coverage/reason，只在它旁边追加
 * 一个独立的归因结论。上层可以同时看到两句话，而不是看到一句被篡改的话。
 *
 * ============================================================================
 * 语义边界（不得越过）
 * ============================================================================
 *
 * - CPU 忙 **不能**推出"网络是好的"。它只能推出"观测者自己有问题"。
 * - CPU 闲 **不能**推出"网络坏了"。它只能排除"CPU 过载"这一个成因。
 * - 归因结论永远不参与 OverallPolicy 的评分与 veto，只作为证据与告警呈现。
 *   理由：过载是本机的运行状态，不是被评估链路的健康度。
 */

#pragma once

#include "assurance/health_state.hpp"
#include <string>
#include <vector>

namespace weaknet {

/**
 * @brief observer 降级成因的归因结果
 */
enum class ObserverAttribution {
    /// 观测质量没问题，无需归因
    NONE,

    /// 观测降级了，但没有 CPU 压力数据可用（模块未加载/首轮基线）
    /// → 老实说"不知道原因"，不得猜。
    UNATTRIBUTED,

    /// 观测降级 **且** 采集端 CPU 处于高负载
    /// → 高度怀疑是采集端自身过载导致丢证据，而不是网络故障
    SELF_INFLICTED,

    /// 观测降级 **且** 采集端 CPU 正常
    /// → 排除了"CPU 过载"这一个成因，故障更可能在被观测链路侧
    NOT_HOST_CPU_BOUND
};

inline const char* observerAttributionToString(ObserverAttribution a) {
    switch (a) {
        case ObserverAttribution::NONE: return "NONE";
        case ObserverAttribution::UNATTRIBUTED: return "UNATTRIBUTED";
        case ObserverAttribution::SELF_INFLICTED: return "SELF_INFLICTED";
        case ObserverAttribution::NOT_HOST_CPU_BOUND: return "NOT_HOST_CPU_BOUND";
        default: return "UNATTRIBUTED";
    }
}

/**
 * @brief 归因结果详情
 */
struct ObserverAttributionResult {
    ObserverAttribution attribution{ObserverAttribution::NONE};

    /// 是否真的做了一次归因（false 表示输入里没有 observer 降级迹象，直接返回）
    bool evaluated{false};

    /// 触发归因的 SLE 名（如 "dns_service"），便于多 SLE 场景下定位
    std::string trigger_sle;

    /// 被归因的原始降级原因（原样保留，绝不改写）
    std::string observer_reason;

    /// 归因阈值：CPU 忙碌比达到多少算"过载"
    double cpu_threshold{0.85};

    /// 实际观测到的 CPU 忙率（仅在 available 时有意义）
    double observed_cpu_busy{0.0};
    bool cpu_available{false};

    /// 人类可读解释（进 warnings / 证据 detail）
    std::string explanation;

    /// 机器可读证据条目，直接挂到 SleResult.evidence 上
    std::vector<EvidenceItem> evidence;
};

/**
 * @brief 归因判定（无状态纯函数）
 *
 * @param sle          第 1 层评估结果（只读，不改）
 * @param sle_name     SLE 名称，进结果便于定位
 * @param cpu_busy     观测到的 CPU 忙率 0~1
 * @param cpu_available CPU 数据是否可用（false 时 cpu_busy 被忽略）
 * @param cfg          阈值配置
 *
 * 判定规则：
 *   1. 若 sle.reason 不是已知的 observer 降级原因 → NONE，evaluated=false
 *   2. 若 CPU 数据不可用 → UNATTRIBUTED（不猜）
 *   3. CPU 忙率 >= 阈值 → SELF_INFLICTED
 *   4. 否则 → NOT_HOST_CPU_BOUND
 *
 * 为什么用 reason 字符串前缀而不是给 SleResult 加枚举字段：
 * 加字段会改动所有 SleResult 生产者的初始化路径与全部下游序列化，
 * 收益却只是省一次字符串比较。reason 已经是稳定契约（有单元测试覆盖），
 * 而这里一旦匹配不上只会退化成 NONE（保守：不归因），不会误报。
 */
ObserverAttributionResult attributeObserverDegradation(
    const SleResult& sle,
    const std::string& sle_name,
    double cpu_busy,
    bool cpu_available,
    double cpu_threshold = 0.85);

}  // namespace weaknet

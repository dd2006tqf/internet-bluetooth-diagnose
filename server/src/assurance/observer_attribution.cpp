/**
 * @file observer_attribution.cpp
 * @brief 观测者降级归因实现
 */

#include "assurance/observer_attribution.hpp"

namespace weaknet {

namespace {

/// 已知的 observer 降级原因前缀。
///
/// 这些字符串由 src/dns_service_evaluator.hpp 的 checkObserverQuality() 产生，
/// 有单元测试逐条覆盖，是稳定契约。任何一项都表示「证据在传输/跟踪环节丢了」，
/// 而不是「网络本身观测到了故障」。
///
/// 注意 observer_unreliable_capture_emit_failure 与 observer_unreliable_event_loss
/// 是两级不同的丢失点（BPF 输出 vs perf 投递），刻意分开不合并——两者是否
/// 描述同一轮拥塞尚未验证，合并会掩盖真实边界。
constexpr const char* kObserverReasonPrefixes[] = {
    "observer_unreliable_capture_emit_failure",
    "observer_unreliable_event_loss",
    "observer_unreliable_ambiguity",
    "tracker_capacity_overflow",
};

bool isObserverDegradationReason(const std::string& reason) {
    for (const char* prefix : kObserverReasonPrefixes) {
        if (reason == prefix) return true;
    }
    return false;
}

}  // namespace

ObserverAttributionResult attributeObserverDegradation(
    const SleResult& sle,
    const std::string& sle_name,
    double cpu_busy,
    bool cpu_available,
    double cpu_threshold) {
    ObserverAttributionResult res;
    res.trigger_sle = sle_name;
    res.observer_reason = sle.reason;
    res.cpu_threshold = cpu_threshold;

    // 1. 这个 SLE 根本没在报 observer 问题 → 不归因
    //
    // 注意不能只看 state：observer 降级时 SLE 可能是 UNKNOWN（无直接坏证据）
    // 也可能是 DEGRADED/BAD（有直接坏证据但观测不可靠）。判定依据必须是 reason。
    if (!isObserverDegradationReason(sle.reason)) {
        return res;
    }

    res.evaluated = true;
    res.cpu_available = cpu_available;
    res.observed_cpu_busy = cpu_busy;

    // 2. 没有 CPU 数据 → 老实说不知道
    //
    // 这是最常见的现场情况：内核模块不在 OTA manifest 里，设备上没有
    // /dev/weaknet_cpu_stat。此时绝不能因为"没数据"就默认 CPU 正常，
    // 也不能默认它有问题。UNATTRIBUTED 是唯一诚实的答案。
    if (!cpu_available) {
        res.attribution = ObserverAttribution::UNATTRIBUTED;
        res.explanation =
            "Observer quality degraded, but host CPU pressure data is unavailable; "
            "cause cannot be attributed (kernel module /dev/weaknet_cpu_stat not present)";
        res.evidence.push_back({"observer_attribution",
                                static_cast<double>(ObserverAttribution::UNATTRIBUTED),
                                "Observer degraded but host CPU data unavailable; not guessing"});
        return res;
    }

    // 3. CPU 高负载 → 高度怀疑是采集端自身过载
    if (cpu_busy >= cpu_threshold) {
        res.attribution = ObserverAttribution::SELF_INFLICTED;
        res.explanation =
            "Observer quality degraded while host CPU was saturated; "
            "evidence loss is most likely caused by the collector itself being starved of CPU, "
            "not by the observed network";
        res.evidence.push_back({"observer_attribution",
                                static_cast<double>(ObserverAttribution::SELF_INFLICTED),
                                "Observer degraded with high host CPU; suspect self-inflicted evidence loss"});
        res.evidence.push_back({"host_cpu_busy_ratio", cpu_busy,
                                "Host CPU busy ratio at the time of observer degradation"});
        return res;
    }

    // 4. CPU 正常 → 排除了过载这一个成因
    res.attribution = ObserverAttribution::NOT_HOST_CPU_BOUND;
    res.explanation =
        "Observer quality degraded while host CPU was within normal range; "
        "host CPU starvation is ruled out as the cause";
    res.evidence.push_back({"observer_attribution",
                            static_cast<double>(ObserverAttribution::NOT_HOST_CPU_BOUND),
                            "Observer degraded with normal host CPU; CPU starvation ruled out"});
    res.evidence.push_back({"host_cpu_busy_ratio", cpu_busy,
                            "Host CPU busy ratio at the time of observer degradation"});
    return res;
}

}  // namespace weaknet

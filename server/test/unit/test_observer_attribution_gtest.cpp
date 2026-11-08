/**
 * @file test_observer_attribution_gtest.cpp
 * @brief 观测者降级归因真值表 + CPU 采样器降级语义
 *
 * 覆盖三类必须钉死的语义：
 *   1. 归因四分支 —— 必须能在证据上区分"网络真的坏了"与"采集端自己过载"，
 *                    且数据缺失时绝不猜。
 *   2. 软中断细分 —— CPU 饱和时再分一层「收包主导」与「非网络负载主导」，
 *                    前者与观测对象相关，后者才是真正意义的自伤。
 *                    细分**不得**改变 ObserverAttribution 枚举值。
 *   3. 采样器降级 —— 设备节点不存在时必须报不可用，**不得回落成 0**。
 *                    0 会被下游解读成"CPU 很闲"（或"没有收包软中断"），
 *                    从而掩盖真实故障。
 */

#include <gtest/gtest.h>

#include "assurance/observer_attribution.hpp"
#include "host_pressure_monitor.hpp"

using namespace weaknet;
using namespace weaknet_dbus;

namespace {

/// 造一个"报 observer 降级"的 SLE 结果
SleResult makeDegraded(const std::string& reason) {
    SleResult s;
    s.state = HealthState::UNKNOWN;
    s.coverage = Coverage::PARTIAL;
    s.reason = reason;
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 不涉及 observer 的 SLE → 不归因
// ---------------------------------------------------------------------------

TEST(ObserverAttribution, HealthyObservationIsNotAttributed) {
    SleResult s;
    s.state = HealthState::GOOD;
    s.reason = "dns_resolution_healthy";

    auto r = attributeObserverDegradation(s, "dns_service", 0.99, true);

    EXPECT_EQ(r.attribution, ObserverAttribution::NONE);
    EXPECT_FALSE(r.evaluated);
    EXPECT_TRUE(r.evidence.empty());
    EXPECT_EQ(r.trigger_sle, "dns_service");
    // 原始 reason 必须原样保留，绝不被改写
    EXPECT_EQ(r.observer_reason, "dns_resolution_healthy");
}

TEST(ObserverAttribution, UnknownWithoutObserverReasonIsNotAttributed) {
    // 只是"没观测到"，不是"观测不可靠"。两者不可混为一谈：
    // 前者 CPU 再忙也与归因无关，乱归因会让 UNKNOWN 变成噪声。
    SleResult s;
    s.state = HealthState::UNKNOWN;
    s.reason = "no_dns_observations";

    auto r = attributeObserverDegradation(s, "dns_service", 0.99, true);

    EXPECT_EQ(r.attribution, ObserverAttribution::NONE);
    EXPECT_FALSE(r.evaluated);
}

// ---------------------------------------------------------------------------
// 2. observer 降级 × 无 CPU 数据 → UNATTRIBUTED（不猜）
// ---------------------------------------------------------------------------

TEST(ObserverAttribution, DegradedWithoutCpuDataIsUnattributed) {
    auto s = makeDegraded("observer_unreliable_capture_emit_failure");

    auto r = attributeObserverDegradation(s, "dns_service", 0.0, false);

    EXPECT_EQ(r.attribution, ObserverAttribution::UNATTRIBUTED);
    EXPECT_TRUE(r.evaluated);
    EXPECT_FALSE(r.cpu_available);
    // 缺数据时不能被当成"CPU 正常"：那会把本机问题甩给网络
    EXPECT_NE(r.attribution, ObserverAttribution::NOT_HOST_CPU_BOUND);
    // 也不能被当成"CPU 过载"：那是凭空指控
    EXPECT_NE(r.attribution, ObserverAttribution::SELF_INFLICTED);
    EXPECT_FALSE(r.explanation.empty());
}

TEST(ObserverAttribution, UnavailableCpuIgnoresCarriedValue) {
    // 即便 cpu_busy 传了 0.99，只要 available=false 就必须忽略该值。
    // 这条防的是"调用方忘了检查 available，顺手把默认值当实测值传进来"。
    auto s = makeDegraded("observer_unreliable_event_loss");

    auto r = attributeObserverDegradation(s, "dns_service", 0.99, false);

    EXPECT_EQ(r.attribution, ObserverAttribution::UNATTRIBUTED);
    EXPECT_FALSE(r.cpu_available);
}

// ---------------------------------------------------------------------------
// 3. observer 降级 × CPU 过载 → SELF_INFLICTED
// ---------------------------------------------------------------------------

TEST(ObserverAttribution, DegradedWithSaturatedCpuIsSelfInflicted) {
    auto s = makeDegraded("observer_unreliable_capture_emit_failure");

    auto r = attributeObserverDegradation(s, "dns_service", 0.95, true);

    EXPECT_EQ(r.attribution, ObserverAttribution::SELF_INFLICTED);
    EXPECT_TRUE(r.evaluated);
    EXPECT_TRUE(r.cpu_available);
    EXPECT_DOUBLE_EQ(r.observed_cpu_busy, 0.95);
    EXPECT_DOUBLE_EQ(r.cpu_threshold, 0.85);

    // 证据必须同时包含"归因结论"与"支撑它的 CPU 实测值"
    bool has_attr = false, has_cpu = false;
    for (const auto& e : r.evidence) {
        if (e.metric == "observer_attribution") has_attr = true;
        if (e.metric == "host_cpu_busy_ratio") has_cpu = true;
    }
    EXPECT_TRUE(has_attr);
    EXPECT_TRUE(has_cpu);
}

TEST(ObserverAttribution, ThresholdIsInclusive) {
    // 恰好等于阈值就算过载：边界值必须可判定，不留"擦边不算"的灰区
    auto s = makeDegraded("tracker_capacity_overflow");

    auto r = attributeObserverDegradation(s, "dns_service", 0.85, true);

    EXPECT_EQ(r.attribution, ObserverAttribution::SELF_INFLICTED);
}

// ---------------------------------------------------------------------------
// 4. observer 降级 × CPU 正常 → NOT_HOST_CPU_BOUND
// ---------------------------------------------------------------------------

TEST(ObserverAttribution, DegradedWithNormalCpuRulesOutHostStarvation) {
    auto s = makeDegraded("observer_unreliable_ambiguity");

    auto r = attributeObserverDegradation(s, "dns_service", 0.20, true);

    EXPECT_EQ(r.attribution, ObserverAttribution::NOT_HOST_CPU_BOUND);
    EXPECT_TRUE(r.evaluated);
    EXPECT_TRUE(r.cpu_available);

    // 语义边界：CPU 正常**不能**推出"网络坏了"，只能排除 CPU 过载这一个成因。
    // 用词必须保守，不能出现断言网络故障的表述。
    EXPECT_EQ(r.explanation.find("suspect self-inflicted"), std::string::npos);
}

TEST(ObserverAttribution, CustomThresholdIsHonored) {
    auto s = makeDegraded("observer_unreliable_event_loss");

    // 阈值为 0.5 时，0.60 应判过载，0.40 应判排除
    auto high = attributeObserverDegradation(s, "dns_service", 0.60, true, 0.50);
    auto low = attributeObserverDegradation(s, "dns_service", 0.40, true, 0.50);

    EXPECT_EQ(high.attribution, ObserverAttribution::SELF_INFLICTED);
    EXPECT_EQ(low.attribution, ObserverAttribution::NOT_HOST_CPU_BOUND);
    EXPECT_DOUBLE_EQ(high.cpu_threshold, 0.50);
}

// ---------------------------------------------------------------------------
// 4b. SELF_INFLICTED 的软中断细分（第 3a 步）
// ---------------------------------------------------------------------------

namespace {

/// 在证据里找某个指标名对应的条目
const EvidenceItem* findEvidence(const std::vector<EvidenceItem>& ev, const std::string& name) {
    for (const auto& e : ev) {
        if (e.metric == name) return &e;
    }
    return nullptr;
}

}  // namespace

TEST(ObserverAttributionSoftirq, ReceiveDominatedKeepsSelfInflictedAndExplainsLinkCorrelation) {
    // CPU 饱和 + 收包软中断占主导 → 结论仍是 SELF_INFLICTED，
    // 但解释里要点明"过载来自被观测链路的流量"，而不是无关进程。
    auto s = makeDegraded("observer_unreliable_capture_emit_failure");

    auto r = attributeObserverDegradation(s, "dns_service", 0.95, true, 0.85, 0.85, true);

    EXPECT_EQ(r.attribution, ObserverAttribution::SELF_INFLICTED);
    EXPECT_TRUE(r.softirq_available);
    EXPECT_DOUBLE_EQ(r.softirq_net_rx_share, 0.85);
    EXPECT_DOUBLE_EQ(r.softirq_share_threshold, 0.5);

    // 原有解释必须原样保留在最前面：它已经进 warnings，被用户看到
    EXPECT_EQ(r.explanation.find("Observer quality degraded while host CPU was saturated"),
              std::size_t{0});
    // 细分说明追加在后
    EXPECT_NE(r.explanation.find("dominated by the network receive path"), std::string::npos);
    EXPECT_EQ(r.explanation.find("NOT dominated"), std::string::npos);

    const EvidenceItem* e = findEvidence(r.evidence, "softirq_net_rx_share");
    ASSERT_NE(e, nullptr);
    EXPECT_DOUBLE_EQ(e->value, 0.85);
}

TEST(ObserverAttributionSoftirq, NonReceiveDominatedSaysUnrelatedLocalWorkload) {
    // CPU 饱和 + 收包不占主导 → 仍然是 SELF_INFLICTED，但这是真正意义的自伤：
    // 把它压满的是与网络无关的本机负载。两种处置方向必须能被区分开。
    auto s = makeDegraded("observer_unreliable_event_loss");

    auto r = attributeObserverDegradation(s, "dns_service", 0.95, true, 0.85, 0.05, true);

    EXPECT_EQ(r.attribution, ObserverAttribution::SELF_INFLICTED);
    EXPECT_TRUE(r.softirq_available);
    EXPECT_NE(r.explanation.find("NOT dominated by the network receive path"),
              std::string::npos);
    EXPECT_NE(r.explanation.find("unrelated local workload"), std::string::npos);
    // 互斥断言必须带否定词："NOT dominated by..." 本身包含
    // "dominated by..." 子串，直接用后者做 find 会假阳性。
    EXPECT_EQ(r.explanation.find("activity is dominated by"), std::string::npos);
}

TEST(ObserverAttributionSoftirq, ShareThresholdIsInclusiveAndConfigurable) {
    auto s = makeDegraded("tracker_capacity_overflow");

    // 默认阈值 0.5，恰好等于即算主导
    auto at = attributeObserverDegradation(s, "dns_service", 0.95, true, 0.85, 0.5, true);
    EXPECT_NE(at.explanation.find("dominated by the network receive path"), std::string::npos);
    EXPECT_EQ(at.explanation.find("NOT dominated"), std::string::npos);

    // 自定义阈值 0.9 时，同一个 0.5 就落到"非主导"
    auto custom = attributeObserverDegradation(s, "dns_service", 0.95, true, 0.85, 0.5, true, 0.9);
    EXPECT_NE(custom.explanation.find("NOT dominated"), std::string::npos);
    EXPECT_DOUBLE_EQ(custom.softirq_share_threshold, 0.9);
}

TEST(ObserverAttributionSoftirq, UnavailableSoftirqAddsNoSpeculation) {
    // 软中断数据不可用 → 保持沉默，绝不猜。
    // 这是本细分最重要的语义边界：没数据就说没数据，与 UNATTRIBUTED 同一纪律。
    auto s = makeDegraded("observer_unreliable_ambiguity");

    auto r = attributeObserverDegradation(s, "dns_service", 0.95, true);

    EXPECT_EQ(r.attribution, ObserverAttribution::SELF_INFLICTED);
    EXPECT_FALSE(r.softirq_available);
    EXPECT_EQ(findEvidence(r.evidence, "softirq_net_rx_share"), nullptr);
    // 解释里不能出现任何关于软中断构成的断言
    EXPECT_EQ(r.explanation.find("softirq"), std::string::npos);
}

TEST(ObserverAttributionSoftirq, SoftirqIsIgnoredWhenCpuIsNotSaturated) {
    // CPU 没饱和时，软中断构成比根本没有被咨询的资格——
    // 一台跑大流量的机器 NET_RX 占比天然高，脱离 CPU 饱和去解读它必然误判。
    auto s = makeDegraded("observer_unreliable_capture_emit_failure");

    auto r = attributeObserverDegradation(s, "dns_service", 0.10, true, 0.85, 0.99, true);

    EXPECT_EQ(r.attribution, ObserverAttribution::NOT_HOST_CPU_BOUND);
    EXPECT_EQ(findEvidence(r.evidence, "softirq_net_rx_share"), nullptr);
    EXPECT_EQ(r.explanation.find("softirq"), std::string::npos);
}

TEST(ObserverAttributionSoftirq, SoftirqIsIgnoredWhenCpuDataUnavailable) {
    // CPU 数据缺失时先落到 UNATTRIBUTED，软中断细分成不了事：
    // 没有"饱和"这个前提，构成比无法被框定。
    auto s = makeDegraded("observer_unreliable_event_loss");

    auto r = attributeObserverDegradation(s, "dns_service", 0.0, false, 0.85, 0.99, true);

    EXPECT_EQ(r.attribution, ObserverAttribution::UNATTRIBUTED);
    EXPECT_EQ(findEvidence(r.evidence, "softirq_net_rx_share"), nullptr);
}

TEST(ObserverAttributionSoftirq, DefaultArgumentsPreserveLegacyBehaviour) {
    // 回归：不传软中断参数的调用点，行为必须与引入本细分之前逐字一致。
    auto s = makeDegraded("observer_unreliable_capture_emit_failure");

    const auto legacy = attributeObserverDegradation(s, "dns_service", 0.95, true);
    const auto explicit_none =
        attributeObserverDegradation(s, "dns_service", 0.95, true, 0.85, 0.0, false, 0.5);

    EXPECT_EQ(legacy.attribution, explicit_none.attribution);
    EXPECT_EQ(legacy.explanation, explicit_none.explanation);
    EXPECT_EQ(legacy.evidence.size(), explicit_none.evidence.size());
    EXPECT_FALSE(legacy.softirq_available);
}

// ---------------------------------------------------------------------------
// 5. 四种已知 observer 降级原因都必须被识别
// ---------------------------------------------------------------------------

TEST(ObserverAttribution, AllKnownObserverReasonsAreRecognized) {
    const char* reasons[] = {
        "observer_unreliable_capture_emit_failure",
        "observer_unreliable_event_loss",
        "observer_unreliable_ambiguity",
        "tracker_capacity_overflow",
    };

    for (const char* reason : reasons) {
        auto r = attributeObserverDegradation(makeDegraded(reason), "dns_service", 0.10, true);
        EXPECT_TRUE(r.evaluated) << "reason not recognized: " << reason;
        EXPECT_NE(r.attribution, ObserverAttribution::NONE) << "reason: " << reason;
    }
}

TEST(ObserverAttribution, SimilarButUnknownReasonIsNotRecognized) {
    // 前缀相同的未知串不应被吞进归因（保守退化，不误报）
    auto s = makeDegraded("observer_unreliable_something_new");

    auto r = attributeObserverDegradation(s, "dns_service", 0.99, true);

    EXPECT_EQ(r.attribution, ObserverAttribution::NONE);
    EXPECT_FALSE(r.evaluated);
}

TEST(ObserverAttribution, ToStringCoversAllStates) {
    EXPECT_STREQ(observerAttributionToString(ObserverAttribution::NONE), "NONE");
    EXPECT_STREQ(observerAttributionToString(ObserverAttribution::UNATTRIBUTED), "UNATTRIBUTED");
    EXPECT_STREQ(observerAttributionToString(ObserverAttribution::SELF_INFLICTED), "SELF_INFLICTED");
    EXPECT_STREQ(observerAttributionToString(ObserverAttribution::NOT_HOST_CPU_BOUND),
                 "NOT_HOST_CPU_BOUND");
}

// ---------------------------------------------------------------------------
// 6. CPU 采样器降级：设备不存在绝不伪造 0
// ---------------------------------------------------------------------------

TEST(HostCpuPressureSampler, MissingDeviceReportsUnavailableNotZero) {
    HostCpuPressureSampler sampler("/nonexistent/weaknet_cpu_stat_probe");

    auto p = sampler.sample();

    EXPECT_FALSE(p.available);
    EXPECT_EQ(p.reason, "device_absent");
    // 关键断言：不得把"读不到"伪装成"CPU 很闲"
    // （busy_ratio 保持默认 0，但 available=false 才是权威语义）
    EXPECT_EQ(p.sampled_cpus, 0u);
}

TEST(HostCpuPressureSampler, MissingDeviceIsReportedEveryTime) {
    // 懒打开 + 每轮重试：模块后插时应能自愈，但在此之前每轮都必须老实报缺失，
    // 不能第一轮报缺失、之后静默继承旧状态。
    HostCpuPressureSampler sampler("/nonexistent/weaknet_cpu_stat_probe");

    for (int i = 0; i < 3; ++i) {
        auto p = sampler.sample();
        EXPECT_FALSE(p.available) << "round " << i;
        EXPECT_EQ(p.reason, "device_absent") << "round " << i;
    }
}

TEST(HostCpuPressureSampler, PermissionDeniedIsNotReportedAsDeviceAbsent) {
    // 回归守护（2026-11-07 真机踩坑）：
    //
    // 模块的 open() 曾经只校验 CAP_SYS_ADMIN，而服务的 systemd 单元只授予
    // CAP_PERFMON 等能力。结果是设备节点明明存在、模块明明加载着，服务端
    // 却一直报 "device_absent" —— 因为它把**所有** open() 失败都映射成了
    // 这一个原因。排查时先怀疑了模块没装、又怀疑了 namespace/DAC，
    // 最后才发现是 EPERM。
    //
    // 这条测试钉死分类边界：设备不存在（ENOENT/ENXIO）与打不开（EPERM/
    // EACCES）必须是两个不同的 reason，因为处置方式完全不同
    // —— 前者装模块，后者改 capability。
    //
    // 为什么直接测纯函数而不是造真实的 open 失败：root 会绕过大部分权限
    // 检查，无法构造在 root 与普通用户下表现一致的夹具。分类逻辑本身
    // 与运行身份无关，抽出来测才稳定。
    EXPECT_STREQ(openErrnoToReason(EPERM), "open_failed");
    EXPECT_STREQ(openErrnoToReason(EACCES), "open_failed");
    EXPECT_STREQ(openErrnoToReason(EISDIR), "open_failed");
    EXPECT_STREQ(openErrnoToReason(EBUSY), "open_failed");
    EXPECT_STREQ(openErrnoToReason(EMFILE), "open_failed");
}

TEST(HostCpuPressureSampler, GenuinelyMissingDeviceMapsToDeviceAbsent) {
    // 反向守护：真正的"缺失"必须仍然报 device_absent。
    // 现场设备没装模块是常态，这个语义不能被上面的细化改动误伤。
    EXPECT_STREQ(openErrnoToReason(ENOENT), "device_absent");
    EXPECT_STREQ(openErrnoToReason(ENXIO), "device_absent");

    // 端到端确认：不存在的路径确实走 ENOENT 分支
    HostCpuPressureSampler sampler("/nonexistent/weaknet_cpu_stat_probe");
    auto p = sampler.sample();
    EXPECT_FALSE(p.available);
    EXPECT_EQ(p.reason, "device_absent");
    EXPECT_FALSE(p.detail.empty()) << "detail 应带 errno 文本，供排障定位";
    EXPECT_NE(p.detail.find("open: "), std::string::npos)
        << "实际 detail: " << p.detail;
}

TEST(HostCpuPressureSampler, ReportsConfiguredDevicePath) {
    HostCpuPressureSampler sampler("/tmp/some_probe_node");
    EXPECT_EQ(sampler.devPath(), "/tmp/some_probe_node");
}

TEST(HostCpuPressureSampler, DefaultDevicePathMatchesKernelModuleContract) {
    HostCpuPressureSampler sampler;
    EXPECT_EQ(sampler.devPath(), "/dev/weaknet_cpu_stat");
}

// ---------------------------------------------------------------------------
// 7. 软中断构成比采样器的降级语义
//
// 与 CPU 采样器同一纪律：设备缺失报 unavailable，**绝不回落成 0**。
// 0 会被下游读成"完全没有收包软中断"，那是伪造的观测，而不是缺数据。
// ---------------------------------------------------------------------------

TEST(HostSoftirqShareSampler, MissingDeviceReportsUnavailableNotZero) {
    HostSoftirqShareSampler sampler("/nonexistent/weaknet_softirq_probe");

    auto q = sampler.sample();

    EXPECT_FALSE(q.available);
    EXPECT_EQ(q.reason, "device_absent");
    // 关键断言：不可用时 net_rx_share 必须是"无意义"，而不是一个看起来
    // 像真实观测的 0。结构体默认就是 0，但 available=false 是唯一的
    // 解释入口——两者必须同时成立，不能只有其一。
    EXPECT_DOUBLE_EQ(q.net_rx_share, 0.0);
    EXPECT_FALSE(q.detail.empty()) << "detail 应带 errno 文本，供排障定位";
    EXPECT_NE(q.detail.find("open: "), std::string::npos)
        << "实际 detail: " << q.detail;
}

TEST(HostSoftirqShareSampler, MissingDeviceIsReportedEveryTime) {
    // 与 CPU 采样器同款回归：懒打开 + 每轮重试，模块后插应能自愈；
    // 在此之前每轮都老实报缺失，不得静默继承旧状态。
    HostSoftirqShareSampler sampler("/nonexistent/weaknet_softirq_probe");

    for (int i = 0; i < 3; ++i) {
        auto q = sampler.sample();
        EXPECT_FALSE(q.available) << "round " << i;
        EXPECT_EQ(q.reason, "device_absent") << "round " << i;
    }
}

TEST(HostSoftirqShareSampler, ReportsConfiguredDevicePath) {
    HostSoftirqShareSampler sampler("/tmp/some_probe_node");
    EXPECT_EQ(sampler.devPath(), "/tmp/some_probe_node");
}

TEST(HostSoftirqShareSampler, DefaultDevicePathMatchesKernelModuleContract) {
    // 与 tools/weaknet-kmod.service 的 insmod 路径、kmod 的
    // device_create 名字构成一条三处必须一致的契约。
    HostSoftirqShareSampler sampler;
    EXPECT_EQ(sampler.devPath(), "/dev/weaknet_softirq");
}

TEST(HostSoftirqShareSampler, TwoSamplersHaveIndependentState) {
    // 两个采样器实例不得共享基线/映射：它们将来可能被不同线程持有。
    // 这条测试防止有人把 Impl 改成静态变量图省事。
    HostSoftirqShareSampler a("/nonexistent/softirq_probe_a");
    HostSoftirqShareSampler b("/nonexistent/softirq_probe_b");

    EXPECT_EQ(a.devPath(), "/nonexistent/softirq_probe_a");
    EXPECT_EQ(b.devPath(), "/nonexistent/softirq_probe_b");

    auto qa = a.sample();
    auto qb = b.sample();
    EXPECT_EQ(qa.reason, "device_absent");
    EXPECT_EQ(qb.reason, "device_absent");
}

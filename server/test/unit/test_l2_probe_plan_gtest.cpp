/**
 * @file test_l2_probe_plan_gtest.cpp
 * @brief 二层探测调度计划器的纯逻辑测试（不需要网络、不需要 CAP_NET_RAW）
 *
 * 这里锁死的是整个方案里最容易**静默失效**的部分：
 * 限速或批次上限吞掉了一部分帧，但结论照常给出。
 * 那种缺陷不报错、不崩溃，只让运维看到基于残缺样本的自信结论。
 */

#include <gtest/gtest.h>

#include "assurance/l2_loop_attribution.hpp"
#include "assurance/l2_probe_plan.hpp"

namespace {

using weaknet::L2ProbeFramePlan;
using weaknet::L2ProbeRoundPlan;
using weaknet::L2ProbeTarget;
using weaknet::L2ProbeTokenBucket;
using weaknet::planL2ProbeRound;
using weaknet::checkL2ProbeEgressEligibility;

L2ProbeTarget makeTarget(const std::string& name, int32_t vlan = -1) {
    L2ProbeTarget t;
    t.ifname = name;
    t.vlan_id = vlan;
    return t;
}

// ============================================================================
// 令牌桶
// ============================================================================

TEST(L2ProbeTokenBucketTest, StartsFullSoFirstRoundIsNotDegraded) {
    // 首次探测不该因为"还没攒够令牌"而一帧都发不出去——
    // 那会让全新部署直接判 DEGRADED，看起来像配置错误。
    L2ProbeTokenBucket bucket(4);
    EXPECT_EQ(bucket.available(), 4u);
}

TEST(L2ProbeTokenBucketTest, ConsumesUntilEmptyThenRefuses) {
    L2ProbeTokenBucket bucket(2);
    EXPECT_TRUE(bucket.tryConsume());
    EXPECT_TRUE(bucket.tryConsume());
    EXPECT_FALSE(bucket.tryConsume());
    EXPECT_EQ(bucket.available(), 0u);
}

TEST(L2ProbeTokenBucketTest, RefillAccumulatesSubTokenRemainder) {
    // 4pps → 250ms/令牌。若调度粒度是 100ms 且不累计零头，
    // 永远只能攒到 100ms 然后丢弃，令牌永远补不满。
    L2ProbeTokenBucket bucket(4);
    while (bucket.tryConsume()) {}   // 清空
    EXPECT_EQ(bucket.available(), 0u);

    bucket.refill(100);   // 100ms → 0 令牌，但 100ms 要留着
    EXPECT_EQ(bucket.available(), 0u);
    bucket.refill(100);   // 累计 200ms → 仍 0
    EXPECT_EQ(bucket.available(), 0u);
    bucket.refill(100);   // 累计 300ms → 1 令牌，剩 50ms
    EXPECT_EQ(bucket.available(), 1u);
    bucket.refill(200);   // 累计 250ms → 再 1 令牌
    EXPECT_EQ(bucket.available(), 2u);
}

TEST(L2ProbeTokenBucketTest, RefillNeverExceedsCapacity) {
    L2ProbeTokenBucket bucket(4);
    bucket.refill(60000);   // 一分钟
    EXPECT_EQ(bucket.available(), 4u);   // 容量 = 速率，不积累超出
}

TEST(L2ProbeTokenBucketTest, ZeroRateDegradesToNoDeadlock) {
    // rate=0 是非法配置（调用方应拒绝），但绝不能除零或死循环
    L2ProbeTokenBucket bucket(0);
    EXPECT_GE(bucket.available(), 0u);
    bucket.refill(1000);   // 不崩即可
}

// ============================================================================
// 计划器：覆盖完整性
// ============================================================================

TEST(L2ProbePlanTest, EmptyTargetsIsCoveredNotDegraded) {
    // 没有目标 = 无事可做，不是"覆盖不全"。
    // 这个区分很重要：空配置应报"未配置"，不该报"测不准"。
    L2ProbeTokenBucket bucket(10);
    auto plan = planL2ProbeRound({}, 8, &bucket, 1000);
    EXPECT_TRUE(plan.frames.empty());
    EXPECT_TRUE(plan.covered);
    EXPECT_EQ(plan.dropped_by_limit, 0u);
    EXPECT_TRUE(plan.degraded_reason.empty());
}

TEST(L2ProbePlanTest, AllTargetsGetAtLeastOneFrameWhenBudgetAllows) {
    L2ProbeTokenBucket bucket(10);
    std::vector<L2ProbeTarget> targets = {
        makeTarget("eth0"), makeTarget("eth1"), makeTarget("eth2"),
    };
    auto plan = planL2ProbeRound(targets, 8, &bucket, 0);

    EXPECT_TRUE(plan.covered);
    EXPECT_EQ(plan.dropped_by_limit, 0u);
    EXPECT_GE(plan.frames.size(), targets.size());

    // 每个口都必须被探到过：一帧没发的口会被误算进"无回流"
    for (const auto& t : targets) {
        bool found = false;
        for (const auto& f : plan.frames) {
            if (f.target.ifname == t.ifname) { found = true; break; }
        }
        EXPECT_TRUE(found) << "target " << t.ifname << " got no frame";
    }
}

TEST(L2ProbePlanTest, BatchLimitSmallerThanTargetCountMarksDegraded) {
    // 4 个口、批次上限 2：只有前两个口测到，后两个一无所知。
    // 这必须判为覆盖不全——否则后两个口会以"没产生回流"被计入 NO_LOOP，
    // 那是把「没测」当成「测了没问题」。
    L2ProbeTokenBucket bucket(100);
    std::vector<L2ProbeTarget> targets = {
        makeTarget("eth0"), makeTarget("eth1"),
        makeTarget("eth2"), makeTarget("eth3"),
    };
    auto plan = planL2ProbeRound(targets, 2, &bucket, 0);

    EXPECT_FALSE(plan.covered);
    EXPECT_EQ(plan.dropped_by_limit, 2u);
    EXPECT_EQ(plan.degraded_reason, "send_throttled");
    EXPECT_EQ(plan.frames.size(), 2u);
}

TEST(L2ProbePlanTest, RateLimitExhaustionMarksDegraded) {
    // 令牌只有 1 个，但有 3 个口 → 2 个口没测到 → 必须降级
    L2ProbeTokenBucket bucket(1);
    std::vector<L2ProbeTarget> targets = {
        makeTarget("eth0"), makeTarget("eth1"), makeTarget("eth2"),
    };
    auto plan = planL2ProbeRound(targets, 64, &bucket, 0);

    EXPECT_FALSE(plan.covered);
    EXPECT_EQ(plan.dropped_by_limit, 2u);
    EXPECT_EQ(plan.frames.size(), 1u);
    EXPECT_EQ(plan.degraded_reason, "send_throttled");
}

TEST(L2ProbePlanTest, ExactlyEnoughBudgetForOneRoundIsCovered) {
    // 边界：4 个口、批次上限恰好 4、令牌恰好 4。
    // 历史缺陷：曾经用"是否用满批次"判覆盖，这种情况下会把完整轮报成降级。
    L2ProbeTokenBucket bucket(4);
    std::vector<L2ProbeTarget> targets = {
        makeTarget("eth0"), makeTarget("eth1"),
        makeTarget("eth2"), makeTarget("eth3"),
    };
    auto plan = planL2ProbeRound(targets, 4, &bucket, 0);

    EXPECT_TRUE(plan.covered) << "满额一轮不该被判为覆盖不全";
    EXPECT_EQ(plan.dropped_by_limit, 0u);
    EXPECT_TRUE(plan.degraded_reason.empty());
    EXPECT_EQ(plan.frames.size(), 4u);
}

TEST(L2ProbePlanTest, ZeroMaxFramesIsDegradedNotSilentSuccess) {
    L2ProbeTokenBucket bucket(10);
    std::vector<L2ProbeTarget> targets = {makeTarget("eth0"), makeTarget("eth1")};
    auto plan = planL2ProbeRound(targets, 0, &bucket, 0);

    EXPECT_FALSE(plan.covered);
    EXPECT_EQ(plan.dropped_by_limit, 2u);
    EXPECT_TRUE(plan.frames.empty());
}

TEST(L2ProbePlanTest, NullBucketIsDegradedNotCrash) {
    std::vector<L2ProbeTarget> targets = {makeTarget("eth0")};
    auto plan = planL2ProbeRound(targets, 8, nullptr, 0);

    EXPECT_FALSE(plan.covered);
    EXPECT_EQ(plan.dropped_by_limit, 1u);
    EXPECT_TRUE(plan.frames.empty());
}

// ============================================================================
// 计划器：序号与 VLAN
// ============================================================================

TEST(L2ProbePlanTest, SequenceIsContiguousFromZero) {
    // seq 是回流匹配的唯一键，必须无缝递增，否则收到帧无法与发送账本对齐
    L2ProbeTokenBucket bucket(10);
    std::vector<L2ProbeTarget> targets = {makeTarget("eth0"), makeTarget("eth1")};
    auto plan = planL2ProbeRound(targets, 10, &bucket, 0);

    for (size_t i = 0; i < plan.frames.size(); ++i) {
        EXPECT_EQ(plan.frames[i].seq, i);
    }
}

TEST(L2ProbePlanTest, VlanIdIsPreservedAndMinusOneMeansUntagged) {
    // vlan_id = -1 表示"不带 tag"，不是 0。
    // VLAN 0 是合法的 priority-tagged 语义，用 0 冒充"无 tag"违反
    // 项目「未采集 != 零值」的硬约束。
    L2ProbeTokenBucket bucket(10);
    std::vector<L2ProbeTarget> targets = {makeTarget("eth0", 100), makeTarget("eth1", -1)};
    auto plan = planL2ProbeRound(targets, 10, &bucket, 0);

    bool saw_100 = false, saw_untagged = false;
    for (const auto& f : plan.frames) {
        if (f.target.vlan_id == 100) saw_100 = true;
        if (f.target.vlan_id == -1) saw_untagged = true;
    }
    EXPECT_TRUE(saw_100);
    EXPECT_TRUE(saw_untagged);
}

TEST(L2ProbePlanTest, TotallyEmptyBudgetProducesNoFramesButStillDegraded) {
    // 令牌桶耗尽的极端情况：一帧都发不出去，仍要老实报覆盖不全，
    // 不能因为"什么都没做"就返回 covered=true
    L2ProbeTokenBucket bucket(1);
    bucket.tryConsume();   // 清空
    std::vector<L2ProbeTarget> targets = {makeTarget("eth0")};
    auto plan = planL2ProbeRound(targets, 8, &bucket, 0);

    EXPECT_TRUE(plan.frames.empty());
    EXPECT_FALSE(plan.covered);
    EXPECT_EQ(plan.dropped_by_limit, 1u);
}

}  // namespace

// ============================================================================
// 归因纯函数：l2_loop_attribution
//
// 这一段的重点不是"算出正确结论"，而是"**拒绝给结论**的四种情形必须真的拒绝"。
// 把「没测准」说成「没问题」，是这个能力最危险的失效方式。
// ============================================================================

TEST(L2LoopAttributionTest, NoProbeDataReturnsNoneNotNoLoop) {
    // 最基础的边界：一条数据都没有时，绝不能返回 NO_LOOP。
    // "我们没测过"与"我们测了，没问题"是两件完全不同的事。
    weaknet::L2ProbeWindow w;
    w.total_rounds = 0;
    auto r = weaknet::attributeL2Loop(w);

    EXPECT_EQ(r.attribution, weaknet::L2LoopAttribution::NONE);
    EXPECT_EQ(r.reason, "no_probe_data");
    EXPECT_FALSE(r.explanation.empty());
}

TEST(L2LoopAttributionTest, NoProbeDataDistinguishesUnconfiguredFromNotRun) {
    // 运维要能区分「没配出口」（正常，这个能力没启用）
    // 与「配了但没跑起来」（要查）。
    weaknet::L2ProbeWindow unconfigured;
    auto r1 = weaknet::attributeL2Loop(unconfigured);
    EXPECT_NE(r1.explanation.find("未配置"), std::string::npos);

    weaknet::L2ProbeWindow configured_but_idle;
    configured_but_idle.egress_ifaces = {"eth0"};
    auto r2 = weaknet::attributeL2Loop(configured_but_idle);
    EXPECT_EQ(r2.reason, "no_probe_data");
    EXPECT_EQ(r2.explanation.find("未配置"), std::string::npos)
        << "配了出口却报未配置，会让运维去查配置而非查运行状态";
}

TEST(L2LoopAttributionTest, AllRoundsDegradedReturnsNone) {
    // 跑了很多轮但全部不可信 == 没有数据。
    // 绝不能因为"轮次很多"就给出结论。
    weaknet::L2ProbeWindow w;
    w.total_rounds = 20;
    w.degraded_rounds = 20;
    w.multi_return_rounds = 0;
    auto r = weaknet::attributeL2Loop(w);

    EXPECT_EQ(r.attribution, weaknet::L2LoopAttribution::NONE);
    EXPECT_EQ(r.reason, "all_rounds_degraded");
}

TEST(L2LoopAttributionTest, DegradedRoundsAreExcludedFromDenominator) {
    // 10 轮里 8 轮降级，只有 2 轮有效，其中 1 轮回流。
    // 按有效轮次算 = 1/2 = 50% → LOOP_SUSPECTED。
    // 若错误地把降级轮算进分母 = 1/10 = 10% → 会得出 NONE。
    weaknet::L2ProbeWindow w;
    w.total_rounds = 10;
    w.degraded_rounds = 8;
    w.multi_return_rounds = 1;
    auto r = weaknet::attributeL2Loop(w);

    EXPECT_EQ(r.attribution, weaknet::L2LoopAttribution::LOOP_SUSPECTED)
        << "降级轮次必须从分母剔除，否则有效样本被稀释";
}

TEST(L2LoopAttributionTest, SingleValidRoundIsInsufficient) {
    // 单轮是样本量 1，无法区分"稳定存在"与 STP 收敛期瞬时现象。
    weaknet::L2ProbeWindow w;
    w.total_rounds = 1;
    w.degraded_rounds = 0;
    w.multi_return_rounds = 1;
    auto r = weaknet::attributeL2Loop(w);

    EXPECT_EQ(r.attribution, weaknet::L2LoopAttribution::NONE);
    EXPECT_EQ(r.reason, "insufficient_valid_rounds");
}

TEST(L2LoopAttributionTest, TwoValidRoundsWithoutReturnIsNoLoop) {
    // 唯一可以说"没看到回流"的情形：至少两轮有效测量。
    weaknet::L2ProbeWindow w;
    w.total_rounds = 2;
    w.degraded_rounds = 0;
    w.multi_return_rounds = 0;
    auto r = weaknet::attributeL2Loop(w);

    EXPECT_EQ(r.attribution, weaknet::L2LoopAttribution::NO_LOOP);
    EXPECT_TRUE(r.reason.empty());
}

TEST(L2LoopAttributionTest, MajorityReturnIsSuspected) {
    weaknet::L2ProbeWindow w;
    w.total_rounds = 10;
    w.degraded_rounds = 0;
    w.multi_return_rounds = 6;
    w.return_pairs = {"eth0->eth1"};
    auto r = weaknet::attributeL2Loop(w);

    EXPECT_EQ(r.attribution, weaknet::L2LoopAttribution::LOOP_SUSPECTED);
    ASSERT_EQ(r.issues.size(), 1u);
    EXPECT_EQ(r.issues[0], "MULTI_PORT_RETURN");
    EXPECT_NE(r.explanation.find("eth0->eth1"), std::string::npos)
        << "解释里必须带上具体端口对，否则运维无从下手";
}

TEST(L2LoopAttributionTest, MinorityReturnIsInconclusiveNotNoLoop) {
    // 少数轮看到回流：既不能断言有环路，也**不能**说没环路——因为确实观测到了。
    // 这是四象限里最容易写错的一格。
    weaknet::L2ProbeWindow w;
    w.total_rounds = 10;
    w.degraded_rounds = 0;
    w.multi_return_rounds = 2;
    auto r = weaknet::attributeL2Loop(w);

    EXPECT_EQ(r.attribution, weaknet::L2LoopAttribution::NONE);
    EXPECT_EQ(r.reason, "insufficient_valid_rounds");
    EXPECT_TRUE(r.issues.empty()) << "拒绝归因时不得给出问题清单";
}

TEST(L2LoopAttributionTest, ExactHalfIsSuspected) {
    // 边界：4 轮里 2 轮回流 = 恰好 50%，按规则取 >=。
    weaknet::L2ProbeWindow w;
    w.total_rounds = 4;
    w.degraded_rounds = 0;
    w.multi_return_rounds = 2;
    auto r = weaknet::attributeL2Loop(w);
    EXPECT_EQ(r.attribution, weaknet::L2LoopAttribution::LOOP_SUSPECTED);
}

TEST(L2LoopAttributionTest, NoneIsNeverReportedAsHealthy) {
    // 契约级断言：NONE 的字符串形式不能长得像"好"。
    // 上游若把字符串直接透给 UI，NONE 必须读起来像"未知"。
    EXPECT_STREQ(weaknet::l2LoopAttributionToString(weaknet::L2LoopAttribution::NONE), "NONE");
    EXPECT_STREQ(weaknet::l2LoopAttributionToString(weaknet::L2LoopAttribution::NO_LOOP), "NO_LOOP");
    EXPECT_STREQ(weaknet::l2LoopAttributionToString(weaknet::L2LoopAttribution::LOOP_SUSPECTED),
                 "LOOP_SUSPECTED");
}

// ============================================================================
// 出口资格：单口必须拒绝启动，否则「不知道」会被说成「没环路」
// ============================================================================
//
// 这条规则原先是**只在引擎里**写得很好看（头文件写着「至少 2 个接口才有
// 判定资格」），但 parseIfaces() 只检查了非空。于是单口配置会被接受，
// 每轮 MULTI_RETURN 恒为 0 → attributeL2Loop() 给出 NO_LOOP ——
// 一个"确证没有环路"的结论，而跨口回流在这个配置下**根本不可能被观察到**。
//
// 这类缺陷的形态：不报错、不崩溃、日志里轮次和收发数字都正常，
// 只有结论永久错。所以判据必须搬到纯逻辑层，让无权限的 CI 也能锁死它。

TEST(L2ProbeEgressEligibilityTest, SingleIfaceIsRejectedNotSilentlyAccepted) {
    std::vector<L2ProbeTarget> one{makeTarget("eth0")};
    const std::string reason = checkL2ProbeEgressEligibility(one);
    EXPECT_FALSE(reason.empty()) << "单口必须拒绝启动";
    EXPECT_NE(reason.find("eth0"), std::string::npos)
        << "拒绝原因要写明是哪个接口，运维才能改配置";
}

TEST(L2ProbeEgressEligibilityTest, TwoIfacesAreEligible) {
    std::vector<L2ProbeTarget> two{makeTarget("eth0"), makeTarget("eth1")};
    EXPECT_EQ(checkL2ProbeEgressEligibility(two), "");
}

TEST(L2ProbeEgressEligibilityTest, SameIfaceWithMultipleVlansIsStillOneEgress) {
    // 同一接口配两个 VLAN 仍然是**一个**出口：跨口回流不可观察。
    // 判据看的是去重后的接口数，不是 target 数 —— 这条最容易写错。
    std::vector<L2ProbeTarget> same_iface{makeTarget("eth0", 100),
                                          makeTarget("eth0", 200)};
    EXPECT_FALSE(checkL2ProbeEgressEligibility(same_iface).empty())
        << "两个 target 同一个口，不得被当成两个出口";
}

TEST(L2ProbeEgressEligibilityTest, EmptyListIsRejected) {
    std::vector<L2ProbeTarget> none;
    EXPECT_FALSE(checkL2ProbeEgressEligibility(none).empty());
}

TEST(L2ProbeEgressEligibilityTest, InterfaceWithVlanDiversityAcrossTwoPortsIsEligible) {
    // 两个不同口 + 各自一个 VLAN：有资格。
    std::vector<L2ProbeTarget> mixed{makeTarget("eth0", 100),
                                     makeTarget("eth1", 200)};
    EXPECT_EQ(checkL2ProbeEgressEligibility(mixed), "");
}

// ============================================================================
// 降级轮不得计入回流分子（口径一致）
// ============================================================================
//
// L2ProbeWindow 的字段注释写着「multi_return_rounds：仅统计非降级轮」，
// 但生产装配代码曾经把所有 verdict==MULTI_RETURN 的轮都算进分子，
// 分母却剔掉了降级轮。后果：一个「回流证据全部来自不可信轮次」的现场，
// 仍会被判成 LOOP_SUSPECTED。这里是那条口径的纯逻辑表达。

TEST(L2ProbeAttributionDenominatorTest, DegradedRoundsMustNotFeedTheNumerator) {
    // 装配层若正确剔除了降级轮，就等价于这个窗口：
    // 2 个有效轮、都没看到回流 → NO_LOOP
    weaknet::L2ProbeWindow w;
    w.total_rounds = 4;
    w.degraded_rounds = 2;        // 2 个降级轮（它们观测到的"回流"不可信）
    w.multi_return_rounds = 0;    // 分子只含非降级轮 → 0
    w.egress_ifaces = {"eth0", "eth1"};

    auto r = weaknet::attributeL2Loop(w);
    EXPECT_EQ(r.attribution, weaknet::L2LoopAttribution::NO_LOOP)
        << "只有 2 个可信轮且都未见回流，应给否定结论";
}

// ============================================================================
// 实测回归：真实 AF_PACKET 收发病理（在隔离 netns + veth 上跑出来的）
// ============================================================================
//
// 这两条不是从设计推出来的，是从一次真实验证里**抓到的缺陷**反推的。
// 它们必须留在单测里，因为缺陷的后果都不会崩、不会报错，只会让功能
// 「看起来在工作、实际永远没有结论」。

TEST(L2ProbeRealWorldRegression, IpForwardMustNotDegradeTheRound) {
    // 缺陷原型：曾经把 /proc/sys/net/ipv4/ip_forward == 1 当作整轮降级。
    //
    // 为什么这是错的（两条独立理由）：
    //   1. 判据与机制不匹配。ip_forward 决定的是 **IP 包**是否被转发；
    //      探测帧是自定义 EtherType 0x88B5，IP 栈根本不解析它。
    //      用 IP 转发开关否决二层观测，是拿不相干的开关否决不相干的证据。
    //   2. 它会让功能在真实部署里永久失效。开发板作为网关 ip_forward 恒为 1，
    //      于是每轮降级 → 归因恒为 NONE → 装了等于没装。
    //
    // 这条测试锁定的是**归因层的行为**：只要窗口里所有轮次都有效
    // （degraded_rounds == 0），即使宿主 ip_forward=1 也必须能出结论。
    // 引擎侧的那段代码已删除；这里防止它换个形式回来（比如
    // "在归因函数里检查一下 ip_forward 更保险"）。
    weaknet::L2ProbeWindow w;
    w.total_rounds = 4;
    w.degraded_rounds = 0;   // 引擎没报降级 —— ip_forward 不得让它报降级
    w.multi_return_rounds = 3;
    w.return_pairs = {"eth0->eth1"};
    w.egress_ifaces = {"eth0", "eth1"};

    auto r = weaknet::attributeL2Loop(w);
    EXPECT_EQ(r.attribution, weaknet::L2LoopAttribution::LOOP_SUSPECTED)
        << "宿主 ip_forward 状态与二层观测无关，不得参与归因";
    EXPECT_EQ(r.reason, "");
}

TEST(L2ProbeRealWorldRegression, BridgeFloodingProducesGenuineMultiReturn) {
    // 实测事实（隔离 netns + veth + bridge）：
    //   不桥接 → sent=4 recv=0 cross=0  → NO_RETURN
    //   桥接后、peer 为桥未学到的单播 → sent=4 recv=4 cross=4 → 跨口回流
    //
    // 这条锁定"跨口回流就是二层通路的正证据"这一语义：
    // **不要**把桥转发当成需要排除的假阳性。桥转发正是我们要观测的东西。
    // 真正的假阳性只有一种：目标 MAC 是本机自己的口（self_egress_loop）。
    weaknet::L2ProbeWindow no_bridge;
    no_bridge.total_rounds = 4;
    no_bridge.degraded_rounds = 0;
    no_bridge.multi_return_rounds = 0;
    no_bridge.egress_ifaces = {"va", "vc"};
    EXPECT_EQ(weaknet::attributeL2Loop(no_bridge).attribution,
              weaknet::L2LoopAttribution::NO_LOOP);

    weaknet::L2ProbeWindow bridged = no_bridge;
    bridged.multi_return_rounds = 4;
    bridged.return_pairs = {"va->vc", "vc->va"};
    auto r = weaknet::attributeL2Loop(bridged);
    EXPECT_EQ(r.attribution, weaknet::L2LoopAttribution::LOOP_SUSPECTED);
    EXPECT_FALSE(r.issues.empty()) << "判为疑似环路时必须给出机器可读问题项";
}

TEST(L2ProbeRealWorldRegression, SelfEgressLoopIsTheOnlyRealFalsePositive) {
    // 目标 MAC 是本机某个口 → 帧被自己的协议栈收下，回来的路径是
    // "自己发给自己"，不是二层回流。这是**唯一**需要整轮降级的假阳性。
    // 实测：peer = va 自身 MAC → degraded=self_egress_loop，
    //       即便观测到 cross>0，归因也必须拒绝（all_rounds_degraded）。
    weaknet::L2ProbeWindow w;
    w.total_rounds = 3;
    w.degraded_rounds = 3;
    w.multi_return_rounds = 3;   // 观测到了，但不可信
    w.return_pairs = {"vc->va"};
    w.egress_ifaces = {"va", "vc"};

    auto r = weaknet::attributeL2Loop(w);
    EXPECT_EQ(r.attribution, weaknet::L2LoopAttribution::NONE)
        << "全是降级轮次时不得下任何结论";
    EXPECT_EQ(r.reason, "all_rounds_degraded");
}

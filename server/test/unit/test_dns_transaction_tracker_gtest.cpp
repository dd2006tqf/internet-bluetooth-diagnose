#include <gtest/gtest.h>
#include "assurance/dns_transaction_tracker.hpp"
#include "assurance/dns_service_evaluator.hpp"

using namespace weaknet;
using namespace std::chrono_literals;

TEST(DnsTransactionTrackerTest, ExactlyOnceAndRcodeClassification) {
    DnsTransactionTracker tracker;
    auto now = std::chrono::steady_clock::now();

    DnsCanonicalKey key1;
    key1.client_ip = 0xC0A80102;   // 192.168.1.2
    key1.resolver_ip = 0xDF050505; // 223.5.5.5
    key1.client_port = 54321;
    key1.txid = 0x1234;
    key1.qname_hash = 0xABCDEF;
    key1.qtype = 1;

    // 1. 发起 Query
    EXPECT_TRUE(tracker.onQueryCaptured(key1, FingerprintQuality::ENRICHED, now));
    EXPECT_EQ(tracker.getSnapshot(now).current_inflight, 1u);

    // 2. 收到标准 NOERROR 响应
    tracker.onResponseCaptured(key1, 0, false, false, now + 15ms);
    EXPECT_EQ(tracker.getSnapshot(now + 15ms).current_inflight, 0u);

    auto window = tracker.getWindowMetrics(120s, now + 20ms);
    EXPECT_EQ(window.queries_started, 1u);
    EXPECT_EQ(window.responses_noerror, 1u);
    EXPECT_EQ(window.knownSuccess(), 1u);
    EXPECT_EQ(window.knownFailure(), 0u);
    EXPECT_DOUBLE_EQ(window.failureRatio(), 0.0);
    ASSERT_EQ(window.latencies_ms.size(), 1u);
    EXPECT_NEAR(window.latencies_ms[0], 15.0, 0.1);
}

TEST(DnsTransactionTrackerTest, TcAndRcodeMutualExclusion) {
    DnsTransactionTracker tracker;
    auto now = std::chrono::steady_clock::now();

    DnsCanonicalKey key;
    key.client_ip = 0xC0A80102;
    key.resolver_ip = 0xDF050505;
    key.client_port = 54321;
    key.txid = 0x1234;

    tracker.onQueryCaptured(key, FingerprintQuality::ENRICHED, now);

    // 修正 #4: 当 TC=1 且 RCODE=0 时，严格归类到 RESPONSE_TRUNCATED，禁止进入 NOERROR
    tracker.onResponseCaptured(key, 0, true, false, now + 10ms);

    auto window = tracker.getWindowMetrics(120s, now + 20ms);
    EXPECT_EQ(window.responses_truncated, 1u);
    EXPECT_EQ(window.responses_noerror, 0u); // 严禁进入 NOERROR
    EXPECT_EQ(window.knownSuccess(), 0u);
    EXPECT_EQ(window.knownFailure(), 0u);    // TRUNCATED 属于 other_terminals，不进 failure
    EXPECT_EQ(window.evaluableTerminals(), 0u);
}

TEST(DnsTransactionTrackerTest, RetransmissionDeduplication) {
    DnsTransactionTracker tracker;
    auto now = std::chrono::steady_clock::now();

    DnsCanonicalKey key;
    key.client_ip = 0xC0A80102;
    key.resolver_ip = 0xDF050505;
    key.client_port = 54321;
    key.txid = 0x1234;

    // 初次发送
    EXPECT_TRUE(tracker.onQueryCaptured(key, FingerprintQuality::ENRICHED, now));
    // 1 秒后重传相同的 key
    EXPECT_TRUE(tracker.onQueryCaptured(key, FingerprintQuality::ENRICHED, now + 1000ms));
    // 在途事务数仍为 1
    EXPECT_EQ(tracker.getSnapshot(now + 1000ms).current_inflight, 1u);

    // 响应到达 (1500ms 后到达，延迟应从 first_sent_at 计算为 1500ms)
    tracker.onResponseCaptured(key, 0, false, false, now + 1500ms);
    auto window = tracker.getWindowMetrics(120s, now + 2000ms);
    EXPECT_EQ(window.responses_noerror, 1u);
    ASSERT_EQ(window.latencies_ms.size(), 1u);
    EXPECT_NEAR(window.latencies_ms[0], 1500.0, 1.0);
}

TEST(DnsTransactionTrackerTest, TimeoutAndLateResponseTombstone) {
    DnsTrackerConfig cfg;
    cfg.query_timeout = 5000ms;
    cfg.tombstone_retention = 15000ms;
    DnsTransactionTracker tracker(cfg);
    auto now = std::chrono::steady_clock::now();

    DnsCanonicalKey key;
    key.client_ip = 0xC0A80102;
    key.resolver_ip = 0xDF050505;
    key.client_port = 54321;
    key.txid = 0x1234;

    tracker.onQueryCaptured(key, FingerprintQuality::ENRICHED, now);

    // 4.9s 扫描不超时
    EXPECT_EQ(tracker.sweepTimeouts(now + 4900ms), 0u);
    EXPECT_EQ(tracker.getSnapshot(now + 4900ms).current_inflight, 1u);

    // 5.1s 扫描超时
    EXPECT_EQ(tracker.sweepTimeouts(now + 5100ms), 1u);
    EXPECT_EQ(tracker.getSnapshot(now + 5100ms).current_inflight, 0u);

    auto window = tracker.getWindowMetrics(120s, now + 5200ms);
    EXPECT_EQ(window.timeouts, 1u);
    EXPECT_EQ(window.knownFailure(), 1u);

    // 6.0s 时迟到响应到达 (在 15s 墓碑期内) -> 计入 LATE_RESPONSE，而不是 UNMATCHED
    tracker.onResponseCaptured(key, 0, false, false, now + 6000ms);
    auto window_late = tracker.getWindowMetrics(120s, now + 6100ms);
    EXPECT_EQ(window_late.late_responses, 1u);
    EXPECT_EQ(window_late.unmatched, 0u);

    // 25.0s 后墓碑过期，再次收到相同的响应 -> 计入 UNMATCHED
    tracker.sweepTimeouts(now + 25000ms); // 清理旧墓碑
    tracker.onResponseCaptured(key, 0, false, false, now + 26000ms);
    auto window_unmatched = tracker.getWindowMetrics(120s, now + 26100ms);
    EXPECT_EQ(window_unmatched.unmatched, 1u);
}

TEST(DnsTransactionTrackerTest, EpochIsolation) {
    DnsTransactionTracker tracker;
    auto now = std::chrono::steady_clock::now();

    DnsCanonicalKey key1;
    key1.client_port = 5001;
    key1.txid = 1;

    tracker.onQueryCaptured(key1, FingerprintQuality::ENRICHED, now);
    EXPECT_EQ(tracker.getSnapshot(now).current_inflight, 1u);

    // 切换网卡 / 路由更新 -> 推进 epoch
    tracker.advanceBindingEpoch(2);
    // 旧 epoch 的活跃事务被清理，不污染新 epoch
    EXPECT_EQ(tracker.getSnapshot(now).current_inflight, 0u);

    DnsCanonicalKey key2;
    key2.client_port = 5002;
    key2.txid = 2;
    tracker.onQueryCaptured(key2, FingerprintQuality::ENRICHED, now + 10ms);
    tracker.onResponseCaptured(key2, 0, false, false, now + 20ms);

    auto window = tracker.getWindowMetrics(120s, now + 30ms);
    EXPECT_EQ(window.binding_epoch, 2u);
    EXPECT_EQ(window.responses_noerror, 1u);
    EXPECT_EQ(window.queries_started, 1u);
}

TEST(DnsServiceEvaluatorTest, MissingnessThreeStates) {
    DnsMetricWindow w;
    std::vector<DnsTransactionRecord> recent;

    // 1. 无查询、无在途 -> UNKNOWN (no_dns_observations)
    w.queries_started = 0;
    w.current_inflight = 0;
    auto res1 = DnsServiceEvaluator::evaluate(w, recent);
    EXPECT_EQ(res1.state, HealthState::UNKNOWN);
    EXPECT_EQ(res1.coverage, Coverage::NONE);
    EXPECT_EQ(res1.reason, "no_dns_observations");

    // 2. 有在途但无完成 -> UNKNOWN (awaiting_inflight)
    w.queries_started = 1;
    w.current_inflight = 1;
    auto res2 = DnsServiceEvaluator::evaluate(w, recent);
    EXPECT_EQ(res2.state, HealthState::UNKNOWN);
    EXPECT_EQ(res2.coverage, Coverage::PARTIAL);
    EXPECT_EQ(res2.reason, "awaiting_inflight");

    // 3. 有终态但不足 5 个门禁 -> UNKNOWN (insufficient_terminal_observations)
    w.responses_noerror = 3;
    w.current_inflight = 0;
    auto res3 = DnsServiceEvaluator::evaluate(w, recent);
    EXPECT_EQ(res3.state, HealthState::UNKNOWN);
    EXPECT_EQ(res3.coverage, Coverage::PARTIAL);
    EXPECT_EQ(res3.reason, "insufficient_terminal_observations");
}

TEST(DnsServiceEvaluatorTest, BurstConsecutiveTimeoutBypassSR9) {
    DnsEvaluatorConfig cfg;
    cfg.critical_timeout_count = 3;
    cfg.critical_window = 15000ms;
    auto now = std::chrono::steady_clock::now();

    DnsMetricWindow w;
    w.binding_epoch = 1;
    w.evaluation_cutoff = now;
    w.queries_started = 3;
    w.timeouts = 3;

    // Case A: 连续 3 次超时 -> 立即 BAD
    std::vector<DnsTransactionRecord> terminalsA;
    for (int i = 0; i < 3; ++i) {
        DnsTransactionRecord r;
        r.binding_epoch = 1;
        r.state = DnsTransactionState::TIMEOUT_EXPIRED;
        r.completed_at = now - std::chrono::milliseconds((3 - i) * 1000);
        terminalsA.push_back(r);
    }
    bool bypassA = false;
    auto resA = DnsServiceEvaluator::evaluate(w, terminalsA, cfg, &bypassA);
    EXPECT_EQ(resA.state, HealthState::BAD);
    EXPECT_TRUE(bypassA);
    EXPECT_EQ(resA.reason, "critical_burst_timeouts");

    // Case B: 超时之间有成功插队 (TIMEOUT, SUCCESS, TIMEOUT, SUCCESS, TIMEOUT) -> 打断连续性，不触发突发单杀
    std::vector<DnsTransactionRecord> terminalsB;
    for (int i = 0; i < 5; ++i) {
        DnsTransactionRecord r;
        r.binding_epoch = 1;
        r.state = (i % 2 == 0) ? DnsTransactionState::TIMEOUT_EXPIRED : DnsTransactionState::NOERROR;
        r.completed_at = now - std::chrono::milliseconds((5 - i) * 1000);
        terminalsB.push_back(r);
    }
    bool bypassB = false;
    auto resB = DnsServiceEvaluator::evaluate(w, terminalsB, cfg, &bypassB);
    EXPECT_FALSE(bypassB);
}

TEST(DnsServiceEvaluatorTest, FailureRateAndMedianLatency) {
    DnsEvaluatorConfig cfg;
    auto now = std::chrono::steady_clock::now();

    DnsMetricWindow w;
    w.binding_epoch = 1;
    w.evaluation_cutoff = now;
    w.queries_started = 10;
    w.responses_noerror = 8;
    w.responses_servfail = 2; // 20% 失败率
    w.latencies_ms = {20.0, 25.0, 30.0, 35.0, 40.0, 45.0, 50.0, 55.0};

    std::vector<DnsTransactionRecord> recent;
    auto res = DnsServiceEvaluator::evaluate(w, recent, cfg);
    EXPECT_EQ(res.state, HealthState::BAD);
    EXPECT_EQ(res.reason, "high_failure_rate");
}

// ===========================================================================
// Tracker 对抗测试：攻击极端场景，验证"不误配、不伪造、不 double-complete"
// ===========================================================================

namespace {

DnsCanonicalKey makeKey(uint32_t client_ip, uint32_t resolver_ip,
                        uint16_t client_port, uint16_t txid) {
    DnsCanonicalKey k;
    k.family = AddressFamily::IPv4;
    k.client_ip = client_ip;
    k.resolver_ip = resolver_ip;
    k.client_port = client_port;
    k.txid = txid;
    return k;
}

}  // namespace

TEST(DnsTrackerAdversarialTest, SameTxIdDifferentEndpointStaysDistinct) {
    DnsTransactionTracker tracker;
    auto now = std::chrono::steady_clock::now();

    // 两个并发事务，TxID 相同但 endpoint 不同
    auto keyA = makeKey(0x0A000001, 0x0A000053, 40001, 0x1234);
    auto keyB = makeKey(0x0A000001, 0x0A000054, 40002, 0x1234);

    EXPECT_TRUE(tracker.onQueryCaptured(keyA, FingerprintQuality::ENRICHED, now));
    EXPECT_TRUE(tracker.onQueryCaptured(keyB, FingerprintQuality::ENRICHED, now));
    EXPECT_EQ(tracker.getSnapshot(now).current_inflight, 2u);

    // 乱序响应：先回 B 再回 A
    tracker.onResponseCaptured(keyB, 0, false, false, now + 20ms);
    tracker.onResponseCaptured(keyA, 3, false, false, now + 30ms);  // NXDOMAIN

    auto window = tracker.getWindowMetrics(120s, now + 40ms);
    EXPECT_EQ(window.responses_noerror, 1u);
    EXPECT_EQ(window.responses_nxdomain, 1u);
    EXPECT_EQ(window.unmatched, 0u);
    EXPECT_EQ(window.tracking_ambiguous, 0u);
}

TEST(DnsTrackerAdversarialTest, AmbiguousPartialMatchIsNeverGuessed) {
    DnsTransactionTracker tracker;
    auto now = std::chrono::steady_clock::now();

    // 两个 PARTIAL 事务，只有 TxID 可用且相同 -> 无法区分
    auto keyA = makeKey(0, 0, 0, 0x7777);
    auto keyB = makeKey(0, 0, 0, 0x7777);
    // 用不同 qname_hash 让它们成为两个不同的 active 记录
    keyA.qname_hash = 0xAAAA;
    keyB.qname_hash = 0xBBBB;

    tracker.onQueryCaptured(keyA, FingerprintQuality::PARTIAL, now);
    tracker.onQueryCaptured(keyB, FingerprintQuality::PARTIAL, now);

    // 响应只有 TxID，无 endpoint -> 候选 2 个，必须判歧义而非猜一个
    auto respKey = makeKey(0, 0, 0, 0x7777);
    tracker.onResponseCaptured(respKey, 0, false, false, now + 10ms);

    auto window = tracker.getWindowMetrics(120s, now + 20ms);
    EXPECT_EQ(window.tracking_ambiguous, 1u);
    // 不得把歧义响应错配成终态
    EXPECT_EQ(window.responses_noerror, 0u);
    EXPECT_EQ(window.evaluableTerminals(), 0u);
    // 两个事务仍在途
    EXPECT_EQ(tracker.getSnapshot(now + 20ms).current_inflight, 2u);
}

TEST(DnsTrackerAdversarialTest, DuplicateResponseDoesNotDoubleComplete) {
    DnsTransactionTracker tracker;
    auto now = std::chrono::steady_clock::now();

    auto key = makeKey(0x0A000001, 0x0A000053, 40001, 0x2222);
    tracker.onQueryCaptured(key, FingerprintQuality::ENRICHED, now);
    tracker.onResponseCaptured(key, 0, false, false, now + 10ms);

    // 同一响应的重复副本：不得二次终态化
    tracker.onResponseCaptured(key, 0, false, false, now + 11ms);

    auto window = tracker.getWindowMetrics(120s, now + 20ms);
    EXPECT_EQ(window.responses_noerror, 1u);   // 只算一次
    EXPECT_EQ(window.unmatched, 1u);           // 第二次是未匹配
    EXPECT_EQ(window.evaluableTerminals(), 1u);
}

TEST(DnsTrackerAdversarialTest, UnmatchedResponseNeverCreatesTransaction) {
    DnsTransactionTracker tracker;
    auto now = std::chrono::steady_clock::now();

    // 从未捕获到任何 query，直接来一个 response
    auto key = makeKey(0x0A000001, 0x0A000053, 40001, 0x3333);
    tracker.onResponseCaptured(key, 0, false, false, now);

    auto window = tracker.getWindowMetrics(120s, now + 10ms);
    EXPECT_EQ(window.unmatched, 1u);
    // 核心不变式：unmatched response 绝不反向创建一个 DNS 事务
    EXPECT_EQ(window.queries_started, 0u);
    EXPECT_EQ(window.evaluableTerminals(), 0u);
    EXPECT_EQ(tracker.getSnapshot(now + 10ms).current_inflight, 0u);
}

TEST(DnsTrackerAdversarialTest, CapacityOverflowIsCountedNotSilent) {
    DnsTrackerConfig cfg;
    cfg.capacity = 4;
    DnsTransactionTracker tracker(cfg);
    auto now = std::chrono::steady_clock::now();

    for (uint16_t i = 0; i < 4; ++i) {
        EXPECT_TRUE(tracker.onQueryCaptured(makeKey(1, 2, 100 + i, i), FingerprintQuality::ENRICHED, now));
    }
    // 超出容量：必须拒绝并计数（IR-1），不能静默丢弃
    EXPECT_FALSE(tracker.onQueryCaptured(makeKey(1, 2, 200, 99), FingerprintQuality::ENRICHED, now));

    auto window = tracker.getWindowMetrics(120s, now + 10ms);
    EXPECT_EQ(window.tracker_insert_failures, 1u);
    EXPECT_GT(window.insertFailureRatio(), 0.0);
}

TEST(DnsTrackerAdversarialTest, EpochAdvanceInvalidatesInflightAcrossEpochs) {
    DnsTransactionTracker tracker;
    auto now = std::chrono::steady_clock::now();

    auto oldKey = makeKey(1, 2, 30001, 0x4444);
    tracker.onQueryCaptured(oldKey, FingerprintQuality::ENRICHED, now);

    // 路由/resolver 变更推进 epoch
    tracker.advanceBindingEpoch(2);
    EXPECT_EQ(tracker.getSnapshot(now).current_inflight, 0u);

    // 旧 epoch 的响应不得在新 epoch 中被匹配
    tracker.onResponseCaptured(oldKey, 0, false, false, now + 10ms);
    auto window = tracker.getWindowMetrics(120s, now + 20ms);
    EXPECT_EQ(window.binding_epoch, 2u);
    EXPECT_EQ(window.responses_noerror, 0u);
    EXPECT_EQ(window.unmatched, 1u);
}

TEST(DnsTrackerAdversarialTest, TerminalCountsAreWindowScopedObserverCountsAreDeltas) {
    DnsTransactionTracker tracker;
    auto now = std::chrono::steady_clock::now();

    tracker.onQueryCaptured(makeKey(1, 2, 40001, 1), FingerprintQuality::ENRICHED, now);
    tracker.onResponseCaptured(makeKey(1, 2, 40001, 1), 0, false, false, now + 5ms);

    // 终态计数按 120s 时间窗统计：同一事务在后续窗口中仍属于该窗口
    auto first = tracker.getWindowMetrics(120s, now + 10ms);
    EXPECT_EQ(first.responses_noerror, 1u);
    auto second = tracker.getWindowMetrics(120s, now + 20ms);
    EXPECT_EQ(second.responses_noerror, 1u);

    // 观测质量计数按窗口增量：第二个窗口无新事件 -> 必须为 0，
    // 否则历史大样本会稀释当前观测质量（当前已坏却测不出）。
    tracker.recordTransportDelta(100, 10, 90, 10);
    auto with_loss = tracker.getWindowMetrics(120s, now + 30ms);
    EXPECT_EQ(with_loss.capture_attempts, 100u);
    EXPECT_EQ(with_loss.capture_emit_failures, 10u);
    EXPECT_EQ(with_loss.perf_lost_events, 10u);

    // 第三个窗口无新增量 -> 观测计数归零，而非重复累计
    auto next = tracker.getWindowMetrics(120s, now + 40ms);
    EXPECT_EQ(next.capture_attempts, 0u);
    EXPECT_EQ(next.capture_emit_failures, 0u);
    EXPECT_EQ(next.perf_lost_events, 0u);
}

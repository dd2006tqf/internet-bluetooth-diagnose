/**
 * @file test_site_incident_gtest.cpp
 * @brief 区域级异常事件（SiteIncident）与关联器的验收测试
 *
 * 覆盖 8 项领域不变式（每一项都对应一个真实会误报/漏报的失效模式）：
 *
 *   1. 单设备异常不触发区域事故（"一台设备坏了" vs "这一片出了问题"）
 *   2. 多设备同时异常达阈触发，且生命周期 OPEN → ONGOING → RESOLVED
 *   3. 关联窗口内继续吸收新证据，绝不新开第二起事故
 *   4. 正常断开（用户主动/对端断开）不计入事故证据
 *   5. 链路劣化（LinkDegraded）算合格异常
 *   6. 同一 event_id 幂等：重复投递不灌水受影响设备数
 *   7. 静态分母是错的：不活跃设备不得稀释受影响比例
 *   8. 重启回放恢复活跃 incident 且不产生重复记录 + 证据回链可查
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <unistd.h>

#include "database_manager.hpp"
#include "site_incident.hpp"
#include "site_incident_correlator.hpp"
#include "wireless_event.hpp"
#include "wireless_event_store.hpp"

using namespace weaknet_dbus;

namespace {

constexpr uint64_t kSecond = 1000;

WirelessDeviceEvent makeEvent(const std::string& addr, DeviceEventType type,
                              DisconnectReason reason, uint64_t ts_ms,
                              const std::string& event_id) {
    WirelessDeviceEvent ev;
    ev.event_id = event_id;
    ev.site_id = "site-test";
    ev.gateway_id = "gw-test";
    ev.hci_index = 0;
    ev.protocol = WirelessProtocol::Bluetooth;
    ev.device_address = addr;
    ev.address_type = BtAddressType::LeRandom;
    ev.event_type = type;
    ev.timestamp_ms = ts_ms;
    ev.raw_reason_code = 0x08;
    ev.reason = reason;
    return ev;
}

/// 一次非计划断连（连接超时）——合格异常
WirelessDeviceEvent disconnectEvent(const std::string& addr, uint64_t ts_ms,
                                    const std::string& event_id) {
    return makeEvent(addr, DeviceEventType::LinkDisconnected,
                     DisconnectReason::ConnectionTimeout, ts_ms, event_id);
}

size_t countOccurrences(const std::string& haystack, const std::string& needle) {
    size_t count = 0;
    for (size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

}  // namespace

// ============================================================================
// 纯内存态：合格异常判定与阈值
// ============================================================================

TEST(QualifyingAnomalyPolicyTest, PlannedTerminationIsNotAnomaly) {
    QualifyingAnomalyPolicy policy;

    // 用户主动断开/对端设备断开属于计划内行为。计入会让"每天下班关机"
    // 变成一起区域无线故障——这是本测试要钉死的失效模式。
    EXPECT_FALSE(policy.qualify(makeEvent("AA:BB:CC:DD:EE:01",
                                          DeviceEventType::LinkDisconnected,
                                          DisconnectReason::RemoteUserTerminated, 1000, "e1")));
    EXPECT_FALSE(policy.qualify(makeEvent("AA:BB:CC:DD:EE:01",
                                          DeviceEventType::LinkDisconnected,
                                          DisconnectReason::LocalHostTerminated, 1000, "e2")));

    // 未知原因**算**合格异常：语义是"已确认断开但原因不在已知分类内"。
    // 把它排除会让故障静默消失（Unknown 不能成为信息黑洞）。
    EXPECT_TRUE(policy.qualify(makeEvent("AA:BB:CC:DD:EE:01",
                                         DeviceEventType::LinkDisconnected,
                                         DisconnectReason::Unknown, 1000, "e3")));
}

TEST(QualifyingAnomalyPolicyTest, OnlyAnomalousEventTypesQualify) {
    QualifyingAnomalyPolicy policy;

    EXPECT_TRUE(policy.qualify(makeEvent("AA:BB:CC:DD:EE:01",
                                         DeviceEventType::LinkDegraded,
                                         DisconnectReason::Unknown, 1000, "e1")));
    EXPECT_FALSE(policy.qualify(makeEvent("AA:BB:CC:DD:EE:01",
                                          DeviceEventType::DeviceAppeared,
                                          DisconnectReason::Unknown, 1000, "e2")));
    EXPECT_FALSE(policy.qualify(makeEvent("AA:BB:CC:DD:EE:01",
                                          DeviceEventType::DeviceLost,
                                          DisconnectReason::Unknown, 1000, "e3")));
    // 恢复事件是故障结束的信号，绝不能被当成事故证据
    EXPECT_FALSE(policy.qualify(makeEvent("AA:BB:CC:DD:EE:01",
                                          DeviceEventType::LinkRecovered,
                                          DisconnectReason::Unknown, 1000, "e4")));
    EXPECT_FALSE(policy.qualify(makeEvent("AA:BB:CC:DD:EE:01",
                                          DeviceEventType::LinkConnected,
                                          DisconnectReason::Unknown, 1000, "e5")));
}

TEST(SiteIncidentCorrelatorTest, SingleDeviceAnomalyDoesNotOpenIncident) {
    SiteIncidentCorrelator correlator(nullptr);

    // 一台设备异常：比例是 1/1 = 100%（远超 30%），但绝对门槛（≥2 台）不满足。
    // 这正是"一台设备坏了"与"这一片无线环境出了问题"的分界线——
    // 只按比例判定会让单设备抖动直接升级成区域事故。
    auto result = correlator.observe(disconnectEvent("AA:BB:CC:DD:EE:01", 10 * kSecond, "e1"));
    EXPECT_FALSE(result.has_value());
    EXPECT_TRUE(correlator.activeIncidents().empty());
    EXPECT_EQ(correlator.totalIncidents(), 0u);
}

TEST(SiteIncidentCorrelatorTest, MultipleDevicesOpenIncidentAndProgressLifecycle) {
    SiteIncidentCorrelator correlator(nullptr);

    auto first = correlator.observe(disconnectEvent("AA:BB:CC:DD:EE:01", 10 * kSecond, "e1"));
    EXPECT_FALSE(first.has_value());  // 第一台还不够

    auto second = correlator.observe(disconnectEvent("AA:BB:CC:DD:EE:02", 20 * kSecond, "e2"));
    ASSERT_TRUE(second.has_value());  // 第二台达阈：开事故
    EXPECT_EQ(second->state, IncidentState::Open);
    EXPECT_EQ(second->affected_devices, 2u);
    EXPECT_EQ(second->started_at_ms, 10 * kSecond);   // 从最早那条异常开始
    EXPECT_EQ(second->last_event_ms, 20 * kSecond);
    EXPECT_FALSE(second->resolved_at_ms.has_value());
    // 采集与关联层不做诊断
    EXPECT_FALSE(second->suspected_cause.has_value());
    EXPECT_EQ(correlator.totalIncidents(), 1u);

    // 窗口内第三条证据被吸收（不新开），状态推进为 ONGOING
    auto third = correlator.observe(disconnectEvent("AA:BB:CC:DD:EE:03", 30 * kSecond, "e3"));
    ASSERT_TRUE(third.has_value());
    EXPECT_EQ(third->state, IncidentState::Ongoing);
    EXPECT_EQ(third->affected_devices, 3u);
    EXPECT_EQ(correlator.totalIncidents(), 1u);

    // 静默期满 → RESOLVED，结案时刻 = 最后一条证据 + 静默窗口
    const uint64_t resolved_at = 30 * kSecond + 60 * kSecond;
    auto resolved = correlator.tick(resolved_at + 1);
    ASSERT_EQ(resolved.size(), 1u);
    EXPECT_EQ(resolved[0].state, IncidentState::Resolved);
    ASSERT_TRUE(resolved[0].resolved_at_ms.has_value());
    EXPECT_EQ(*resolved[0].resolved_at_ms, resolved_at);
    EXPECT_TRUE(correlator.activeIncidents().empty());
}

TEST(SiteIncidentCorrelatorTest, LinkDegradedCountsAsAnomaly) {
    SiteIncidentCorrelator correlator(nullptr);

    correlator.observe(makeEvent("AA:BB:CC:DD:EE:01", DeviceEventType::LinkDegraded,
                                 DisconnectReason::Unknown, 10 * kSecond, "d1"));
    auto second = correlator.observe(makeEvent("AA:BB:CC:DD:EE:02", DeviceEventType::LinkDegraded,
                                               DisconnectReason::Unknown, 20 * kSecond, "d2"));

    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->affected_devices, 2u);
}

TEST(SiteIncidentCorrelatorTest, DuplicateEventIdIsIdempotent) {
    SiteIncidentCorrelator correlator(nullptr);

    correlator.observe(disconnectEvent("AA:BB:CC:DD:EE:01", 10 * kSecond, "e1"));
    // 同一 event_id 重复投递（重试/回放场景）：不得重复计数。
    // 若重复计数，受影响设备数会被灌水到门槛之上，凭空造出一起假事故。
    auto dup = correlator.observe(disconnectEvent("AA:BB:CC:DD:EE:01", 10 * kSecond, "e1"));
    EXPECT_FALSE(dup.has_value());
    EXPECT_TRUE(correlator.activeIncidents().empty());

    // 换一个真实的新事件才允许开事故
    auto second = correlator.observe(disconnectEvent("AA:BB:CC:DD:EE:02", 20 * kSecond, "e2"));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->affected_devices, 2u);
}

TEST(SiteIncidentCorrelatorTest, PlannedDisconnectDoesNotContributeEvidenceButCountsAsActive) {
    SiteIncidentCorrelator correlator(nullptr);

    // 三台设备同时"下班关机"：全部是计划内断开 → 绝不开事故
    correlator.observe(makeEvent("AA:BB:CC:DD:EE:01", DeviceEventType::LinkDisconnected,
                                 DisconnectReason::RemoteUserTerminated, 10 * kSecond, "p1"));
    correlator.observe(makeEvent("AA:BB:CC:DD:EE:02", DeviceEventType::LinkDisconnected,
                                 DisconnectReason::LocalHostTerminated, 11 * kSecond, "p2"));
    correlator.observe(makeEvent("AA:BB:CC:DD:EE:03", DeviceEventType::LinkDisconnected,
                                 DisconnectReason::RemoteUserTerminated, 12 * kSecond, "p3"));
    EXPECT_TRUE(correlator.activeIncidents().empty());

    // 但它们**计入**活跃设备分母：它们此刻确实在现场被观测到。
    // 于是两台真实异常设备只占 2/5 = 40% —— 仍达标（>30%），照常开事故。
    auto real = correlator.observe(disconnectEvent("AA:BB:CC:DD:EE:04", 20 * kSecond, "r1"));
    EXPECT_FALSE(real.has_value());
    auto opened = correlator.observe(disconnectEvent("AA:BB:CC:DD:EE:05", 21 * kSecond, "r2"));
    ASSERT_TRUE(opened.has_value());
    EXPECT_EQ(opened->affected_devices, 2u);
}

TEST(SiteIncidentCorrelatorTest, DenominatorIsActiveDevicesNotKnownDevices) {
    SiteIncidentCorrelator correlator(nullptr);

    // 模拟"现场有 20 台历史设备，但此刻只有 2 台在活跃"：
    // 只有出现在活跃记忆里的设备才计入分母。
    //
    // 若分母取全库设备数（工业现场会长期留存设备画像），
    // 2/20 = 10% < 30%，关联器将**永久**不再触发且没有任何报错——
    // 这是最容易在实现时退化成静态分母的地方。
    for (int i = 0; i < 20; ++i) {
        // 每台历史设备在很久以前被观测过一次（远超活跃记忆窗口）
        char addr[32];
        snprintf(addr, sizeof(addr), "AA:BB:CC:DD:%02X:00", i);
        correlator.observe(disconnectEvent(addr, 10 * kSecond, "old" + std::to_string(i)));
    }

    // 时间推进到远超活跃记忆窗口之后：旧设备全部到期
    const uint64_t now = 1000 * kSecond;
    correlator.tick(now);

    // 此刻只有两台活跃设备出现异常 → 分母 = 2，2/2 = 100% 达标
    correlator.observe(disconnectEvent("11:22:33:44:55:01", now, "new1"));
    auto opened = correlator.observe(disconnectEvent("11:22:33:44:55:02", now, "new2"));
    ASSERT_TRUE(opened.has_value());
    EXPECT_EQ(opened->affected_devices, 2u);
}

TEST(SiteIncidentCorrelatorTest, RatioGateBlocksWideButSparseFailures) {
    SiteIncidentCorrelator correlator(nullptr);

    // 20 台活跃设备中只有 2 台异常：2/20 = 10% < 30% → 不成事故。
    // 这就是"一台设备坏了"与"这一片无线环境出了问题"的比例判据：
    // 大面积现场里零星两台掉线是常态，不是区域故障。
    for (int i = 0; i < 20; ++i) {
        char addr[32];
        snprintf(addr, sizeof(addr), "AA:BB:CC:EE:%02X:00", i);
        correlator.observe(makeEvent(addr, DeviceEventType::DeviceAppeared,
                                     DisconnectReason::Unknown, 10 * kSecond,
                                     "app" + std::to_string(i)));
    }
    correlator.observe(disconnectEvent("AA:BB:CC:EE:00:00", 11 * kSecond, "x1"));
    auto result = correlator.observe(disconnectEvent("AA:BB:CC:EE:01:00", 12 * kSecond, "x2"));
    EXPECT_FALSE(result.has_value());
}

// ============================================================================
// 持久化：upsert 幂等、NULL 语义、证据回链、重启回放
// ============================================================================

class SiteIncidentPersistenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* testName = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        dbPath_ = "/tmp/weaknet_site_incident_" + std::to_string(getpid()) + "_" +
                  std::string(testName) + ".db";
        std::remove(dbPath_.c_str());
        db_ = std::make_unique<DatabaseManager>(dbPath_);
        ASSERT_TRUE(db_->isOpen());
    }

    void TearDown() override {
        db_.reset();
        std::remove(dbPath_.c_str());
    }

    /// 把一条事件写进 device_events（模拟已落库的设备层事实）
    void persistEvent(const WirelessDeviceEvent& ev) {
        ASSERT_TRUE(db_->insertDeviceEvent(
            ev.event_id, static_cast<int64_t>(ev.timestamp_ms), ev.site_id, ev.gateway_id,
            toString(ev.protocol), ev.device_address, toString(ev.address_type), ev.hci_index,
            toString(ev.event_type), ev.rssi_at_event_dbm, ev.raw_reason_code,
            toString(ev.reason), toString(ev.source), ev.source_detail, ev.suspected_cause,
            ev.details_json));
    }

    std::string dbPath_;
    std::unique_ptr<DatabaseManager> db_;
};

TEST_F(SiteIncidentPersistenceTest, UpsertIsIdempotentOnIncidentId) {
    // 同一 incident_id 反复写回（每次状态推进都写一次）只应留下一行
    ASSERT_TRUE(db_->upsertSiteIncident("sitinc_site-test_1000", "site-test", "gw-test",
                                        1000, 2000, std::nullopt, 2, "OPEN", std::nullopt));
    ASSERT_TRUE(db_->upsertSiteIncident("sitinc_site-test_1000", "site-test", "gw-test",
                                        1000, 3000, std::nullopt, 3, "ONGOING", std::nullopt));
    ASSERT_TRUE(db_->upsertSiteIncident("sitinc_site-test_1000", "site-test", "gw-test",
                                        1000, 3000, 63000, 3, "RESOLVED", std::nullopt));

    const std::string json = db_->querySiteIncidents("", 0, 0, 100, true);
    EXPECT_EQ(countOccurrences(json, "\"incident_id\":\""), 1u);
    // 最终状态与结案时刻都已更新
    EXPECT_NE(json.find("\"state\":\"RESOLVED\""), std::string::npos);
    EXPECT_NE(json.find("\"resolved_at_ms\":63000"), std::string::npos);
    // suspected_cause 恒为 null（关联层不做根因推断）
    EXPECT_NE(json.find("\"suspected_cause\":null"), std::string::npos);
}

TEST_F(SiteIncidentPersistenceTest, ActiveIncidentSerializesResolvedAtAsNull) {
    ASSERT_TRUE(db_->upsertSiteIncident("sitinc_a", "site-test", "gw-test",
                                        1000, 2000, std::nullopt, 2, "OPEN", std::nullopt));
    const std::string json = db_->querySiteIncidents("", 0, 0, 100, false);
    // 未结案必须是 null，而不是 0——0 会造出"1970 年结案"这种假事实
    EXPECT_NE(json.find("\"resolved_at_ms\":null"), std::string::npos);
}

TEST_F(SiteIncidentPersistenceTest, StateFilterAndLatestResolvedFloor) {
    ASSERT_TRUE(db_->upsertSiteIncident("sitinc_open", "site-test", "gw-test",
                                        1000, 2000, std::nullopt, 2, "OPEN", std::nullopt));
    ASSERT_TRUE(db_->upsertSiteIncident("sitinc_done", "site-test", "gw-test",
                                        3000, 4000, 64000, 3, "RESOLVED", std::nullopt));

    EXPECT_EQ(countOccurrences(db_->querySiteIncidents("OPEN", 0, 0, 100, false),
                               "\"incident_id\":\""), 1u);
    EXPECT_EQ(countOccurrences(db_->querySiteIncidents("RESOLVED", 0, 0, 100, false),
                               "\"incident_id\":\""), 1u);

    // 回放下限只认已结案的行：活跃 incident 不构成下限，
    // 否则"上一起还没结案"会让回放完全跳过证据。
    EXPECT_EQ(db_->queryLatestIncidentResolvedAt("site-test"), 64000);
    EXPECT_EQ(db_->queryLatestIncidentResolvedAt("site-other"), 0);
}

TEST_F(SiteIncidentPersistenceTest, EvidenceBacklinkIsQueryableAndIdempotent) {
    const auto ev1 = disconnectEvent("AA:BB:CC:DD:EE:01", 10 * kSecond, "e1");
    const auto ev2 = disconnectEvent("AA:BB:CC:DD:EE:02", 11 * kSecond, "e2");
    persistEvent(ev1);
    persistEvent(ev2);

    ASSERT_TRUE(db_->insertSiteIncidentEvent("sitinc_x", "e1"));
    ASSERT_TRUE(db_->insertSiteIncidentEvent("sitinc_x", "e2"));
    // 重复回链是幂等无操作（复合主键 + INSERT OR IGNORE）
    ASSERT_TRUE(db_->insertSiteIncidentEvent("sitinc_x", "e1"));

    ASSERT_TRUE(db_->upsertSiteIncident("sitinc_x", "site-test", "gw-test",
                                        10 * kSecond, 11 * kSecond, std::nullopt, 2,
                                        "OPEN", std::nullopt));
    const std::string json = db_->querySiteIncidents("", 0, 0, 100, true);
    // 受影响设备清单由回链表 JOIN device_events 派生，每台设备只出现一次
    EXPECT_NE(json.find("AA:BB:CC:DD:EE:01"), std::string::npos);
    EXPECT_NE(json.find("AA:BB:CC:DD:EE:02"), std::string::npos);
    EXPECT_EQ(countOccurrences(json, "AA:BB:CC:DD:EE:01"), 1u);
}

TEST_F(SiteIncidentPersistenceTest, ReplayRestoresActiveIncidentWithoutDuplicating) {
    // 场景：事故在服务重启前处于 ONGOING，重启后必须恢复而不是丢失，
    // 也绝不能在库里多出一条记录（那意味着用户看到两起事故）。
    const uint64_t window_start = 1000 * kSecond;
    const auto ev1 = disconnectEvent("AA:BB:CC:DD:EE:01", window_start, "e1");
    const auto ev2 = disconnectEvent("AA:BB:CC:DD:EE:02", window_start + kSecond, "e2");
    persistEvent(ev1);
    persistEvent(ev2);

    SiteIncidentConfig cfg;
    cfg.site_id = "site-test";

    // 重启前：关联器产出并落库
    {
        SiteIncidentCorrelator before(db_.get(), cfg);
        before.observe(ev1);
        ASSERT_TRUE(before.observe(ev2).has_value());
        EXPECT_EQ(before.totalIncidents(), 1u);
    }

    // 重启后：用同一份 device_events 回放，应恢复出同一份 incident
    SiteIncidentCorrelator after(db_.get(), cfg);
    const size_t active = after.recoverFromStore(window_start + 2 * kSecond);
    EXPECT_EQ(active, 1u);

    const auto recovered = after.activeIncidents();
    ASSERT_EQ(recovered.size(), 1u);
    // 确定性 ID：回放推导出的 incident_id 与重启前完全一致（否则 UPSERT 会插新行）
    EXPECT_EQ(recovered[0].incident_id, "sitinc_site_test_" + std::to_string(window_start));
    EXPECT_EQ(recovered[0].affected_devices, 2u);

    // 库里仍然只有一行
    const std::string json = db_->querySiteIncidents("", 0, 0, 100, true);
    EXPECT_EQ(countOccurrences(json, "\"incident_id\":\""), 1u);
    // 并且恢复出的活跃 incident 真的可以下钻到设备清单
    EXPECT_NE(json.find("AA:BB:CC:DD:EE:01"), std::string::npos);
    EXPECT_NE(json.find("AA:BB:CC:DD:EE:02"), std::string::npos);
}

TEST_F(SiteIncidentPersistenceTest, ReplayDoesNotResurrectResolvedIncident) {
    // 结案下限闸门：已静默已久的事故不得因为重启而被拉回活跃态。
    const uint64_t old_ms = 1000 * kSecond;
    persistEvent(disconnectEvent("AA:BB:CC:DD:EE:01", old_ms, "e1"));
    persistEvent(disconnectEvent("AA:BB:CC:DD:EE:02", old_ms + kSecond, "e2"));

    SiteIncidentConfig cfg;
    cfg.site_id = "site-test";

    // 先把这起事故走完整生命周期并结案
    {
        SiteIncidentCorrelator before(db_.get(), cfg);
        before.observe(disconnectEvent("AA:BB:CC:DD:EE:01", old_ms, "e1"));
        before.observe(disconnectEvent("AA:BB:CC:DD:EE:02", old_ms + kSecond, "e2"));
        auto resolved = before.tick(old_ms + 2 * kSecond + cfg.quiet_window_ms);
        ASSERT_EQ(resolved.size(), 1u);
    }

    // 很久之后重启：回放窗口与结案下限都排除了这两个事件
    const uint64_t much_later = old_ms + 10 * 60 * kSecond;
    SiteIncidentCorrelator after(db_.get(), cfg);
    EXPECT_EQ(after.recoverFromStore(much_later), 0u);
    EXPECT_TRUE(after.activeIncidents().empty());

    // 库里仍是那一行已结案的记录，没有被改回活跃
    const std::string json = db_->querySiteIncidents("", 0, 0, 100, false);
    EXPECT_EQ(countOccurrences(json, "\"state\":\"RESOLVED\""), 1u);
    EXPECT_EQ(countOccurrences(json, "\"state\":\"OPEN\""), 0u);
}

TEST_F(SiteIncidentPersistenceTest, StoreForwardsEventsToCorrelator) {
    // store 是关联器的唯一生产消费者：事件落库后自动进入区域关联，
    // 不需要第二个生产者，也不会漏掉任何一条合格异常。
    SiteIncidentConfig cfg;
    cfg.site_id = "site-test";
    SiteIncidentCorrelator correlator(db_.get(), cfg);

    WirelessEventStoreConfig store_cfg;
    store_cfg.site_id = "site-test";
    store_cfg.gateway_id = "gw-test";
    WirelessEventStore store(db_.get(), store_cfg);
    store.setIncidentCorrelator(&correlator);

    store.recordEvent(disconnectEvent("AA:BB:CC:DD:EE:01", 10 * kSecond, "e1"));
    store.recordEvent(disconnectEvent("AA:BB:CC:DD:EE:02", 20 * kSecond, "e2"));

    EXPECT_EQ(correlator.totalIncidents(), 1u);

    // 静默期由消费线程通过 store 推进（关联器自身不做轮询）
    EXPECT_EQ(store.tickIncidents(100 * kSecond), 1u);

    // 查询出口经 store 代理到关联器
    const std::string json = store.querySiteIncidents("RESOLVED", 0, 0, 100);
    EXPECT_NE(json.find("\"state\":\"RESOLVED\""), std::string::npos);
}

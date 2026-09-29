/**
 * @file test_wireless_event_gtest.cpp
 * @brief 无线设备事件领域模型 + 归一化器 + 存储 的单元测试
 *
 * 本测试覆盖 Phase 1 的核心不变式：
 *   - 一次真实断连，无论被多少个内核 hook 观测到，只产生一个 canonical 业务事件
 *   - 窗口外的两次真实断连不被错误合并
 *   - 原始事实（raw_reason_code、raw_evidence）永不丢失
 *   - 未采集的 RSSI 保持 NULL 语义，不用 0 冒充
 *   - 采集层不产出 suspected_cause
 *
 * 不需要内核、不需要 eBPF、不需要板子 —— 纯用户态逻辑验证。
 */

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <set>

#include "bt_event_normalizer.hpp"
#include "database_manager.hpp"
#include "wireless_event.hpp"
#include "wireless_event_store.hpp"

using namespace weaknet_dbus;

namespace {

/// 构造一条 mgmt 层观测（最权威的事实来源）
///
/// 第三个参数是 **MGMT_DEV_DISCONN_* 域**的原始码（0x00..0x05），
/// 不是 HCI error code —— 内核 mgmt_device_disconnected 的 reason 参数
/// 就是这个域，归一化器按 source 选 disconnectReasonFromMgmtCode 映射。
RawBtObservation makeMgmtObservation(uint64_t ts_ms,
                                     const std::string& mac,
                                     uint8_t mgmt_code,
                                     const std::string& gateway = "gw-test") {
    RawBtObservation obs;
    obs.timestamp_ms = ts_ms;
    obs.gateway_id = gateway;
    obs.hci_index = 0;
    obs.device_address = mac;
    obs.address_type = BtAddressType::Bredr;
    obs.event_type = DeviceEventType::LinkDisconnected;
    obs.source = EvidenceSource::KernelMgmt;
    obs.source_detail = "mgmt_device_disconnected";
    obs.raw_reason_code = mgmt_code;
    return obs;
}

/// 构造一条 hci_conn_timeout 观测（无原始 code，只带 reason_hint）
RawBtObservation makeTimeoutObservation(uint64_t ts_ms,
                                        const std::string& mac,
                                        const std::string& gateway = "gw-test") {
    RawBtObservation obs;
    obs.timestamp_ms = ts_ms;
    obs.gateway_id = gateway;
    obs.hci_index = 0;
    obs.device_address = mac;
    obs.address_type = BtAddressType::Bredr;
    obs.event_type = DeviceEventType::LinkDisconnected;
    obs.source = EvidenceSource::KernelHciTimeout;
    obs.source_detail = "hci_conn_timeout";
    obs.reason_hint = DisconnectReason::ConnectionTimeout;
    return obs;
}

/// 构造一条 hci_disconnect 观测（本机主动断开）
RawBtObservation makeHciDisconnectObservation(uint64_t ts_ms,
                                              const std::string& mac,
                                              uint8_t hci_reason) {
    RawBtObservation obs;
    obs.timestamp_ms = ts_ms;
    obs.gateway_id = "gw-test";
    obs.hci_index = 0;
    obs.device_address = mac;
    obs.address_type = BtAddressType::Bredr;
    obs.event_type = DeviceEventType::LinkDisconnected;
    obs.source = EvidenceSource::KernelHci;
    obs.source_detail = "hci_disconnect";
    obs.raw_reason_code = hci_reason;
    return obs;
}

/// 统计 details_json 中 raw_evidence 数组的元素个数（简单计数，测试专用）
size_t countEvidenceEntries(const std::string& details_json) {
    size_t count = 0;
    size_t pos = 0;
    while ((pos = details_json.find("\"ts\":", pos)) != std::string::npos) {
        ++count;
        pos += 5;
    }
    return count;
}

}  // namespace

// ============================================================================
// 枚举映射
// ============================================================================

TEST(WirelessEventEnumTest, HciReasonMappingClassifiesKnownCodes) {
    EXPECT_EQ(disconnectReasonFromHciCode(0x08), DisconnectReason::ConnectionTimeout);
    EXPECT_EQ(disconnectReasonFromHciCode(0x10), DisconnectReason::ConnectionTimeout);
    EXPECT_EQ(disconnectReasonFromHciCode(0x22), DisconnectReason::ConnectionTimeout);

    EXPECT_EQ(disconnectReasonFromHciCode(0x05), DisconnectReason::AuthenticationFailure);
    EXPECT_EQ(disconnectReasonFromHciCode(0x06), DisconnectReason::AuthenticationFailure);
    EXPECT_EQ(disconnectReasonFromHciCode(0x2F), DisconnectReason::AuthenticationFailure);

    EXPECT_EQ(disconnectReasonFromHciCode(0x13), DisconnectReason::RemoteUserTerminated);
    EXPECT_EQ(disconnectReasonFromHciCode(0x14), DisconnectReason::RemoteUserTerminated);
    EXPECT_EQ(disconnectReasonFromHciCode(0x15), DisconnectReason::RemoteUserTerminated);

    EXPECT_EQ(disconnectReasonFromHciCode(0x16), DisconnectReason::LocalHostTerminated);
}

TEST(WirelessEventEnumTest, UnknownCodeFallsBackToOtherNotUnknown) {
    // 0x99 不在分类表里：必须落到 Other（表示"确认是断连但原因未分类"），
    // 而不是 Unknown（表示"完全不知道发生了什么"）。原始 code 由调用方另行保留。
    EXPECT_EQ(disconnectReasonFromHciCode(0x99), DisconnectReason::Other);
}

TEST(WirelessEventEnumTest, StringRoundTripPreservesValue) {
    for (auto v : {DisconnectReason::Unknown, DisconnectReason::ConnectionTimeout,
                   DisconnectReason::AuthenticationFailure,
                   DisconnectReason::RemoteUserTerminated,
                   DisconnectReason::LocalHostTerminated, DisconnectReason::Other}) {
        EXPECT_EQ(disconnectReasonFromString(toString(v), DisconnectReason::Unknown), v);
    }
    for (auto v : {DeviceEventType::LinkDisconnected, DeviceEventType::DeviceAppeared,
                   DeviceEventType::LinkDegraded}) {
        EXPECT_EQ(deviceEventTypeFromString(toString(v), DeviceEventType::DeviceLost), v);
    }
    // 无法识别的字符串返回 fallback，不抛异常（历史数据兼容性）
    EXPECT_EQ(disconnectReasonFromString("NOT_A_REAL_VALUE", DisconnectReason::Other),
              DisconnectReason::Other);
}

// ============================================================================
// JSON 序列化
// ============================================================================

TEST(WirelessEventJsonTest, RssiNulloptSerializesAsNullNotZero) {
    WirelessDeviceEvent ev;
    ev.event_id = "ev-1";
    ev.device_address = "AA:BB:CC:DD:EE:FF";
    ev.event_type = DeviceEventType::LinkDisconnected;
    ev.rssi_at_event_dbm = std::nullopt;

    const std::string json = ev.toJson();
    EXPECT_NE(json.find("\"rssi_at_event_dbm\":null"), std::string::npos)
        << "未采集的 RSSI 必须序列化为 null；输出: " << json;
    EXPECT_EQ(json.find("\"rssi_at_event_dbm\":0"), std::string::npos)
        << "0 会被误读为真实读数；输出: " << json;
}

TEST(WirelessEventJsonTest, SuspectedCauseNulloptSerializesAsNull) {
    WirelessDeviceEvent ev;
    ev.event_id = "ev-1";
    ev.device_address = "AA:BB:CC:DD:EE:FF";

    const std::string json = ev.toJson();
    EXPECT_NE(json.find("\"suspected_cause\":null"), std::string::npos)
        << "采集层不产出推断原因；输出: " << json;
}

TEST(WirelessEventJsonTest, AllIdentityFieldsPresentInJson) {
    WirelessDeviceEvent ev;
    ev.event_id = "ev-42";
    ev.site_id = "site-1";
    ev.gateway_id = "gw-1";
    ev.hci_index = 0;
    ev.device_address = "AA:BB:CC:DD:EE:FF";
    ev.address_type = BtAddressType::LePublic;
    ev.event_type = DeviceEventType::LinkDisconnected;
    ev.raw_reason_code = 0x08;
    ev.reason = DisconnectReason::ConnectionTimeout;
    ev.source = EvidenceSource::KernelMgmt;
    ev.source_detail = "mgmt_device_disconnected";

    const std::string json = ev.toJson();
    // 事件必须能回答"哪台网关、哪个地址（含类型）、什么原始 reason、哪个来源"
    EXPECT_NE(json.find("\"gateway_id\":\"gw-1\""), std::string::npos);
    EXPECT_NE(json.find("\"address_type\":\"LE_PUBLIC\""), std::string::npos);
    EXPECT_NE(json.find("\"raw_reason_code\":8"), std::string::npos);
    EXPECT_NE(json.find("\"reason\":\"CONNECTION_TIMEOUT\""), std::string::npos);
    EXPECT_NE(json.find("\"source\":\"KERNEL_MGMT\""), std::string::npos);
    EXPECT_NE(json.find("\"source_detail\":\"mgmt_device_disconnected\""), std::string::npos);
}

// ============================================================================
// 归一化：核心不变式
// ============================================================================

TEST(BtEventNormalizerTest, MultipleHooksForOneDisconnectProduceSingleEvent) {
    BtEventNormalizer normalizer;

    const std::string mac = "5C:D8:25:B2:27:87";

    // 一次真实的 timeout 断连，被三个内核 hook 各自观测到
    normalizer.submit(makeTimeoutObservation(1000, mac));        // hci_conn_timeout
    normalizer.submit(makeHciDisconnectObservation(1050, mac, 0x08));  // hci_disconnect
    normalizer.submit(makeMgmtObservation(1100, mac, 0x01));     // mgmt_device_disconnected (MGMT TIMEOUT)

    const auto events = normalizer.flushExpired(5000);

    // 关键断言：**只有一个** canonical 业务事件
    ASSERT_EQ(events.size(), 1u)
        << "同一现实故障被多个 hook 观测到时，业务事件必须只有一个，否则后续计数会翻倍";

    const auto& ev = events[0];
    EXPECT_EQ(ev.event_type, DeviceEventType::LinkDisconnected);
    // 事件时间取窗口中最早的观测时刻
    EXPECT_EQ(ev.timestamp_ms, 1000u);
    // 原因由最权威的来源（mgmt 的原始 code，MGMT 域 0x01=TIMEOUT）定案
    EXPECT_EQ(ev.raw_reason_code, 0x01);
    EXPECT_EQ(ev.reason, DisconnectReason::ConnectionTimeout);
    EXPECT_EQ(ev.source, EvidenceSource::KernelMgmt);
    EXPECT_EQ(ev.source_detail, "mgmt_device_disconnected");

    // 原始证据不丢：三条观测全部保留
    EXPECT_EQ(countEvidenceEntries(ev.details_json), 3u)
        << "所有原始观测必须进入 raw_evidence；details=" << ev.details_json;
    // 且每条证据的来源可追溯
    EXPECT_NE(ev.details_json.find("hci_conn_timeout"), std::string::npos);
    EXPECT_NE(ev.details_json.find("hci_disconnect"), std::string::npos);
    EXPECT_NE(ev.details_json.find("mgmt_device_disconnected"), std::string::npos);
}

TEST(BtEventNormalizerTest, ObservationsOutsideWindowAreNotMerged) {
    BtEventNormalizer normalizer;

    const std::string mac = "5C:D8:25:B2:27:87";

    // 两次真实的断连，相隔远超合并窗口（默认 2000ms）
    normalizer.submit(makeMgmtObservation(1000, mac, 0x01));
    normalizer.submit(makeMgmtObservation(100000, mac, 0x03));

    auto events = normalizer.flushExpired(200000);

    // 关键断言：窗口外不合并 —— 两次真实事件必须产生两个 canonical 事件
    ASSERT_EQ(events.size(), 2u)
        << "相隔超出合并窗口的两次断连是两件事，不能被合并成一件";

    // 按事件时间排序后应能区分两次不同的原因
    std::sort(events.begin(), events.end(),
              [](const WirelessDeviceEvent& a, const WirelessDeviceEvent& b) {
                  return a.timestamp_ms < b.timestamp_ms;
              });
    EXPECT_EQ(events[0].reason, DisconnectReason::ConnectionTimeout);
    EXPECT_EQ(events[1].reason, DisconnectReason::RemoteUserTerminated);
}

TEST(BtEventNormalizerTest, MgmtReasonIsNotOverriddenByLaterSupplementalHint) {
    BtEventNormalizer normalizer;
    const std::string mac = "11:22:33:44:55:66";

    // mgmt 先给出权威 code（认证失败，MGMT_DEV_DISCONN_AUTH_FAILURE=0x04），
    // 随后 hci_conn_timeout 又投递一条 supplemental hint（超时）。
    // 优先级规则要求 mgmt 的定案**不被覆盖**。
    normalizer.submit(makeMgmtObservation(1000, mac, 0x04));
    normalizer.submit(makeTimeoutObservation(1200, mac));

    const auto events = normalizer.flushExpired(5000);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].reason, DisconnectReason::AuthenticationFailure)
        << "mgmt 的原始 reason 是权威事实，不能被后到的 supplemental hint 冲掉";
    EXPECT_EQ(events[0].raw_reason_code, 0x04);
    // 但两条证据都要留下
    EXPECT_EQ(countEvidenceEntries(events[0].details_json), 2u);
}

TEST(BtEventNormalizerTest, SupplementalHintUsedWhenNoMgmtObservation) {
    BtEventNormalizer normalizer;
    const std::string mac = "11:22:33:44:55:67";

    // 只有 hci_conn_timeout（没有 mgmt），reason 由 hint 定案
    normalizer.submit(makeTimeoutObservation(1000, mac));

    const auto events = normalizer.flushExpired(5000);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].reason, DisconnectReason::ConnectionTimeout);
    EXPECT_EQ(events[0].source, EvidenceSource::KernelHciTimeout);
    // 该来源没有原始 code，raw 保持 0（"未提供"），不能凭空捏造
    EXPECT_EQ(events[0].raw_reason_code, 0);
}

TEST(BtEventNormalizerTest, SameMacOnDifferentGatewaysIsNotMerged) {
    BtEventNormalizer normalizer;
    const std::string mac = "AA:BB:CC:00:00:01";

    // 同一 MAC 被两台网关各自观测到 —— 这是两台探针独立看到的事实，不能合并
    normalizer.submit(makeMgmtObservation(1000, mac, 0x08, "gw-A"));
    normalizer.submit(makeMgmtObservation(1050, mac, 0x08, "gw-B"));

    const auto events = normalizer.flushExpired(5000);
    EXPECT_EQ(events.size(), 2u)
        << "同 MAC 跨网关是两个独立事实；合并会丢失事件来源归属";
}

TEST(BtEventNormalizerTest, SameMacWithDifferentAddressTypeIsNotMerged) {
    BtEventNormalizer normalizer;
    const std::string mac = "AA:BB:CC:00:00:02";

    auto obs_a = makeMgmtObservation(1000, mac, 0x08);
    obs_a.address_type = BtAddressType::LePublic;
    auto obs_b = makeMgmtObservation(1050, mac, 0x08);
    obs_b.address_type = BtAddressType::LeRandom;

    normalizer.submit(obs_a);
    normalizer.submit(obs_b);

    EXPECT_EQ(normalizer.flushExpired(5000).size(), 2u)
        << "同地址不同地址类型属于不同身份，合并会产生错误的设备归属";
}

TEST(BtEventNormalizerTest, RssiIsTakenFromFirstObservationWithValue) {
    BtEventNormalizer normalizer;
    const std::string mac = "AA:BB:CC:00:00:03";

    auto first = makeTimeoutObservation(1000, mac);
    first.rssi_dbm = -78;
    auto second = makeMgmtObservation(1100, mac, 0x08);
    second.rssi_dbm = -85;

    normalizer.submit(first);
    normalizer.submit(second);

    const auto events = normalizer.flushExpired(5000);
    ASSERT_EQ(events.size(), 1u);
    ASSERT_TRUE(events[0].rssi_at_event_dbm.has_value());
    EXPECT_EQ(*events[0].rssi_at_event_dbm, -78)
        << "断连瞬间的链路质量比最后一条证据更有诊断价值";
}

TEST(BtEventNormalizerTest, MissingRssiStaysNulloptNotZero) {
    BtEventNormalizer normalizer;
    const std::string mac = "AA:BB:CC:00:00:04";

    normalizer.submit(makeMgmtObservation(1000, mac, 0x08));

    const auto events = normalizer.flushExpired(5000);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_FALSE(events[0].rssi_at_event_dbm.has_value())
        << "没有 RSSI 观测时必须保持 nullopt，不能用 0 冒充";
}

TEST(BtEventNormalizerTest, SuspectedCauseIsNeverPopulatedByNormalizer) {
    BtEventNormalizer normalizer;
    const std::string mac = "AA:BB:CC:00:00:05";

    normalizer.submit(makeMgmtObservation(1000, mac, 0x08));

    const auto events = normalizer.flushExpired(5000);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_FALSE(events[0].suspected_cause.has_value())
        << "采集层不承担诊断职责；suspected_cause 由 Phase 2/3 的诊断器填充";
}

TEST(BtEventNormalizerTest, EventIdsAreUniqueWithinNormalizer) {
    BtEventNormalizer normalizer;

    normalizer.submit(makeMgmtObservation(1000, "AA:00:00:00:00:01", 0x08));
    normalizer.submit(makeMgmtObservation(1000, "AA:00:00:00:00:02", 0x08));

    const auto events = normalizer.flushExpired(5000);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_NE(events[0].event_id, events[1].event_id);
}

TEST(BtEventNormalizerTest, EventIdsDifferAcrossInstances) {
    // 回归护栏：event_id 带实例随机码。
    // 早期版本是 btev_<seq>（seq 每次进程启动从 0 重来），而 device_events.event_id
    // 是 UNIQUE + INSERT OR IGNORE —— 服务重启后第一批事件会被静默丢弃
    // （每次部署都重启服务，必然踩中）。两个"模拟重启"的实例必须产出不相交的 ID 集。
    auto makeIds = [] {
        BtEventNormalizer n;
        n.submit(makeMgmtObservation(1000, "AA:00:00:00:00:11", 0x03));
        n.submit(makeMgmtObservation(1000, "AA:00:00:00:00:12", 0x03));
        auto evs = n.flushExpired(5000);
        std::set<std::string> ids;
        for (const auto& e : evs) ids.insert(e.event_id);
        return ids;
    };

    const auto first = makeIds();
    const auto second = makeIds();
    ASSERT_EQ(first.size(), 2u);
    ASSERT_EQ(second.size(), 2u);
    for (const auto& id : first) {
        EXPECT_EQ(second.count(id), 0u)
            << "跨实例(=跨服务重启) event_id 碰撞会被 DB UNIQUE+OR IGNORE 静默吞掉: " << id;
    }
}

TEST(BtEventNormalizerTest, FlushAllEmptiesPendingState) {
    BtEventNormalizer normalizer;
    normalizer.submit(makeMgmtObservation(1000, "AA:00:00:00:00:09", 0x08));
    EXPECT_EQ(normalizer.pendingCount(), 1u);

    const auto events = normalizer.flushAll();
    EXPECT_EQ(events.size(), 1u);
    EXPECT_EQ(normalizer.pendingCount(), 0u);
    // 已定案的事件不应再次被 flush 出来
    EXPECT_TRUE(normalizer.flushAll().empty());
}

TEST(BtEventNormalizerTest, ResetClearsStateAndCounters) {
    BtEventNormalizer normalizer;
    normalizer.submit(makeMgmtObservation(1000, "AA:00:00:00:00:0A", 0x08));
    normalizer.flushAll();
    EXPECT_GT(normalizer.normalizedCount(), 0u);

    normalizer.reset();
    EXPECT_EQ(normalizer.pendingCount(), 0u);
    EXPECT_EQ(normalizer.normalizedCount(), 0u);
}

// ============================================================================
// 存储层（无 DB，仅内存环形缓冲）
// ============================================================================

TEST(WirelessEventStoreTest, RingBufferEvictsOldestWhenFull) {
    WirelessEventStoreConfig cfg;
    cfg.ring_capacity = 3;
    WirelessEventStore store(nullptr, cfg);

    for (int i = 0; i < 5; ++i) {
        WirelessDeviceEvent ev;
        ev.event_id = "ev-" + std::to_string(i);
        ev.device_address = "AA:BB:CC:DD:EE:0" + std::to_string(i);
        ev.timestamp_ms = 1000 + static_cast<uint64_t>(i);
        ev.event_type = DeviceEventType::LinkDisconnected;
        store.recordEvent(ev);
    }

    EXPECT_EQ(store.totalRecorded(), 5u) << "总计数不受环形缓冲容量影响";

    const auto recent = store.recentEvents("", 10);
    ASSERT_EQ(recent.size(), 3u) << "容量为 3 时只保留最近 3 条";
    // 倒序返回，最近的是 ev-4
    EXPECT_EQ(recent[0].event_id, "ev-4");
    EXPECT_EQ(recent[2].event_id, "ev-2");
}

TEST(WirelessEventStoreTest, RecentEventsFiltersByDeviceAddress) {
    WirelessEventStore store(nullptr);

    WirelessDeviceEvent a;
    a.event_id = "ev-a";
    a.device_address = "AA:AA:AA:AA:AA:AA";
    a.timestamp_ms = 1000;
    WirelessDeviceEvent b;
    b.event_id = "ev-b";
    b.device_address = "BB:BB:BB:BB:BB:BB";
    b.timestamp_ms = 2000;

    store.recordEvent(a);
    store.recordEvent(b);

    const auto filtered = store.recentEvents("AA:AA:AA:AA:AA:AA", 10);
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].event_id, "ev-a");
}

TEST(WirelessEventStoreTest, StoreBackfillsIdentityWhenEventOmitsIt) {
    WirelessEventStoreConfig cfg;
    cfg.gateway_id = "gw-fallback";
    WirelessEventStore store(nullptr, cfg);

    WirelessDeviceEvent ev;
    ev.event_id = "ev-1";
    ev.device_address = "AA:BB:CC:DD:EE:FF";
    // 刻意不设置 gateway_id / site_id
    store.recordEvent(ev);

    const auto recent = store.recentEvents("", 1);
    ASSERT_EQ(recent.size(), 1u);
    EXPECT_EQ(recent[0].gateway_id, "gw-fallback")
        << "事件必须能回答来自哪台网关，缺失时由 store 兜底";
    // Phase 1 运行模型是 1 Gateway = 1 Site，site 回落为 gateway
    EXPECT_EQ(recent[0].site_id, "gw-fallback");
}

TEST(WirelessEventStoreTest, QueryPersistedReturnsEmptyArrayWithoutDatabase) {
    WirelessEventStore store(nullptr);
    EXPECT_EQ(store.queryPersisted("", "", 0, 0, 10), "[]");
}

// ============================================================================
// 域区分：mgmt 枚举 vs HCI error code（板端源码+寄存器 dump 双重实锤）
// ============================================================================

TEST(BtReasonDomainTest, MgmtCodesAreNotHciCodes) {
    // 内核 mgmt.h 的 MGMT_DEV_DISCONN_*：mgmt_device_disconnected 的 reason 参数域。
    // 若误用 HCI 映射，0x02（本地断开）会被当成 HCI "Unknown Connection Identifier"
    // 归入 Other —— 板端实测踩过，本机断开被记成 OTHER，而 btmon 同一次断连
    // 显示 "Connection Terminated By Local Host (0x16)"，语义其实是本地断开。
    EXPECT_EQ(disconnectReasonFromMgmtCode(0x00), DisconnectReason::Unknown);
    EXPECT_EQ(disconnectReasonFromMgmtCode(0x01), DisconnectReason::ConnectionTimeout);
    EXPECT_EQ(disconnectReasonFromMgmtCode(0x02), DisconnectReason::LocalHostTerminated);
    EXPECT_EQ(disconnectReasonFromMgmtCode(0x03), DisconnectReason::RemoteUserTerminated);
    EXPECT_EQ(disconnectReasonFromMgmtCode(0x04), DisconnectReason::AuthenticationFailure);
    EXPECT_EQ(disconnectReasonFromMgmtCode(0x05), DisconnectReason::LocalHostTerminated);
    EXPECT_EQ(disconnectReasonFromMgmtCode(0x42), DisconnectReason::Other);

    // 同一个数值 0x02 在 HCI 域另有含义（Unknown Connection Identifier，不在断连
    // 分类表里 → Other），两个域**不得互套**。这一行就是域不通用的回归护栏：
    EXPECT_EQ(disconnectReasonFromHciCode(0x02), DisconnectReason::Other);
    EXPECT_NE(disconnectReasonFromMgmtCode(0x02),
              disconnectReasonFromHciCode(0x02));
}

TEST(BtReasonDomainTest, NormalizerMapsMgmtReasonBySource) {
    // 归一化器必须按 source 选映射：mgmt 来源的 raw=0x02 应定案为本地断开
    BtEventNormalizer normalizer;
    auto obs = makeMgmtObservation(1000, "AA:00:00:00:00:01", 0x02);
    normalizer.submit(obs);

    const auto events = normalizer.flushExpired(5000);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].reason, DisconnectReason::LocalHostTerminated)
        << "mgmt 域 0x02 = MGMT_DEV_DISCONN_LOCAL_HOST，不能当 HCI 码映射";
    EXPECT_EQ(events[0].raw_reason_code, 0x02) << "原始值必须原样保留";
}

// ============================================================================
// 地址类型归类：必须同时看 link_type（HCI 域，非 BDADDR_* 域）
// ============================================================================

TEST(BtAddressTypeTest, ClassifiesByLinkTypeAndHciAddrType) {
    // 板端实测：LE 随机地址设备（手机）link_type=0x80、addr_type=0x01，
    // 旧映射只看 addr_type 会误判为 LE_PUBLIC；只看 addr_type=0 还可能误判 BREDR。
    EXPECT_EQ(btAddressTypeFromKernel(kKernelLeLink, 0x00), BtAddressType::LePublic);
    EXPECT_EQ(btAddressTypeFromKernel(kKernelLeLink, 0x01), BtAddressType::LeRandom);
    // resolved 变体（2/3）内核 link_to_bdaddr 同样归 RANDOM
    EXPECT_EQ(btAddressTypeFromKernel(kKernelLeLink, 0x02), BtAddressType::LeRandom);
    EXPECT_EQ(btAddressTypeFromKernel(kKernelLeLink, 0x03), BtAddressType::LeRandom);

    // 经典链路：不论 addr_type 取值一律 BR/EDR
    EXPECT_EQ(btAddressTypeFromKernel(kKernelAclLink, 0x00), BtAddressType::Bredr);
    EXPECT_EQ(btAddressTypeFromKernel(kKernelAclLink, 0x01), BtAddressType::Bredr);
    // SCO_LINK=0x00 同样非 LE
    EXPECT_EQ(btAddressTypeFromKernel(0x00, 0x00), BtAddressType::Bredr);
}

// ============================================================================
// BDADDR 字节序（板端实测踩过的坑）
// ============================================================================

TEST(BdaddrByteOrderTest, KernelOrderIsReversedRelativeToHumanReadable) {
    // 内核 bdaddr_t 反序存储：b[0] 是最低字节。
    // 连接 "AA:BB:CC:DD:EE:01" 时内核传来的字节是 01 EE DD CC BB AA。
    const uint8_t kernel_bytes[6] = {0x01, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA};
    EXPECT_EQ(formatBdaddr(kernel_bytes), "AA:BB:CC:DD:EE:01")
        << "显示顺序必须与 BlueZ ba2str() 一致（b[5] 在前），"
        << "否则记录的设备地址与 D-Bus 侧对不上";
}

TEST(BdaddrByteOrderTest, ParseRoundTripsWithFormat) {
    const uint8_t kernel_bytes[6] = {0x01, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA};
    EXPECT_EQ(formatBdaddr(kernel_bytes), "AA:BB:CC:DD:EE:01");

    uint8_t parsed[6] = {0};
    ASSERT_TRUE(parseBdaddr("AA:BB:CC:DD:EE:01", parsed));
    EXPECT_EQ(memcmp(parsed, kernel_bytes, 6), 0)
        << "parseBdaddr 必须是 formatBdaddr 的逆运算（同样反序填充）";
}

TEST(BdaddrByteOrderTest, ParseRejectsBadInput) {
    uint8_t out[6] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    EXPECT_FALSE(parseBdaddr("AA-BB-CC-DD-EE-FF", out)) << "必须严格要求冒号分隔";
    EXPECT_FALSE(parseBdaddr("AA:BB:CC", out));
    EXPECT_FALSE(parseBdaddr("", out));
    // 失败时 out 不变
    EXPECT_EQ(out[0], 0xAA);
}

// ============================================================================
// 存储层 + SQLite：NULL 语义与原始事实落库
// ============================================================================

class WirelessEventStoreDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_path_ = ::testing::TempDir() + "/test_device_events.db";
        std::remove(db_path_.c_str());
        db_ = std::make_unique<DatabaseManager>(db_path_);
        ASSERT_TRUE(db_->isOpen()) << "临时数据库应能打开: " << db_path_;
    }

    void TearDown() override {
        db_.reset();
        std::remove(db_path_.c_str());
    }

    std::string db_path_;
    std::unique_ptr<DatabaseManager> db_;
};

TEST_F(WirelessEventStoreDbTest, NullRssiAndSuspectedCausePersistAsNull) {
    WirelessEventStore store(db_.get());

    WirelessDeviceEvent ev;
    ev.event_id = "ev-null-test";
    ev.timestamp_ms = 1759142400000ULL;
    ev.gateway_id = "gw-1";
    ev.device_address = "AA:BB:CC:DD:EE:FF";
    ev.address_type = BtAddressType::Bredr;
    ev.event_type = DeviceEventType::LinkDisconnected;
    ev.raw_reason_code = 0x08;
    ev.reason = DisconnectReason::ConnectionTimeout;
    ev.source = EvidenceSource::KernelMgmt;
    ev.source_detail = "mgmt_device_disconnected";
    // rssi 与 suspected_cause 都留空
    ASSERT_TRUE(store.recordEvent(ev));
    EXPECT_EQ(store.persistFailures(), 0u);

    const std::string json = store.queryPersisted("AA:BB:CC:DD:EE:FF", "", 0, 0, 10);
    EXPECT_NE(json.find("\"event_id\":\"ev-null-test\""), std::string::npos) << json;
    EXPECT_NE(json.find("\"rssi_at_event_dbm\":null"), std::string::npos)
        << "未采集的 RSSI 落库再读回仍是 null；输出: " << json;
    EXPECT_NE(json.find("\"suspected_cause\":null"), std::string::npos)
        << "Phase 1 不产出推断原因；输出: " << json;
    // 原始事实无损
    EXPECT_NE(json.find("\"raw_reason_code\":8"), std::string::npos) << json;
    EXPECT_NE(json.find("\"reason\":\"CONNECTION_TIMEOUT\""), std::string::npos) << json;
    EXPECT_NE(json.find("\"source\":\"KERNEL_MGMT\""), std::string::npos) << json;
}

TEST_F(WirelessEventStoreDbTest, DuplicateEventIdIsIgnoredNotDuplicated) {
    WirelessEventStore store(db_.get());

    WirelessDeviceEvent ev;
    ev.event_id = "ev-dup";
    ev.timestamp_ms = 1759142400000ULL;
    ev.gateway_id = "gw-1";
    ev.device_address = "AA:BB:CC:DD:EE:01";
    ev.event_type = DeviceEventType::LinkDisconnected;

    store.recordEvent(ev);
    store.recordEvent(ev);  // 同 event_id 重复写入

    const std::string json = store.queryPersisted("AA:BB:CC:DD:EE:01", "", 0, 0, 10);
    // event_id 唯一约束保证不会出现两行
    size_t occurrences = 0;
    size_t pos = 0;
    while ((pos = json.find("\"ev-dup\"", pos)) != std::string::npos) {
        ++occurrences;
        pos += 8;
    }
    EXPECT_EQ(occurrences, 1u) << "同一 event_id 只能落一行；输出: " << json;
}

TEST_F(WirelessEventStoreDbTest, EventTypeFilterWorks) {
    // 事件类型过滤是 QueryDeviceEvents / history_query_tool --events 的核心过滤条件：
    // 运维要能只看 LINK_DISCONNECTED，不被 DEVICE_APPEARED 等稀释
    WirelessEventStore store(db_.get());

    auto makeEvent = [&](const std::string& id, const std::string& type) {
        WirelessDeviceEvent ev;
        ev.event_id = id;
        ev.timestamp_ms = 1759142400000ULL;
        ev.gateway_id = "gw-1";
        ev.device_address = "AA:BB:CC:DD:EE:03";
        ev.event_type = deviceEventTypeFromString(type, DeviceEventType::DeviceLost);
        return ev;
    };

    store.recordEvent(makeEvent("ev-disc", "LINK_DISCONNECTED"));
    store.recordEvent(makeEvent("ev-appr", "DEVICE_APPEARED"));

    const std::string all = store.queryPersisted("AA:BB:CC:DD:EE:03", "", 0, 0, 10);
    EXPECT_NE(all.find("ev-disc"), std::string::npos);
    EXPECT_NE(all.find("ev-appr"), std::string::npos);

    const std::string disc = store.queryPersisted("AA:BB:CC:DD:EE:03",
                                                  "LINK_DISCONNECTED", 0, 0, 10);
    EXPECT_NE(disc.find("ev-disc"), std::string::npos) << disc;
    EXPECT_EQ(disc.find("ev-appr"), std::string::npos)
        << "类型过滤必须排除非匹配事件；输出: " << disc;

    // 组合过滤：类型 + 时间窗都生效
    // 组合过滤：类型 + 时间窗都生效（窗口必须覆盖事件时间戳，否则两个条件
    // 里有一个不匹配时会把"过滤生效"与"窗口拦住"混淆，测不出独立语义）
    const std::string combo = store.queryPersisted(
        "AA:BB:CC:DD:EE:03", "DEVICE_APPEARED",
        1759142399999LL, 1759142400001LL, 10);
    EXPECT_NE(combo.find("ev-appr"), std::string::npos);
}

TEST_F(WirelessEventStoreDbTest, TimeRangeFilterWorks) {
    WirelessEventStore store(db_.get());

    auto makeEvent = [](const std::string& id, uint64_t ts) {
        WirelessDeviceEvent ev;
        ev.event_id = id;
        ev.timestamp_ms = ts;
        ev.gateway_id = "gw-1";
        ev.device_address = "AA:BB:CC:DD:EE:02";
        ev.event_type = DeviceEventType::LinkDisconnected;
        return ev;
    };

    store.recordEvent(makeEvent("ev-t1", 1000000));
    store.recordEvent(makeEvent("ev-t2", 2000000));
    store.recordEvent(makeEvent("ev-t3", 3000000));

    const std::string json = store.queryPersisted("", "", 1500000, 2500000, 10);
    EXPECT_NE(json.find("ev-t2"), std::string::npos) << json;
    EXPECT_EQ(json.find("ev-t1"), std::string::npos) << "早于起始时间的事件不应返回";
    EXPECT_EQ(json.find("ev-t3"), std::string::npos) << "晚于结束时间的事件不应返回";
}

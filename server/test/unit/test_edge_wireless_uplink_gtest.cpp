// test_edge_wireless_uplink_gtest.cpp
// 无线事实上行器单元测试（Phase 4a）
//
// 覆盖面：
//   1. 报文字节契约（字段名/类型/空批拒绝/批次上限）——板↔云契约的最上游；
//   2. 游标语义：缺失/损坏即从 0 重扫（宁可重发，绝不跳发）；
//   3. selectSince 的严格大于过滤（防止已确认事实被反复重发）。

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "database_manager.hpp"
#include "edge_wireless_uplink_exporter.hpp"
#include "site_incident.hpp"
#include "weaknet_config.hpp"
#include "wireless_event.hpp"

using namespace weaknet;
using namespace weaknet_dbus;

namespace {

std::string uniqueTempPath(const std::string& tag) {
    return "/tmp/weaknet_uplink_" + std::to_string(getpid()) + "_" + tag;
}

WirelessDeviceEvent makeEvent(const std::string& id, uint64_t ts_ms) {
    WirelessDeviceEvent ev;
    ev.event_id = id;
    ev.timestamp_ms = ts_ms;
    ev.site_id = "site-1";
    ev.gateway_id = "gw-1";
    ev.protocol = WirelessProtocol::Bluetooth;
    ev.device_address = "AA:BB:CC:DD:EE:01";
    ev.address_type = BtAddressType::LeRandom;
    ev.hci_index = 0;
    ev.event_type = DeviceEventType::LinkDisconnected;
    ev.reason = DisconnectReason::LocalHostTerminated;
    ev.source = EvidenceSource::KernelMgmt;
    ev.source_detail = "mgmt_device_disconnected";
    ev.raw_reason_code = 2;
    ev.rssi_at_event_dbm = -71;
    ev.details_json = "{\"raw_evidence\":[]}";
    return ev;
}

SiteIncident makeIncident(const std::string& id) {
    SiteIncident inc;
    inc.incident_id = id;
    inc.site_id = "site-1";
    inc.gateway_id = "gw-1";
    inc.started_at_ms = 1'700'000'001'000ULL;
    inc.last_event_ms = 1'700'000'001'500ULL;
    inc.affected_devices = 3;
    inc.state = IncidentState::Ongoing;
    return inc;
}

DeviceLinkProfile makeBaseline() {
    DeviceLinkProfile p;
    p.key.site_id = "site-1";
    p.key.gateway_id = "gw-1";
    p.key.hci_index = 0;
    p.key.protocol = WirelessProtocol::Bluetooth;
    p.key.address_type = BtAddressType::LeRandom;
    p.key.device_address = "AA:BB:CC:DD:EE:01";
    p.baseline_rssi_dbm = static_cast<int16_t>(-66);
    p.min_seen_rssi_dbm = static_cast<int16_t>(-78);
    p.max_seen_rssi_dbm = static_cast<int16_t>(-59);
    p.baseline_sample_count = 12;
    p.state = LinkQualityState::Stable;
    p.updated_at_ms = 1'700'000'001'500ULL;
    return p;
}

/// 按 WirelessDeviceEvent 结构体整行入库（字段映射与生产 recordEvent 一致）
void insertEvent(DatabaseManager& db, const WirelessDeviceEvent& ev) {
    ASSERT_TRUE(db.insertDeviceEvent(ev.event_id, ev.timestamp_ms, ev.site_id, ev.gateway_id,
                                     toString(ev.protocol), ev.device_address,
                                     toString(ev.address_type), ev.hci_index,
                                     toString(ev.event_type), ev.rssi_at_event_dbm,
                                     ev.raw_reason_code, toString(ev.reason),
                                     toString(ev.source), ev.source_detail,
                                     std::nullopt, ev.details_json));
}

}  // namespace

// ============================================================================
// 报文字节契约
// ============================================================================

TEST(EdgeWirelessUplinkBody, EmitsSchemaAndAllGroups) {
    WirelessUplinkPayload payload;
    payload.device_id = "radxa-cubie-a7a";
    payload.watermark_ms = 1'700'000'001'500ULL;
    payload.events = {makeEvent("evt-1", 1'700'000'001'000ULL)};
    payload.incidents = {makeIncident("site-1_1700000001000")};
    payload.incident_evidence = {{"evt-1", "evt-2"}};
    payload.baselines = {makeBaseline()};

    std::string error;
    const std::string body = EdgeWirelessUplinkExporter::buildBody(payload, &error);
    ASSERT_FALSE(body.empty()) << error;

    // schema_version 必须逐字匹配云端 pydantic Literal
    EXPECT_NE(body.find("\"schema_version\":\"network.edge.wireless-events.v1\""),
              std::string::npos);
    EXPECT_NE(body.find("\"device_id\":\"radxa-cubie-a7a\""), std::string::npos);
    EXPECT_NE(body.find("\"watermark_ms\":1700000001500"), std::string::npos);

    // 事件组：身份 + 原因 + RSSI 不能缺
    EXPECT_NE(body.find("\"event_id\":\"evt-1\""), std::string::npos);
    EXPECT_NE(body.find("\"event_type\":\"LINK_DISCONNECTED\""), std::string::npos);
    EXPECT_NE(body.find("\"reason\":\"LOCAL_HOST_TERMINATED\""), std::string::npos);
    EXPECT_NE(body.find("\"rssi_at_event_dbm\":-71"), std::string::npos);
    EXPECT_NE(body.find("\"source\":\"KERNEL_MGMT\""), std::string::npos);

    // 事故组：证据回链必须随本体上行（可追溯性）
    EXPECT_NE(body.find("\"incident_id\":\"site-1_1700000001000\""), std::string::npos);
    EXPECT_NE(body.find("\"state\":\"ONGOING\""), std::string::npos);
    EXPECT_NE(body.find("\"evidence_event_ids\":[\"evt-1\",\"evt-2\"]"), std::string::npos);

    // 基线组：复合身份字段齐全（跨网关/跨控制器不合并）
    EXPECT_NE(body.find("\"baseline_rssi_dbm\":-66"), std::string::npos);
    EXPECT_NE(body.find("\"baseline_sample_count\":12"), std::string::npos);
    EXPECT_NE(body.find("\"state\":\"STABLE\""), std::string::npos);

    // 环境证据：无深度内核快照时诚实上报不可用，不发伪证据
    EXPECT_NE(body.find("\"available\":false"), std::string::npos);
    EXPECT_NE(body.find("\"snapshots\":[]"), std::string::npos);
}

TEST(EdgeWirelessUplinkBody, EmitsKernelSnapshotsWhenPresent) {
    WirelessUplinkPayload payload;
    payload.device_id = "gw";
    payload.events = {makeEvent("evt-k", 1'700'000'001'000ULL)};
    payload.env_to_ms = 1'700'000'010'000ULL;

    weaknet_dbus::ProcessNetInfo proc;
    proc.pid = 1234;
    proc.comm = "iperf3";
    proc.txBytes = 80'000'000;
    proc.txPackets = 60'000;
    proc.retransCount = 42;
    payload.top_processes = {proc};

    weaknet_dbus::DropStatsSummary drop;
    drop.totalDrops = 1024;
    weaknet_dbus::DropReasonItem item;
    item.reasonCode = 6;
    item.reasonName = "NETFILTER_DROP";
    item.humanDesc = "被 netfilter 规则丢弃";
    item.protocol = "TCP/IPv4";
    item.count = 1024;
    drop.topReasons = {item};
    payload.drop_stats = drop;

    std::string error;
    const std::string body = EdgeWirelessUplinkExporter::buildBody(payload, &error);
    ASSERT_FALSE(body.empty()) << error;

    // 有数据时才 available=true（绝不空壳冒充）
    EXPECT_NE(body.find("\"available\":true"), std::string::npos);

    // 进程画像条目
    EXPECT_NE(body.find("\"kind\":\"process_top\""), std::string::npos);
    EXPECT_NE(body.find("\"pid\":1234"), std::string::npos);
    EXPECT_NE(body.find("\"comm\":\"iperf3\""), std::string::npos);
    EXPECT_NE(body.find("\"retrans_count\":42"), std::string::npos);

    // 内核丢包归因条目
    EXPECT_NE(body.find("\"kind\":\"skb_drop_hist\""), std::string::npos);
    EXPECT_NE(body.find("\"reason_name\":\"NETFILTER_DROP\""), std::string::npos);
    EXPECT_NE(body.find("\"total_drops\":1024"), std::string::npos);
}

TEST(EdgeWirelessUplinkBody, OmitsKernelSnapshotsWhenAbsent) {
    WirelessUplinkPayload payload;
    payload.device_id = "gw";
    payload.events = {makeEvent("evt-nk", 1'700'000'001'000ULL)};
    payload.env_to_ms = 1'700'000'010'000ULL;
    // top_processes / drop_stats 均为空

    std::string error;
    const std::string body = EdgeWirelessUplinkExporter::buildBody(payload, &error);
    ASSERT_FALSE(body.empty()) << error;

    EXPECT_NE(body.find("\"available\":false"), std::string::npos);
    EXPECT_EQ(body.find("process_top"), std::string::npos);
    EXPECT_EQ(body.find("skb_drop_hist"), std::string::npos);
}

// O1-A：TCP 重传/连接聚合进 env_window（kind=tcp_loss_agg）。
// 有数据才 available=true 且带 kind；缺数据诚实缺席，与其它三类同纪律。
TEST(EdgeWirelessUplinkBody, EmitsTcpLossAggWhenPresent) {
    WirelessUplinkPayload payload;
    payload.device_id = "gw";
    payload.events = {makeEvent("evt-tla", 1'700'000'001'000ULL)};
    payload.env_to_ms = 1'700'000'010'000ULL;
    payload.tcp_retrans_loss_pct = 1.25;
    weaknet_dbus::TcpConnAggStats cs;
    cs.totalAccepts = 30;
    cs.totalCloses = 28;
    cs.activeInbound = 2;
    cs.totalAcceptFailures = 1;
    payload.tcp_conn_stats = cs;

    std::string error;
    const std::string body = EdgeWirelessUplinkExporter::buildBody(payload, &error);
    ASSERT_FALSE(body.empty()) << error;

    EXPECT_NE(body.find("\"available\":true"), std::string::npos);
    EXPECT_NE(body.find("\"kind\":\"tcp_loss_agg\""), std::string::npos);
    EXPECT_NE(body.find("\"loss_rate_pct\":1.25"), std::string::npos);
    EXPECT_NE(body.find("\"accepts\":30"), std::string::npos);
    EXPECT_NE(body.find("\"active\":2"), std::string::npos);
}

TEST(EdgeWirelessUplinkBody, OmitsTcpLossAggWhenAbsent) {
    WirelessUplinkPayload payload;
    payload.device_id = "gw";
    payload.events = {makeEvent("evt-nla", 1'700'000'001'000ULL)};
    payload.env_to_ms = 1'700'000'010'000ULL;

    std::string error;
    const std::string body = EdgeWirelessUplinkExporter::buildBody(payload, &error);
    ASSERT_FALSE(body.empty()) << error;

    EXPECT_EQ(body.find("tcp_loss_agg"), std::string::npos);
    EXPECT_EQ(body.find("loss_rate_pct"), std::string::npos);
}

TEST(EdgeWirelessUplinkBody, NullRssiStaysJsonNull) {
    WirelessUplinkPayload payload;
    payload.device_id = "gw";
    auto ev = makeEvent("evt-null", 1'700'000'000'000ULL);
    ev.rssi_at_event_dbm = std::nullopt;  // 未采集 != 0
    payload.events = {ev};

    std::string error;
    const std::string body = EdgeWirelessUplinkExporter::buildBody(payload, &error);
    ASSERT_FALSE(body.empty()) << error;
    EXPECT_NE(body.find("\"rssi_at_event_dbm\":null"), std::string::npos);
    EXPECT_EQ(body.find("\"rssi_at_event_dbm\":0"), std::string::npos);
}

TEST(EdgeWirelessUplinkBody, RejectsEmptyBatchAndMissingDevice) {
    WirelessUplinkPayload empty;
    std::string error;
    EXPECT_TRUE(EdgeWirelessUplinkExporter::buildBody(empty, &error).empty());
    EXPECT_FALSE(error.empty());

    WirelessUplinkPayload no_device;
    no_device.events = {makeEvent("e", 1)};
    error.clear();
    EXPECT_TRUE(EdgeWirelessUplinkExporter::buildBody(no_device, &error).empty());
}

TEST(EdgeWirelessUplinkBody, RejectsOverCapBatch) {
    WirelessUplinkPayload payload;
    payload.device_id = "gw";
    for (size_t i = 0; i <= EDGE_WIRELESS_MAX_EVENTS_PER_BATCH; ++i) {
        payload.events.push_back(makeEvent("evt-" + std::to_string(i), i + 1));
    }
    std::string error;
    EXPECT_TRUE(EdgeWirelessUplinkExporter::buildBody(payload, &error).empty());
    EXPECT_FALSE(error.empty());
}

TEST(EdgeWirelessUplinkBody, EscapesHostileEvidenceText) {
    WirelessUplinkPayload payload;
    payload.device_id = "gw";
    auto ev = makeEvent("evt-esc", 1'700'000'000'000ULL);
    // 采集层把内核给出的 hook 名无损带上；测试确认转义不会破坏报文
    ev.source_detail = "evil\"\n\\hook";
    payload.events = {ev};

    std::string error;
    const std::string body = EdgeWirelessUplinkExporter::buildBody(payload, &error);
    ASSERT_FALSE(body.empty()) << error;
    EXPECT_EQ(body.find("evil\"\n"), std::string::npos);
    EXPECT_NE(body.find("evil\\\"\\n\\\\hook"), std::string::npos);
}

// ============================================================================
// 过滤与游标
// ============================================================================

TEST(EdgeWirelessUplinkSelect, KeepsOnlyFactsStrictlyAfterWatermark) {
    WirelessUplinkPayload all;
    all.device_id = "gw";
    all.events = {makeEvent("old", 1'000ULL), makeEvent("edge", 2'000ULL),
                  makeEvent("new", 3'000ULL)};

    const auto filtered = EdgeWirelessUplinkExporter::selectSince(all, 2'000ULL);
    ASSERT_EQ(filtered.events.size(), 1u);
    EXPECT_EQ(filtered.events[0].event_id, "new");
}

TEST(EdgeWirelessUplinkCursor, MissingLegacyAndCorruptCursorRescanPerDomain) {
    // 游标语义必须真实覆盖实现：用临时 SQLite 库构造上行器（构建廉价，
    // 且 loadCursors/storeCursors 不访问数据库，但构造要求有 db 引用）。
    const std::string db_path = uniqueTempPath("cursor.db");
    const std::string state_path = uniqueTempPath("cursor.state");
    std::remove(db_path.c_str());
    std::remove(state_path.c_str());

    WeakNetConfig cfg;
    DatabaseManager db(db_path);
    ASSERT_TRUE(db.isOpen());

    {
        EdgeWirelessUplinkExporter exporter(cfg, db, state_path);
        // 文件缺失 → 三域全 0（宁可重发，绝不跳发）
        const auto zero = exporter.loadCursors();
        EXPECT_EQ(zero.events_ms, 0u);
        EXPECT_EQ(zero.incidents_ms, 0u);
        EXPECT_EQ(zero.baselines_ms, 0u);

        exporter.storeCursors({1'700'000'001'500ULL, 1'700'000'002'500ULL, 1'700'000'003'500ULL});
        const auto c = exporter.loadCursors();
        EXPECT_EQ(c.events_ms, 1'700'000'001'500ULL);
        EXPECT_EQ(c.incidents_ms, 1'700'000'002'500ULL);
        EXPECT_EQ(c.baselines_ms, 1'700'000'003'500ULL);
        // 运行期原子游标与持久化保持一致（collect 读原子、重启读文件）
        EXPECT_EQ(exporter.watermarkMs(), 1'700'000'003'500ULL);
    }

    // 旧格式（仅 events_ms）→ 另两域回退 0 全量重扫：
    // 单水位时代的截断丢发只有靠重扫才能被补回，服务端按 id 幂等吸收重发。
    {
        std::ofstream legacy(state_path, std::ios::trunc);
        legacy << "events_ms=1700000001500\n";
    }
    {
        EdgeWirelessUplinkExporter exporter(cfg, db, state_path);
        const auto c = exporter.loadCursors();
        EXPECT_EQ(c.events_ms, 1'700'000'001'500ULL);
        EXPECT_EQ(c.incidents_ms, 0u);
        EXPECT_EQ(c.baselines_ms, 0u);
    }

    // 单键损坏只回退该域，不牵连其余两域的已确认位置
    {
        std::ofstream bad(state_path, std::ios::trunc);
        bad << "events_ms=not-a-number\nincidents_ms=7\nbaselines_ms=8\n";
    }
    {
        EdgeWirelessUplinkExporter exporter(cfg, db, state_path);
        const auto c = exporter.loadCursors();
        EXPECT_EQ(c.events_ms, 0u);
        EXPECT_EQ(c.incidents_ms, 7u);
        EXPECT_EQ(c.baselines_ms, 8u);
    }

    std::remove(state_path.c_str());
    std::remove(db_path.c_str());
}

// ============================================================================
// 三域独立游标：跨域水位推进不再截断丢事实（C2 回归）
// ============================================================================

TEST(EdgeWirelessUplinkCursor, AdvanceStaysWithinEachDomain) {
    // 事故/基线时间更晚也不能推进事件游标——单水位时代正是这里把
    // 截断在批上限之后的未发事件永久跳过。
    WirelessUplinkPayload sent;
    sent.events = {makeEvent("e2", 2'000ULL), makeEvent("e3", 3'000ULL)};
    auto inc = makeIncident("inc-1");
    inc.last_event_ms = 9'000ULL;
    sent.incidents = {inc};
    auto base = makeBaseline();
    base.updated_at_ms = 8'000ULL;
    sent.baselines = {base};

    const auto after =
        EdgeWirelessUplinkExporter::advanceCursors({1'000ULL, 1'000ULL, 1'000ULL}, sent);
    EXPECT_EQ(after.events_ms, 3'000ULL);
    EXPECT_EQ(after.incidents_ms, 9'000ULL);
    EXPECT_EQ(after.baselines_ms, 8'000ULL);

    // 只有事件域有进展时，其余域原地不动（不回退、不越界）
    WirelessUplinkPayload only_events;
    only_events.events = {makeEvent("e4", 5'500ULL)};
    const auto after2 =
        EdgeWirelessUplinkExporter::advanceCursors({5'000ULL, 4'000ULL, 6'000ULL}, only_events);
    EXPECT_EQ(after2.events_ms, 5'500ULL);
    EXPECT_EQ(after2.incidents_ms, 4'000ULL);
    EXPECT_EQ(after2.baselines_ms, 6'000ULL);

    // 空批不推进任何域
    const auto after3 =
        EdgeWirelessUplinkExporter::advanceCursors({5'000ULL, 4'000ULL, 6'000ULL}, {});
    EXPECT_EQ(after3.events_ms, 5'000ULL);
    EXPECT_EQ(after3.incidents_ms, 4'000ULL);
    EXPECT_EQ(after3.baselines_ms, 6'000ULL);
}

TEST(EdgeWirelessUplinkCollect, UsesIndependentPerDomainCursors) {
    const std::string db_path = uniqueTempPath("collect.db");
    const std::string state_path = uniqueTempPath("collect.state");
    std::remove(db_path.c_str());
    std::remove(state_path.c_str());

    WeakNetConfig cfg;
    DatabaseManager db(db_path);
    ASSERT_TRUE(db.isOpen());

    // 三域事实：事件 ts=1000/2000，事故 last_event=4000，基线 updated=4500
    insertEvent(db, makeEvent("e1", 1'000ULL));
    insertEvent(db, makeEvent("e2", 2'000ULL));
    auto inc = makeIncident("inc-1");
    inc.started_at_ms = 3'000ULL;
    inc.last_event_ms = 4'000ULL;
    ASSERT_TRUE(db.upsertSiteIncident(inc.incident_id, inc.site_id, inc.gateway_id,
                                      static_cast<int64_t>(inc.started_at_ms),
                                      static_cast<int64_t>(inc.last_event_ms),
                                      std::nullopt, inc.affected_devices,
                                      toString(inc.state), std::nullopt));
    auto base = makeBaseline();
    base.updated_at_ms = 4'500ULL;
    ASSERT_TRUE(db.upsertDeviceBaseline(
        base.key.site_id, base.key.gateway_id, base.key.hci_index,
        toString(base.key.protocol), toString(base.key.address_type),
        base.key.device_address, base.baseline_rssi_dbm, base.min_seen_rssi_dbm,
        base.max_seen_rssi_dbm, base.baseline_sample_count,
        static_cast<int64_t>(base.first_seen_ms), static_cast<int64_t>(base.last_seen_ms),
        toString(base.state), static_cast<int64_t>(base.updated_at_ms)));

    EdgeWirelessUplinkExporter exporter(cfg, db, state_path);

    // 事件游标落后、事故/基线游标领先：事件仍按自己的游标取，
    // 不被跨域水位跳过（单水位时代 ts∈(1500,5000] 的事件会被永久丢弃）
    exporter.storeCursors({1'500ULL, 5'000ULL, 5'000ULL});
    WirelessUplinkPayload p;
    std::string error;
    ASSERT_TRUE(exporter.collect(&p, &error)) << error;
    ASSERT_EQ(p.events.size(), 1u);
    EXPECT_EQ(p.events[0].event_id, "e2");
    EXPECT_TRUE(p.incidents.empty());   // 4000 < 5000：已确认
    EXPECT_TRUE(p.baselines.empty());   // 4500 < 5000：已确认
    EXPECT_EQ(p.watermark_ms, 5'000ULL);  // 回显 = 三域已确认水位的最大值

    // 反向：事件域已确认到 5000、事故/基线域从未推进
    exporter.storeCursors({5'000ULL, 0ULL, 0ULL});
    WirelessUplinkPayload p2;
    ASSERT_TRUE(exporter.collect(&p2, &error)) << error;
    EXPECT_TRUE(p2.events.empty());
    ASSERT_EQ(p2.incidents.size(), 1u);
    EXPECT_EQ(p2.incidents[0].incident_id, "inc-1");
    ASSERT_EQ(p2.baselines.size(), 1u);
    EXPECT_EQ(p2.watermark_ms, 5'000ULL);

    std::remove(state_path.c_str());
    std::remove(db_path.c_str());
}

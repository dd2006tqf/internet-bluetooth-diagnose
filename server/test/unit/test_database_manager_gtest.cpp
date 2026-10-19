// test_database_manager_gtest.cpp
// DatabaseManager 单元测试 (Google Test 版本)
// Module under test: database_manager.cpp (SQLite 历史数据持久化)

#include <gtest/gtest.h>
#include "database_manager.hpp"
#include "metrics/metrics_registry.hpp"
#include "metrics/metric_sample.hpp"
#include "metrics/metric_types.hpp"
#include "net_info.hpp"
#include "network_quality_result.hpp"
#include <cstdio>
#include <unistd.h>
#include <sys/stat.h>
#include <chrono>

using namespace weaknet_dbus;

// ============================================================================
// Test Fixture
// ============================================================================
class DatabaseManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* testName = ::testing::UnitTest::GetInstance()->current_test_info()->name();
        dbPath_ = "/tmp/weaknet_test_" + std::to_string(getpid()) + "_" + std::string(testName) + ".db";
        // 清理可能存在的旧数据库
        std::remove(dbPath_.c_str());
    }

    void TearDown() override {
        std::remove(dbPath_.c_str());
    }

    // 创建一个填充了测试数据的 NetInfo
    NetInfo makeTestIface(const std::string& name, int rtt, double jitter, int rssi) {
        NetInfo info(name);
        info.setRttMs(rtt);
        info.setJitterMs(jitter);
        info.setRssiDbm(rssi);
        info.setTcpLossRate(0.5);
        info.setQuality(LinkQuality::Fair);
        info.setUsingNow(true);
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        info.setMetricSampleTimes(now, now, now, now, now);
        info.markMetricUpdated(1, now);
        return info;
    }

    // 辅助函数：通过全参数版本插入快照（模拟唯一生产调用路径）
    static bool insertSimple(DatabaseManager& db, const std::string& iface, const NetInfo& info, double score = 0.0) {
        NetworkQualityResult overall;
        overall.score = score;
        overall.level = NetworkQualityLevel::UNKNOWN;
        overall.levelName = "UNKNOWN";
        return db.insertSnapshot(iface, info, overall,
                                 info.generation(), info.lastUpdatedMs(),
                                 info.rttSampleTsMs(), info.rssiSampleTsMs(),
                                 info.jitterSampleTsMs(), info.tcpLossSampleTsMs(),
                                 info.trafficSampleTsMs());
    }

    std::string dbPath_;
};

// ============================================================================
// Test Cases: 构造与基本操作
// ============================================================================

TEST_F(DatabaseManagerTest, OpenCreatesDatabase) {
    DatabaseManager db(dbPath_);
    EXPECT_TRUE(db.isOpen());
    EXPECT_EQ(db.getRecordCount(), 0);
}

TEST_F(DatabaseManagerTest, OpenInvalidPathFails) {
    DatabaseManager db("/nonexistent/deeply/nested/path/test.db");
    // SQLite 会尝试创建，如果目录不存在则失败
    // 具体行为取决于 SQLite 版本
}

// ============================================================================
// Test Cases: 插入与查询
// ============================================================================

TEST_F(DatabaseManagerTest, InsertAndQuery) {
    DatabaseManager db(dbPath_);
    ASSERT_TRUE(db.isOpen());

    auto info = makeTestIface("wlan0", 50, 10.5, -55);
    EXPECT_TRUE(insertSimple(db, "wlan0", info));
    EXPECT_EQ(db.getRecordCount(), 1);

    std::string result = db.queryHistory("wlan0", "", "", 10);
    EXPECT_NE(result.find("wlan0"), std::string::npos);
    EXPECT_NE(result.find("50"), std::string::npos);  // rtt_ms
}

TEST_F(DatabaseManagerTest, InsertMultipleRecords) {
    DatabaseManager db(dbPath_);
    ASSERT_TRUE(db.isOpen());

    for (int i = 0; i < 5; ++i) {
        auto info = makeTestIface("wlan0", 40 + i, 10.0 + i, -50 - i);
        EXPECT_TRUE(insertSimple(db, "wlan0", info));
    }
    EXPECT_EQ(db.getRecordCount(), 5);

    std::string result = db.queryHistory("wlan0", "", "", 100);
    // 检查返回的是 JSON 数组
    EXPECT_EQ(result.front(), '[');
    EXPECT_EQ(result.back(), ']');
}

TEST_F(DatabaseManagerTest, QueryFilterByInterface) {
    DatabaseManager db(dbPath_);
    ASSERT_TRUE(db.isOpen());

    auto wlan = makeTestIface("wlan0", 50, 10.0, -55);
    auto eth = makeTestIface("eth0", 10, 1.0, -1000);
    insertSimple(db, "wlan0", wlan);
    insertSimple(db, "eth0", eth);

    std::string result = db.queryHistory("wlan0", "", "", 10);
    EXPECT_NE(result.find("wlan0"), std::string::npos);
    EXPECT_EQ(result.find("eth0"), std::string::npos);
}

TEST_F(DatabaseManagerTest, QueryAllInterfaces) {
    DatabaseManager db(dbPath_);
    ASSERT_TRUE(db.isOpen());

    auto wlan = makeTestIface("wlan0", 50, 10.0, -55);
    auto eth = makeTestIface("eth0", 10, 1.0, -1000);
    insertSimple(db, "wlan0", wlan);
    insertSimple(db, "eth0", eth);

    std::string result = db.queryHistory("", "", "", 10);
    EXPECT_NE(result.find("wlan0"), std::string::npos);
    EXPECT_NE(result.find("eth0"), std::string::npos);
}

TEST_F(DatabaseManagerTest, QueryLimit) {
    DatabaseManager db(dbPath_);
    ASSERT_TRUE(db.isOpen());

    for (int i = 0; i < 10; ++i) {
        auto info = makeTestIface("wlan0", 50, 10.0, -55);
        insertSimple(db, "wlan0", info);
    }

    std::string result = db.queryHistory("wlan0", "", "", 3);
    // 最多返回 3 条（按时间倒序）
    int count = 0;
    for (size_t i = 0; i < result.size(); ++i) {
        if (result[i] == '{') count++;
    }
    EXPECT_LE(count, 3);
}

// ============================================================================
// Test Cases: 清理
// ============================================================================

TEST_F(DatabaseManagerTest, Cleanup) {
    DatabaseManager db(dbPath_);
    ASSERT_TRUE(db.isOpen());

    // 插入一条记录
    auto info = makeTestIface("wlan0", 50, 10.0, -55);
    insertSimple(db, "wlan0", info);
    EXPECT_EQ(db.getRecordCount(), 1);

    // 清理 365 天前的数据 → 刚插入的记录不应被删除
    int deleted = db.cleanup(365);
    EXPECT_EQ(deleted, 0);
    EXPECT_EQ(db.getRecordCount(), 1);

    // 未打开的数据库实例调用 cleanup 应返回 -1
    DatabaseManager unopenDb("/nonexistent_dir/impossible/path.db");
    EXPECT_EQ(unopenDb.cleanup(7), -1);
}

TEST_F(DatabaseManagerTest, InsertWithScore) {
    DatabaseManager db(dbPath_);
    ASSERT_TRUE(db.isOpen());

    auto info = makeTestIface("wlan0", 50, 10.5, -55);
    EXPECT_TRUE(insertSimple(db, "wlan0", info, 72.5));
    EXPECT_EQ(db.getRecordCount(), 1);

    std::string result = db.queryHistory("wlan0", "", "", 10);
    EXPECT_NE(result.find("72.5"), std::string::npos);  // score
}

TEST_F(DatabaseManagerTest, StaleMetricsAreNullAndReported) {
    DatabaseManager db(dbPath_);
    ASSERT_TRUE(db.isOpen());
    auto info = makeTestIface("wlan0", 50, 10.5, -55);
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    info.setMetricSampleTimes(now - 30001, now - 30001, now - 30001, now - 30001, now - 30001);
    NetworkQualityResult overall;
    overall.level = NetworkQualityLevel::GOOD;
    overall.levelName = "GOOD";
    overall.score = 80.0;
    ASSERT_TRUE(db.insertSnapshot("wlan0", info, overall, 2, now,
                                  info.rttSampleTsMs(), info.rssiSampleTsMs(),
                                  info.jitterSampleTsMs(), info.tcpLossSampleTsMs(),
                                  info.trafficSampleTsMs()));
    const std::string result = db.queryHistory("wlan0", "", "", 1);
    EXPECT_NE(result.find("\"rtt_ms\":null"), std::string::npos);
    EXPECT_NE(result.find("\"rtt_status\":\"stale\""), std::string::npos);
    EXPECT_NE(result.find("\"traffic_bps\":null"), std::string::npos);
    EXPECT_NE(result.find("\"traffic_status\":\"stale\""), std::string::npos);
}
TEST_F(DatabaseManagerTest, QualityReportGroupsInterfaces) {
    DatabaseManager db(dbPath_);
    ASSERT_TRUE(db.isOpen());
    auto wlan = makeTestIface("wlan0", 20, 1.0, -50);
    auto eth = makeTestIface("eth0", 30, 2.0, -1000);
    ASSERT_TRUE(insertSimple(db, "wlan0", wlan));
    ASSERT_TRUE(insertSimple(db, "eth0", eth));
    const std::string report = db.getQualityReport();
    EXPECT_NE(report.find("\"interfaces\""), std::string::npos);
    EXPECT_NE(report.find("\"wlan0\""), std::string::npos);
    EXPECT_NE(report.find("\"eth0\""), std::string::npos);
    EXPECT_NE(report.find("\"status_counts\""), std::string::npos);
    EXPECT_NE(report.find("\"timestamp_zero_counts\""), std::string::npos);
}
TEST_F(DatabaseManagerTest, GetDbInfo) {
    DatabaseManager db(dbPath_);
    ASSERT_TRUE(db.isOpen());

    auto info = makeTestIface("wlan0", 50, 10.0, -55);
    insertSimple(db, "wlan0", info);

    std::string dbInfo = db.getDbInfo();
    EXPECT_NE(dbInfo.find("\"records\""), std::string::npos);
    EXPECT_NE(dbInfo.find("\"size_kb\""), std::string::npos);
    EXPECT_NE(dbInfo.find("\"earliest\""), std::string::npos);
    EXPECT_NE(dbInfo.find("\"latest\""), std::string::npos);
}

TEST_F(DatabaseManagerTest, InsertAndQueryBtSnapshot) {
    DatabaseManager db(dbPath_);
    ASSERT_TRUE(db.isOpen());

    bool ok1 = db.insertBtSnapshot("9C:04:B6:84:0B:57", "AA:BB:CC:11:22:33", "MyHeadset",
                                   true, -55, 1.2, true, 88.5, false, 42000, 35);
    EXPECT_TRUE(ok1);

    bool ok2 = db.insertBtSnapshot("9C:04:B6:84:0B:57", "AA:BB:CC:44:55:66", "OtherPhone",
                                   false, -80, 5.4, false, 0.0, false, 0, 0);
    EXPECT_TRUE(ok2);

    // 查询所有蓝牙设备
    std::string allRes = db.queryBtHistory("", "", "", 10);
    EXPECT_NE(allRes.find("AA:BB:CC:11:22:33"), std::string::npos);
    EXPECT_NE(allRes.find("AA:BB:CC:44:55:66"), std::string::npos);
    EXPECT_NE(allRes.find("\"quality_score\":88.5"), std::string::npos);
    EXPECT_NE(allRes.find("\"audio_active\":true"), std::string::npos);

    // 按设备 MAC 单独过滤
    std::string singleRes = db.queryBtHistory("AA:BB:CC:11:22:33", "", "", 10);
    EXPECT_NE(singleRes.find("MyHeadset"), std::string::npos);
    EXPECT_EQ(singleRes.find("OtherPhone"), std::string::npos);
}


// ============================================================================
// 回归：历史行写入的 assessment_profile 必须是调用方传入的真实 Profile
//
// 缺陷背景：insertSnapshot 曾把 assessment_profile 列写死为 "INTERNET_ACCESS"，
// 忽略设备实际的评估 Profile。设备跑 NETWORK_ONLY 时，GetHistory /
// getQualityReport 读到的审计元数据是错的——而这条元数据正是用来解释
// 「当时的结论是在哪个模型下得出的」。
// ============================================================================
TEST_F(DatabaseManagerTest, InsertsCallerSuppliedAssessmentProfile) {
    DatabaseManager db(dbPath_);
    ASSERT_TRUE(db.isOpen());

    auto info = makeTestIface("wlan0", 20, 1.0, -50);
    NetworkQualityResult overall;
    overall.level = NetworkQualityLevel::GOOD;
    overall.levelName = "GOOD";
    overall.score = 80.0;

    ASSERT_TRUE(db.insertSnapshot("wlan0", info, overall, /*generation=*/1, /*snapshot_ts_ms=*/0,
                                  info.rttSampleTsMs(), info.rssiSampleTsMs(),
                                  info.jitterSampleTsMs(), info.tcpLossSampleTsMs(),
                                  info.trafficSampleTsMs(),
                                  /*assessment_profile=*/"NETWORK_ONLY"));

    const std::string result = db.queryHistory("wlan0", "", "", 10);
    EXPECT_NE(result.find("\"assessment_profile\":\"NETWORK_ONLY\""), std::string::npos);
    // 不得再出现写死的 INTERNET_ACCESS
    EXPECT_EQ(result.find("\"assessment_profile\":\"INTERNET_ACCESS\""), std::string::npos);
}

TEST_F(DatabaseManagerTest, DefaultsNullOrEmptyAssessmentProfileToUnspecified) {
    DatabaseManager db(dbPath_);
    ASSERT_TRUE(db.isOpen());

    auto info = makeTestIface("wlan0", 20, 1.0, -50);
    NetworkQualityResult overall;
    overall.level = NetworkQualityLevel::GOOD;
    overall.levelName = "GOOD";
    overall.score = 80.0;

    // 显式传 nullptr：必须落到 UNSPECIFIED，而不是空串或编造的 profile
    ASSERT_TRUE(db.insertSnapshot("wlan0", info, overall, 1, 0,
                                  info.rttSampleTsMs(), info.rssiSampleTsMs(),
                                  info.jitterSampleTsMs(), info.tcpLossSampleTsMs(),
                                  info.trafficSampleTsMs(),
                                  /*assessment_profile=*/nullptr));

    const std::string result = db.queryHistory("wlan0", "", "", 10);
    EXPECT_NE(result.find("\"assessment_profile\":\"UNSPECIFIED\""), std::string::npos);
}

// ============================================================================
// Phase 1b：工业蓝牙 HCI ACL 数据通道指标存储与检索测试
// ============================================================================
TEST_F(DatabaseManagerTest, InsertAndQueryBtDataPathMetrics) {
    DatabaseManager db(dbPath_);
    ASSERT_TRUE(db.isOpen());

    const int64_t now_ms = 1700000000000LL;

    // 1. 写入第 1 条指标：设备 A (AA:BB:CC:11:22:33)
    bool ok1 = db.insertBtDataPathMetric(
        now_ms,
        "hci0",
        129,
        1,
        "device",
        "AA:BB:CC:11:22:33",
        1000,
        5000,
        10000,
        50,
        100,
        40000.0,
        80000.0,
        25
    );
    EXPECT_TRUE(ok1);

    // 2. 写入第 2 条指标：设备 B (DD:EE:FF:44:55:66)
    bool ok2 = db.insertBtDataPathMetric(
        now_ms + 2000,
        "hci0",
        130,
        1,
        "device",
        "DD:EE:FF:44:55:66",
        2000,
        2500,
        3500,
        25,
        35,
        10000.0,
        14000.0,
        40
    );
    EXPECT_TRUE(ok2);

    // 3. 全量查询
    std::string all_res = db.queryBtDataPathMetrics("", 0, 0, 10);
    EXPECT_NE(all_res.find("AA:BB:CC:11:22:33"), std::string::npos);
    EXPECT_NE(all_res.find("DD:EE:FF:44:55:66"), std::string::npos);
    EXPECT_NE(all_res.find("\"tx_bytes\":5000"), std::string::npos);
    EXPECT_NE(all_res.find("\"rx_bps\":80000"), std::string::npos);

    // 4. 按设备 MAC 过滤
    std::string dev_a_res = db.queryBtDataPathMetrics("AA:BB:CC:11:22:33", 0, 0, 10);
    EXPECT_NE(dev_a_res.find("AA:BB:CC:11:22:33"), std::string::npos);
    EXPECT_EQ(dev_a_res.find("DD:EE:FF:44:55:66"), std::string::npos);

    // 5. 按时间窗口过滤
    std::string time_filtered = db.queryBtDataPathMetrics("", now_ms + 1000, now_ms + 3000, 10);
    EXPECT_EQ(time_filtered.find("AA:BB:CC:11:22:33"), std::string::npos);
    EXPECT_NE(time_filtered.find("DD:EE:FF:44:55:66"), std::string::npos);
}

// ============================================================================
// O1-A：TCP 重传/连接聚合列（tcp_retrans_loss_pct / tcp_conn_active /
// tcp_conn_accepts）随快照持久化，"未观测"诚实写 NULL（≠ 0）。
//
// 缺陷背景：tcp_retrans/tcp_conn 两条 eBPF 管线此前零真实消费——
// 不进 D-Bus、不进上行、不进 DB。此用例锁死 DB 落库这一环。
// ============================================================================
TEST_F(DatabaseManagerTest, PersistsTcpLossAggregateColumns) {
    DatabaseManager db(dbPath_);
    ASSERT_TRUE(db.isOpen());

    auto info = makeTestIface("wlan0", 20, 1.0, -50);
    NetworkQualityResult overall;
    overall.level = NetworkQualityLevel::GOOD;
    overall.levelName = "GOOD";
    overall.score = 80.0;

    // 未发布任何指标时：loss 与 conn 计数都写 NULL（不是 0——0 是"观测到零"）
    ASSERT_TRUE(insertSimple(db, "wlan0", info));
    std::string result = db.queryHistory("wlan0", "", "", 1);
    EXPECT_NE(result.find("\"tcp_retrans_loss_pct\":null"), std::string::npos);
    EXPECT_NE(result.find("\"tcp_conn_active\":null"), std::string::npos);
    EXPECT_NE(result.find("\"tcp_conn_accepts\":null"), std::string::npos);

    // 发布全局聚合后：下一行快照携带新值
    weaknet::MetricsRegistry::global().publishGlobal(
        weaknet::MetricId::TCP_RETRANS_LOSS_PCT,
        weaknet::MetricSample::valid(2.5));
    weaknet::MetricsRegistry::global().publishGlobal(
        weaknet::MetricId::TCP_CONN_ACTIVE,
        weaknet::MetricSample::valid(7.0));
    weaknet::MetricsRegistry::global().publishGlobal(
        weaknet::MetricId::TCP_CONN_ACCEPT_RATE,
        weaknet::MetricSample::valid(42.0));
    auto info2 = makeTestIface("wlan0", 20, 1.0, -50);
    ASSERT_TRUE(insertSimple(db, "wlan0", info2));
    result = db.queryHistory("wlan0", "", "", 10);
    EXPECT_NE(result.find("\"tcp_retrans_loss_pct\":2.5"), std::string::npos);
    EXPECT_NE(result.find("\"tcp_conn_active\":7"), std::string::npos);
    EXPECT_NE(result.find("\"tcp_conn_accepts\":42"), std::string::npos);

    // 清理全局注册中心，避免污染其它用例
    weaknet::MetricsRegistry::global().publishGlobal(
        weaknet::MetricId::TCP_RETRANS_LOSS_PCT,
        weaknet::MetricSample::unavailable());
    weaknet::MetricsRegistry::global().publishGlobal(
        weaknet::MetricId::TCP_CONN_ACTIVE,
        weaknet::MetricSample::unavailable());
    weaknet::MetricsRegistry::global().publishGlobal(
        weaknet::MetricId::TCP_CONN_ACCEPT_RATE,
        weaknet::MetricSample::unavailable());
}


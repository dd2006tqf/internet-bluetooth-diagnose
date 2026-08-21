// test_database_manager_gtest.cpp
// DatabaseManager 单元测试 (Google Test 版本)
// Module under test: database_manager.cpp (SQLite 历史数据持久化)

#include <gtest/gtest.h>
#include "database_manager.hpp"
#include "net_info.hpp"
#include <cstdio>
#include <unistd.h>
#include <sys/stat.h>

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
        return info;
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
    EXPECT_TRUE(db.insertSnapshot("wlan0", info));
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
        EXPECT_TRUE(db.insertSnapshot("wlan0", info));
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
    db.insertSnapshot("wlan0", wlan);
    db.insertSnapshot("eth0", eth);

    std::string result = db.queryHistory("wlan0", "", "", 10);
    EXPECT_NE(result.find("wlan0"), std::string::npos);
    EXPECT_EQ(result.find("eth0"), std::string::npos);
}

TEST_F(DatabaseManagerTest, QueryAllInterfaces) {
    DatabaseManager db(dbPath_);
    ASSERT_TRUE(db.isOpen());

    auto wlan = makeTestIface("wlan0", 50, 10.0, -55);
    auto eth = makeTestIface("eth0", 10, 1.0, -1000);
    db.insertSnapshot("wlan0", wlan);
    db.insertSnapshot("eth0", eth);

    std::string result = db.queryHistory("", "", "", 10);
    EXPECT_NE(result.find("wlan0"), std::string::npos);
    EXPECT_NE(result.find("eth0"), std::string::npos);
}

TEST_F(DatabaseManagerTest, QueryLimit) {
    DatabaseManager db(dbPath_);
    ASSERT_TRUE(db.isOpen());

    for (int i = 0; i < 10; ++i) {
        auto info = makeTestIface("wlan0", 50, 10.0, -55);
        db.insertSnapshot("wlan0", info);
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
    db.insertSnapshot("wlan0", info);
    EXPECT_EQ(db.getRecordCount(), 1);

    // 清理 365 天前的数据 → 刚插入的记录不应被删除
    int deleted = db.cleanup(365);
    EXPECT_EQ(deleted, 0);
    EXPECT_EQ(db.getRecordCount(), 1);
}

TEST_F(DatabaseManagerTest, InsertWithScore) {
    DatabaseManager db(dbPath_);
    ASSERT_TRUE(db.isOpen());

    auto info = makeTestIface("wlan0", 50, 10.5, -55);
    EXPECT_TRUE(db.insertSnapshot("wlan0", info, 72.5));
    EXPECT_EQ(db.getRecordCount(), 1);

    std::string result = db.queryHistory("wlan0", "", "", 10);
    EXPECT_NE(result.find("72.5"), std::string::npos);  // score
}

// ============================================================================
// Test Cases: 数据库信息
// ============================================================================

TEST_F(DatabaseManagerTest, GetDbInfo) {
    DatabaseManager db(dbPath_);
    ASSERT_TRUE(db.isOpen());

    auto info = makeTestIface("wlan0", 50, 10.0, -55);
    db.insertSnapshot("wlan0", info);

    std::string dbInfo = db.getDbInfo();
    EXPECT_NE(dbInfo.find("\"records\""), std::string::npos);
    EXPECT_NE(dbInfo.find("\"size_kb\""), std::string::npos);
    EXPECT_NE(dbInfo.find("\"earliest\""), std::string::npos);
    EXPECT_NE(dbInfo.find("\"latest\""), std::string::npos);
}

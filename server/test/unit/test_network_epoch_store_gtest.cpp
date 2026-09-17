/**
 * @file test_network_epoch_store_gtest.cpp
 * @brief 网络代次（network epoch）跨重启持久化测试
 *
 * ## 这个测试要防的是什么
 *
 * 服务端按 ``(tenant, device, network_epoch, sequence_id)`` 幂等入库。
 * 板端 ``sequence_id`` 是进程内计数器，每次重启都从 0 重新计到 1、2、3…；
 * 若 ``network_epoch`` 也每次重启都回到 1，那么重启后发出的
 * ``(1, 1)``、``(1, 2)``… 会与上一轮运行留下的历史键**完全重合**。
 * 服务端判定为重复，返回 HTTP 200 但丢弃数据 —— 表现为"上报正常但库里
 * 没有新数据"，是最难从日志里看出的一类静默丢失。
 *
 * 因此本测试的核心断言是：**重启（重新构造）后代次必须严格递增**，
 * 从而保证幂等键落在全新的命名空间里。
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "network_epoch_store.hpp"

namespace {

/**
 * @brief 每个用例独占一个临时目录，避免用例间相互影响。
 *
 * 用目录而非单文件，是为了让 store 的"文件不存在"分支和"目录不存在"
 * 分支都能被独立覆盖。
 */
class NetworkEpochStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* testName =
            ::testing::UnitTest::GetInstance()->current_test_info()->name();
        dir_ = "/tmp/weaknet_epoch_test_" + std::to_string(getpid()) + "_" + testName;
        // 清理上一次运行残留
        removeTree(dir_);
        ASSERT_EQ(::mkdir(dir_.c_str(), 0700), 0);
    }

    void TearDown() override { removeTree(dir_); }

    static void removeTree(const std::string& dir) {
        ::unlink((dir + "/network-epoch").c_str());
        ::rmdir(dir.c_str());
    }

    /// 让 store 使用本用例的临时目录
    std::string statePath() const { return dir_ + "/network-epoch"; }

    std::string dir_;
};

// ---------------------------------------------------------------------------
// 首次运行
// ---------------------------------------------------------------------------

TEST_F(NetworkEpochStoreTest, FirstRunStartsAtOne) {
    weaknet::NetworkEpochStore store(statePath());
    // 没有历史状态文件时，第一次打开应当返回 1：与既有部署的
    // (epoch=1, seq=N) 历史数据保持一致，不制造一次性的键空间跳变。
    EXPECT_EQ(store.open(), 1u);
}

TEST_F(NetworkEpochStoreTest, PersistsStateFileOnOpen) {
    weaknet::NetworkEpochStore store(statePath());
    store.open();

    struct stat st {};
    ASSERT_EQ(::stat(statePath().c_str(), &st), 0) << "状态文件必须在 open 后落盘";
    EXPECT_GT(st.st_size, 0);
}

// ---------------------------------------------------------------------------
// 核心：重启必须推进代次
// ---------------------------------------------------------------------------

TEST_F(NetworkEpochStoreTest, RestartAdvancesEpoch) {
    uint64_t first = 0;
    {
        weaknet::NetworkEpochStore store(statePath());
        first = store.open();
    }
    // 模拟进程重启：重新构造一个 store，读取同一状态文件
    weaknet::NetworkEpochStore restarted(statePath());
    const uint64_t second = restarted.open();

    // 这是本文件唯一不可妥协的断言：重启后代次必须严格更大。
    // 若相等，重启后的上行数据会全部撞上历史幂等键并被静默丢弃。
    EXPECT_GT(second, first);
}

TEST_F(NetworkEpochStoreTest, EpochAdvancesAcrossManyRestarts) {
    uint64_t previous = 0;
    for (int i = 0; i < 5; ++i) {
        weaknet::NetworkEpochStore store(statePath());
        const uint64_t epoch = store.open();
        EXPECT_GT(epoch, previous) << "第 " << (i + 1) << " 次重启代次未推进";
        previous = epoch;
    }
    EXPECT_EQ(previous, 5u) << "每次重启恰好推进 1";
}

// ---------------------------------------------------------------------------
// 边界与容错
// ---------------------------------------------------------------------------

TEST_F(NetworkEpochStoreTest, CorruptStateFileDoesNotReuseEpoch) {
    // 写入非法内容（模拟掉电写坏 / 人为篡改）
    FILE* fp = std::fopen(statePath().c_str(), "w");
    ASSERT_NE(fp, nullptr);
    std::fputs("not-a-number", fp);
    std::fclose(fp);

    weaknet::NetworkEpochStore store(statePath());
    const uint64_t epoch = store.open();

    // 损坏时绝不能返回一个可能已被用过的值（尤其不能是 1，因为
    // epoch=1 的历史数据一定存在）。从高位重新开始是安全的选择：
    // 代价只是键空间出现一次跳变，而错误复用会导致数据丢失。
    EXPECT_GT(epoch, 1u) << "状态损坏时不得回落到可能已用过的代次";
}

TEST_F(NetworkEpochStoreTest, ZeroOrNegativeStateIsRejected) {
    for (const char* bad : {"0", "-3"}) {
        FILE* fp = std::fopen(statePath().c_str(), "w");
        ASSERT_NE(fp, nullptr);
        std::fputs(bad, fp);
        std::fclose(fp);

        weaknet::NetworkEpochStore store(statePath());
        EXPECT_GT(store.open(), 1u) << "非法值 " << bad << " 不得被当作有效代次";
    }
}

TEST_F(NetworkEpochStoreTest, MissingDirectoryDoesNotCrashAndStillAdvances) {
    // 目录不存在（例如 data_dir 尚未创建）：open() 必须仍返回可用值，
    // 且不得因为无法写盘就把代次退回。
    const std::string missing = dir_ + "/does-not-exist/network-epoch";
    weaknet::NetworkEpochStore store(missing);
    const uint64_t epoch = store.open();
    EXPECT_GT(epoch, 1u);
}

}  // namespace

/**
 * @file test_snapshot_retention_gtest.cpp
 * @brief 板端静帧容量回收契约测试（无摄像头、无 VE、无网络）
 *
 * ============================================================================
 * 这份测试针对的是什么
 * ============================================================================
 *
 * 回收是**不可逆的本地证据销毁**。它比云端保留策略更危险一点：板子离线时，
 * 本地这份是唯一的副本，而云端一条记录都没有。所以这里断言的全部是
 * 「什么时候不该删」，而不是「能不能删」：
 *
 *   1. **默认关闭**：`max_total_bytes == 0` 时一个字节都不动。没人复核过的
 *      数字不该开始删证据。
 *   2. **不删刚写的那张**：上限配得比一张图还小时，「删到装得下」的唯一解法
 *      是删掉刚拍的图——事故发生时拍一张、立刻消失。
 *   3. **时钟不可信也能用**：顺序判据只看文件名里的毫秒数。时钟整体偏移时
 *      相对顺序仍然正确，而时钟跳变不会让「谁更旧」变成「全删」。
 *   4. **`.jpg.tmp` 也算**：原子写崩在 rename 之前会留下半成品，它永远不会
 *      被改回正式文件，比正式文件更该先清掉。
 *   5. **失败不静默**：删不掉要计数，不许假装成功。
 *
 * 所有用例都在 `mkdtemp` 出来的临时目录里跑，不碰真实数据。
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "snapshot_retention.hpp"

namespace weaknet {
namespace {

/// 临时目录，析构时递归删除。
class TempDir {
public:
    TempDir() {
        char tmpl[] = "/tmp/snapshot-retention-XXXXXX";
        const char* made = ::mkdtemp(tmpl);
        if (made != nullptr) root_ = made;
    }
    ~TempDir() {
        if (!root_.empty()) removeTree(root_);
    }
    const std::string& path() const { return root_; }

    std::string mkdir(const std::string& name) const {
        const std::string p = root_ + "/" + name;
        ::mkdir(p.c_str(), 0755);
        return p;
    }

    /// 写一个大小为 bytes 的文件。返回完整路径。
    std::string writeFile(const std::string& incident, const std::string& name,
                          size_t bytes) const {
        const std::string dir = root_ + "/" + incident;
        ::mkdir(dir.c_str(), 0755);
        const std::string p = dir + "/" + name;
        std::ofstream out(p, std::ios::binary);
        std::string filler(bytes, 'x');
        out.write(filler.data(), static_cast<std::streamsize>(filler.size()));
        return p;
    }

private:
    static void removeTree(const std::string& path) {
        DIR* d = ::opendir(path.c_str());
        if (d != nullptr) {
            struct dirent* e = nullptr;
            while ((e = ::readdir(d)) != nullptr) {
                const std::string name = e->d_name;
                if (name == "." || name == "..") continue;
                removeTree(path + "/" + name);
            }
            ::closedir(d);
        }
        ::unlink(path.c_str());
        ::rmdir(path.c_str());
    }

    std::string root_;
};

bool exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

size_t countFiles(const std::string& root) {
    std::vector<SnapshotFileEntry> files;
    scanSnapshotFiles(root, &files);
    return files.size();
}

// ===========================================================================
// 1. 扫描：只认自己写的东西，且顺序确定
// ===========================================================================

TEST(SnapshotScan, MissingRootIsNotAnError) {
    // 还没拍过任何一张时目录可能不存在。这不是错误，是常态。
    std::vector<SnapshotFileEntry> files;
    EXPECT_TRUE(scanSnapshotFiles("/nonexistent/snapshot/path", &files));
    EXPECT_TRUE(files.empty());
}

TEST(SnapshotScan, FindsFilesTwoLevelsDeep) {
    TempDir dir;
    dir.writeFile("incident-a", "1000_1.jpg", 100);
    dir.writeFile("incident-b", "2000_2.jpg", 200);

    std::vector<SnapshotFileEntry> files;
    ASSERT_TRUE(scanSnapshotFiles(dir.path(), &files));
    ASSERT_EQ(files.size(), 2u);

    // 按毫秒升序
    EXPECT_EQ(files[0].name_ms, 1000u);
    EXPECT_EQ(files[1].name_ms, 2000u);
    EXPECT_EQ(files[0].size_bytes, 100u);
}

TEST(SnapshotScan, SortedByTimestampNotReaddirOrder) {
    // readdir 顺序由文件系统决定。不排序会让「哪张证据先被删」在不同板子上
    // 不同——而回收顺序直接决定删哪一张。
    TempDir dir;
    dir.writeFile("inc-a", "3000_1.jpg", 10);
    dir.writeFile("inc-b", "1000_1.jpg", 10);
    dir.writeFile("inc-c", "2000_1.jpg", 10);

    std::vector<SnapshotFileEntry> files;
    ASSERT_TRUE(scanSnapshotFiles(dir.path(), &files));
    ASSERT_EQ(files.size(), 3u);
    EXPECT_EQ(files[0].name_ms, 1000u);
    EXPECT_EQ(files[1].name_ms, 2000u);
    EXPECT_EQ(files[2].name_ms, 3000u);
}

TEST(SnapshotScan, CountsTmpFilesAsGarbage) {
    // 原子写崩在 rename 之前会留下 .jpg.tmp。它永远不会被改回正式文件
    // （rename 只有写入线程会做，而那次写入已经失败了），比正式文件更该先清。
    TempDir dir;
    dir.writeFile("inc-a", "1000_1.jpg", 10);
    dir.writeFile("inc-a", "2000_1.jpg.tmp", 10);

    std::vector<SnapshotFileEntry> files;
    ASSERT_TRUE(scanSnapshotFiles(dir.path(), &files));
    EXPECT_EQ(files.size(), 2u);
}

TEST(SnapshotScan, IgnoresNonSnapshotFilesAndRootFiles) {
    // 根目录下的散文件不是本模块写的（或是一次失败留下的）。不删它：
    // 回收只负责自己那块盘的占用，不清理别人的东西。
    TempDir dir;
    dir.writeFile("inc-a", "1000_1.jpg", 10);
    {
        std::ofstream out(dir.path() + "/stray.txt");
        out << "not ours";
    }
    dir.writeFile("inc-a", "notes.txt", 10);

    std::vector<SnapshotFileEntry> files;
    ASSERT_TRUE(scanSnapshotFiles(dir.path(), &files));
    ASSERT_EQ(files.size(), 1u);
    EXPECT_EQ(files[0].name_ms, 1000u);
}

TEST(SnapshotScan, ParsesTheRealWallClockMillisecondForm) {
    // 钉住**板端实测的真实文件名形态**：13 位墙钟毫秒 + 下划线 + 帧序号。
    // 上面的用例都用 1000/2000 这种 4 位数，而真实值是 1795155756003——
    // 位数不同会让任何"按位数判断"的实现（比如假定 10 位）在这里露馅，
    // 而它露馅的方式是「所有文件 ms 都解析成 0 → 按字典序删 → 删错图」。
    TempDir dir;
    dir.writeFile("sitinc_radxa_cubie_a7a_radxa_cubie_a7a_1795155743913",
                  "1795155756003_2133.jpg", 118332);
    dir.writeFile("sitinc_radxa_cubie_a7a_radxa_cubie_a7a_1795158465933",
                  "1795158478700_610.jpg", 127895);
    dir.writeFile("sitinc_radxa_cubie_a7a_radxa_cubie_a7a_1795230092765",
                  "1795230105185_7478.jpg", 119573);

    std::vector<SnapshotFileEntry> files;
    ASSERT_TRUE(scanSnapshotFiles(dir.path(), &files));
    ASSERT_EQ(files.size(), 3u);
    EXPECT_EQ(files[0].name_ms, 1795155756003ull);
    EXPECT_EQ(files[1].name_ms, 1795158478700ull);
    EXPECT_EQ(files[2].name_ms, 1795230105185ull);
    EXPECT_EQ(files[0].size_bytes, 118332u) << "真实 JPEG 大小必须原样读到";
    EXPECT_EQ(files[0].incident,
              "sitinc_radxa_cubie_a7a_radxa_cubie_a7a_1795155743913")
        << "事故目录名要原样带出来（删空后要靠它 rmdir）";
}

TEST(SnapshotScan, UnparsableNamesSortFirst) {
    // 解析不出毫秒的文件排最前（ms=0）：宁可先删来历不明的东西。
    TempDir dir;
    dir.writeFile("inc-a", "garbage.jpg", 10);
    dir.writeFile("inc-a", "5000_1.jpg", 10);

    std::vector<SnapshotFileEntry> files;
    ASSERT_TRUE(scanSnapshotFiles(dir.path(), &files));
    ASSERT_EQ(files.size(), 2u);
    EXPECT_EQ(files[0].name_ms, 0u);
    EXPECT_EQ(files[1].name_ms, 5000u);
}

// ===========================================================================
// 2. 默认关闭：一个字节都不动
// ===========================================================================

TEST(SnapshotRetention, ZeroLimitDisablesPruningEntirely) {
    // 默认值。0 不是「上限为零」，是「不回收」。
    TempDir dir;
    dir.writeFile("inc-a", "1000_1.jpg", 1000);

    SnapshotRetentionConfig config;  // max_total_bytes = 0
    SnapshotRetentionResult result;
    const size_t deleted = pruneSnapshotDir(dir.path(), config, "", &result);

    EXPECT_EQ(deleted, 0u);
    EXPECT_EQ(countFiles(dir.path()), 1u);
    EXPECT_FALSE(result.still_over_limit);
}

TEST(SnapshotRetention, UnderLimitDeletesNothing) {
    TempDir dir;
    dir.writeFile("inc-a", "1000_1.jpg", 100);
    dir.writeFile("inc-a", "2000_1.jpg", 100);

    SnapshotRetentionConfig config;
    config.max_total_bytes = 10'000;
    SnapshotRetentionResult result;
    const size_t deleted = pruneSnapshotDir(dir.path(), config, "", &result);

    EXPECT_EQ(deleted, 0u);
    EXPECT_EQ(result.bytes_before, 200u);
    EXPECT_EQ(result.bytes_after, 200u);
    EXPECT_EQ(countFiles(dir.path()), 2u);
}

TEST(SnapshotRetention, ExactlyAtLimitDeletesNothing) {
    // **边界：等于上限不算超。** 写成 `<` 会让刚好的部署每次多删一张。
    TempDir dir;
    dir.writeFile("inc-a", "1000_1.jpg", 100);
    dir.writeFile("inc-a", "2000_1.jpg", 100);

    SnapshotRetentionConfig config;
    config.max_total_bytes = 200;
    SnapshotRetentionResult result;
    EXPECT_EQ(pruneSnapshotDir(dir.path(), config, "", &result), 0u);
    EXPECT_EQ(countFiles(dir.path()), 2u);
}

// ===========================================================================
// 3. 超了之后：从最旧的删，绝不删刚写的那张
// ===========================================================================

TEST(SnapshotRetention, DeletesOldestFirst) {
    TempDir dir;
    const std::string oldest = dir.writeFile("inc-a", "1000_1.jpg", 100);
    const std::string middle = dir.writeFile("inc-b", "2000_1.jpg", 100);
    const std::string newest = dir.writeFile("inc-c", "3000_1.jpg", 100);

    SnapshotRetentionConfig config;
    config.max_total_bytes = 200;  // 只装得下两张
    SnapshotRetentionResult result;
    const size_t deleted = pruneSnapshotDir(dir.path(), config, "", &result);

    EXPECT_EQ(deleted, 1u);
    EXPECT_FALSE(exists(oldest)) << "最旧的必须被删";
    EXPECT_TRUE(exists(middle));
    EXPECT_TRUE(exists(newest));
    EXPECT_EQ(result.bytes_freed, 100u);
    EXPECT_FALSE(result.still_over_limit);
}

TEST(SnapshotRetention, DeletesMultipleWhenFarOverLimit) {
    TempDir dir;
    for (int i = 0; i < 5; ++i) {
        dir.writeFile("inc", std::to_string(1000 * (i + 1)) + "_1.jpg", 100);
    }

    SnapshotRetentionConfig config;
    config.max_total_bytes = 200;  // 5 张 500 字节 → 只留 2 张
    SnapshotRetentionResult result;
    const size_t deleted = pruneSnapshotDir(dir.path(), config, "", &result);

    EXPECT_EQ(deleted, 3u);
    EXPECT_EQ(countFiles(dir.path()), 2u);
    EXPECT_EQ(result.bytes_after, 200u);
}

TEST(SnapshotRetention, NeverDeletesTheJustWrittenFile) {
    // **最关键的一条。** keep 的那张即使是最旧的也不能删。
    TempDir dir;
    const std::string keep = dir.writeFile("inc-a", "1000_1.jpg", 100);
    const std::string other = dir.writeFile("inc-b", "2000_1.jpg", 100);

    SnapshotRetentionConfig config;
    config.max_total_bytes = 1;  // 比一张图还小
    SnapshotRetentionResult result;
    const size_t deleted = pruneSnapshotDir(dir.path(), config, keep, &result);

    EXPECT_TRUE(exists(keep)) << "刚写的那张被删掉了：设备看到成功，图却立刻消失";
    EXPECT_FALSE(exists(other));
    EXPECT_EQ(deleted, 1u);
    // 删完仍然超上限（keep 那张撑着），必须如实报出来。
    EXPECT_TRUE(result.still_over_limit);
}

TEST(SnapshotRetention, OnlyTheKeptFileStaysWhenItIsTheOnlyOne) {
    // 上限比一张图还小、且只有这一张：保留并让 still_over_limit 可见，
    // 让配置错误被人看见，而不是静默删掉唯一一份证据。
    TempDir dir;
    const std::string only = dir.writeFile("inc-a", "1000_1.jpg", 5000);

    SnapshotRetentionConfig config;
    config.max_total_bytes = 1;
    SnapshotRetentionResult result;
    EXPECT_EQ(pruneSnapshotDir(dir.path(), config, only, &result), 0u);
    EXPECT_TRUE(exists(only));
    EXPECT_TRUE(result.still_over_limit);
}

TEST(SnapshotRetention, ClockSkewKeepsRelativeOrderUsable) {
    // 板子没有 RTC。若所有文件名都被写成 1970 年（ms=0），相对顺序虽然
    // 退化成字典序，但**不会**变成「全删」——上限判据只问占了多少字节。
    TempDir dir;
    dir.writeFile("inc-a", "0_1.jpg", 100);
    dir.writeFile("inc-b", "0_2.jpg", 100);
    dir.writeFile("inc-c", "0_3.jpg", 100);

    SnapshotRetentionConfig config;
    config.max_total_bytes = 200;
    SnapshotRetentionResult result;
    EXPECT_EQ(pruneSnapshotDir(dir.path(), config, "", &result), 1u);
    EXPECT_EQ(countFiles(dir.path()), 2u);
    EXPECT_FALSE(result.still_over_limit);
}

// ===========================================================================
// 4. 单次预算与失败计数
// ===========================================================================

TEST(SnapshotRetention, RespectsPerRunFileBudget) {
    // 上限误配成 1 字节时，一次扫空整张目录会让事故线程卡很久。
    TempDir dir;
    for (int i = 0; i < 10; ++i) {
        dir.writeFile("inc", std::to_string(1000 * (i + 1)) + "_1.jpg", 100);
    }

    SnapshotRetentionConfig config;
    config.max_total_bytes = 1;
    config.max_files_per_run = 3;
    SnapshotRetentionResult result;
    EXPECT_EQ(pruneSnapshotDir(dir.path(), config, "", &result), 3u);
    EXPECT_EQ(countFiles(dir.path()), 7u);
    // 预算用尽 ≠ 完成：还有剩余就必须如实说，下一轮接着删。
    EXPECT_TRUE(result.still_over_limit);
}

TEST(SnapshotRetention, ZeroBudgetMeansUnlimited) {
    TempDir dir;
    for (int i = 0; i < 4; ++i) {
        dir.writeFile("inc", std::to_string(1000 * (i + 1)) + "_1.jpg", 100);
    }

    SnapshotRetentionConfig config;
    config.max_total_bytes = 1;
    config.max_files_per_run = 0;  // 不限
    SnapshotRetentionResult result;
    EXPECT_EQ(pruneSnapshotDir(dir.path(), config, "", &result), 4u);
    EXPECT_EQ(countFiles(dir.path()), 0u);
}

TEST(SnapshotRetention, RemovesEmptiedIncidentDirectories) {
    // 事故目录是整段取证拷走的单位。文件删空后留着它只会让目录数无限增长，
    // 而 du 看起来「还好」。
    TempDir dir;
    const std::string emptied = dir.mkdir("incident-old");
    dir.writeFile("incident-old", "1000_1.jpg", 100);
    dir.writeFile("incident-new", "9000_1.jpg", 100);

    SnapshotRetentionConfig config;
    config.max_total_bytes = 100;
    SnapshotRetentionResult result;
    pruneSnapshotDir(dir.path(), config, "", &result);

    EXPECT_FALSE(exists(emptied)) << "空的事故目录应当被删掉";
    EXPECT_TRUE(exists(dir.path() + "/incident-new"));
    EXPECT_EQ(result.dirs_removed, 1u);
}

TEST(SnapshotRetention, KeepsDirectoryThatStillHasOtherFiles) {
    TempDir dir;
    dir.writeFile("incident-a", "1000_1.jpg", 100);
    dir.writeFile("incident-a", "9000_1.jpg", 100);

    SnapshotRetentionConfig config;
    config.max_total_bytes = 100;
    SnapshotRetentionResult result;
    pruneSnapshotDir(dir.path(), config, "", &result);

    EXPECT_TRUE(exists(dir.path() + "/incident-a"));
    EXPECT_EQ(result.dirs_removed, 0u);
}

TEST(SnapshotRetention, TmpFileIsDeletedBeforeRealOne) {
    // .tmp 的 ms 解析出来和正式文件一样（前缀相同），但字典序里
    // "x.jpg.tmp" < ... 不成立。这里验证的是：只要超限，.tmp 会被清掉，
    // 不会因为「它不是正式图」而被跳过。
    TempDir dir;
    const std::string tmp = dir.writeFile("inc-a", "1000_1.jpg.tmp", 100);
    const std::string real = dir.writeFile("inc-b", "2000_1.jpg", 100);

    SnapshotRetentionConfig config;
    config.max_total_bytes = 100;
    SnapshotRetentionResult result;
    EXPECT_EQ(pruneSnapshotDir(dir.path(), config, "", &result), 1u);
    EXPECT_FALSE(exists(tmp));
    EXPECT_TRUE(exists(real));
}

TEST(SnapshotRetention, EmptyRootIsSafe) {
    TempDir dir;
    SnapshotRetentionConfig config;
    config.max_total_bytes = 1;
    SnapshotRetentionResult result;
    EXPECT_EQ(pruneSnapshotDir(dir.path(), config, "", &result), 0u);
    EXPECT_TRUE(scanSnapshotFiles(dir.path(), nullptr) == false)
        << "out 为空必须返回 false，而不是崩溃";
}

TEST(SnapshotRetention, ResultIsResetOnEveryRun) {
    // 结果结构是复用的：上一轮的计数不能泄漏到这一轮，否则日志会撒谎。
    TempDir dir;
    dir.writeFile("inc-a", "1000_1.jpg", 1000);

    SnapshotRetentionConfig config;
    config.max_total_bytes = 100;
    SnapshotRetentionResult result;
    result.files_deleted = 999;  // 脏数据
    result.bytes_freed = 999;

    pruneSnapshotDir(dir.path(), config, "", &result);
    EXPECT_EQ(result.files_deleted, 1u);
    EXPECT_EQ(result.bytes_freed, 1000u);
}

}  // namespace
}  // namespace weaknet

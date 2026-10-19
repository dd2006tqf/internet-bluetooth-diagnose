// test_update_status_gtest.cpp
// OTA Phase 1 本地升级：版本比较 + manifest 校验（RED：先红后绿）
//
// Module under test: server/src/update_status.cpp（待创建）
// 语义：
//   - 版本格式 X.Y.Z，数字分段比较（1.10.0 > 1.2.0，字典序会错）
//   - apply 允许: 同版本幂等、无版本（首次）装任意版、升级到更高版
//   - apply 拒绝: 降级到更低版（防回滚攻击的本地版）
//   - manifest: 文件→sha256 全量校验，任一缺失/不匹配即失败

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <string>

#include "update_status.hpp"

TEST(UpdateStatusVersion, RejectsMalformed) {
    EXPECT_FALSE(weaknet::update::IsValidVersion(""));
    EXPECT_FALSE(weaknet::update::IsValidVersion("1.0"));
    EXPECT_FALSE(weaknet::update::IsValidVersion("v1.0.0"));
    EXPECT_FALSE(weaknet::update::IsValidVersion("1.0.0-rc1"));
    EXPECT_FALSE(weaknet::update::IsValidVersion("a.b.c"));
    EXPECT_FALSE(weaknet::update::IsValidVersion("1.0.0.0"));
    EXPECT_TRUE(weaknet::update::IsValidVersion("1.0.0"));
    EXPECT_TRUE(weaknet::update::IsValidVersion("0.0.1"));
    EXPECT_TRUE(weaknet::update::IsValidVersion("10.20.30"));
}

TEST(UpdateStatusVersion, NumericNotLexicographic) {
    // 字典序下 "1.10.0" < "1.2.0"，数字比较必须反过来
    EXPECT_GT(weaknet::update::CompareVersions("1.10.0", "1.2.0"), 0);
    EXPECT_LT(weaknet::update::CompareVersions("1.2.0", "1.10.0"), 0);
    EXPECT_EQ(weaknet::update::CompareVersions("1.0.0", "1.0.0"), 0);
    EXPECT_GT(weaknet::update::CompareVersions("2.0.0", "1.99.99"), 0);
    EXPECT_GT(weaknet::update::CompareVersions("0.0.2", "0.0.1"), 0);
}

TEST(UpdateStatusUpgrade, PolicyMatrix) {
    // 首次安装：任意版本都允许
    EXPECT_TRUE(weaknet::update::IsUpgradeAllowed("", "1.0.0"));
    // 同版本：幂等允许
    EXPECT_TRUE(weaknet::update::IsUpgradeAllowed("1.0.0", "1.0.0"));
    // 升级：允许
    EXPECT_TRUE(weaknet::update::IsUpgradeAllowed("1.0.0", "1.1.0"));
    EXPECT_TRUE(weaknet::update::IsUpgradeAllowed("1.9.9", "2.0.0"));
    // 降级：拒绝（防回滚攻击的本地版）
    EXPECT_FALSE(weaknet::update::IsUpgradeAllowed("1.1.0", "1.0.0"));
    EXPECT_FALSE(weaknet::update::IsUpgradeAllowed("2.0.0", "1.99.99"));
    // 非法目标版本：拒绝
    EXPECT_FALSE(weaknet::update::IsUpgradeAllowed("1.0.0", "not-a-version"));
}

TEST(UpdateStatusManifest, ParseLine) {
    std::string sha, path;
    EXPECT_TRUE(weaknet::update::ParseManifestLine(
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  server/bin/weaknet-dbus-server",
        &sha, &path));
    EXPECT_EQ(sha, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(path, "server/bin/weaknet-dbus-server");
    // 空行与注释行拒绝
    EXPECT_FALSE(weaknet::update::ParseManifestLine("", &sha, &path));
    EXPECT_FALSE(weaknet::update::ParseManifestLine("# comment", &sha, &path));
    // sha 长度不对拒绝
    EXPECT_FALSE(weaknet::update::ParseManifestLine("abc  some/file", &sha, &path));
    // 路径穿越拒绝（manifest 不得写 releases/ 之外）
    EXPECT_FALSE(weaknet::update::ParseManifestLine(
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  ../evil", &sha, &path));
    EXPECT_FALSE(weaknet::update::ParseManifestLine(
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  /abs/path", &sha, &path));
}

// ---- 运行时只读查询（FileIoTest：临时目录，无真机依赖）----

namespace {

class FileIoTest : public ::testing::Test {
protected:
    void SetUp() override {
        base_ = mkdtemp(tpl);
        ASSERT_FALSE(base_.empty());
    }
    void TearDown() override {
        if (!base_.empty()) {
            std::string cmd = "rm -rf '" + base_ + "'";
            ASSERT_EQ(std::system(cmd.c_str()), 0);
        }
    }
    void WriteFile(const std::string& rel, const std::string& content) {
        std::string full = base_ + "/" + rel;
        std::string cmd = "mkdir -p $(dirname '" + full + "')";
        ASSERT_EQ(std::system(cmd.c_str()), 0);
        std::ofstream out(full);
        out << content;
    }
    std::string base_;
    char tpl[64] = "/tmp/weaknet-update-test-XXXXXX";
};

}  // namespace

TEST_F(FileIoTest, ReadCurrentVersionNoLink) {
    // 无 current 软链接（未迁移布局）→ 空串
    EXPECT_TRUE(weaknet::update::ReadCurrentVersion(base_).empty());
}

TEST_F(FileIoTest, ReadCurrentVersionFromLink) {
    WriteFile("releases/1.2.3/.committed", "");
    ASSERT_EQ(std::system(("cd '" + base_ + "' && ln -sfn releases/1.2.3 current").c_str()), 0);
    EXPECT_EQ(weaknet::update::ReadCurrentVersion(base_), "1.2.3");
}

TEST_F(FileIoTest, ReadCurrentVersionRejectsNonVersionTarget) {
    // current 指向非版本目录（人为破坏）→ 当作空，不当合法版本
    WriteFile("releases/not-a-version/x", "");
    ASSERT_EQ(std::system(("cd '" + base_ + "' && ln -sfn releases/not-a-version current").c_str()), 0);
    EXPECT_TRUE(weaknet::update::ReadCurrentVersion(base_).empty());
}

TEST_F(FileIoTest, StatusJsonFlatLayout) {
    const std::string json = weaknet::update::StatusJson(base_);
    EXPECT_NE(json.find("\"installed_version\":\"unmanaged\""), std::string::npos);
    EXPECT_NE(json.find("\"layout\":\"flat\""), std::string::npos);
    EXPECT_NE(json.find("\"current_committed\":false"), std::string::npos);
}

TEST_F(FileIoTest, StatusJsonReleasesLayoutCommitted) {
    WriteFile("releases/1.0.0/.committed", "");
    ASSERT_EQ(std::system(("cd '" + base_ + "' && ln -sfn releases/1.0.0 current").c_str()), 0);
    const std::string json = weaknet::update::StatusJson(base_);
    EXPECT_NE(json.find("\"installed_version\":\"1.0.0\""), std::string::npos);
    EXPECT_NE(json.find("\"layout\":\"releases\""), std::string::npos);
    EXPECT_NE(json.find("\"current_committed\":true"), std::string::npos);
}

TEST_F(FileIoTest, ReleaseVersionFile) {
    // 版本单一来源是 release 目录里的 VERSION 文件；缺失返回空
    EXPECT_TRUE(weaknet::update::ReadReleaseVersion(base_ + "/releases/9.9.9").empty());
    WriteFile("releases/9.9.9/VERSION", "2.5.1\n");
    EXPECT_EQ(weaknet::update::ReadReleaseVersion(base_ + "/releases/9.9.9"), "2.5.1");
    // 非法内容返回空（防手工塞入垃圾）
    WriteFile("releases/9.9.9/VERSION", "banana\n");
    EXPECT_TRUE(weaknet::update::ReadReleaseVersion(base_ + "/releases/9.9.9").empty());
}

TEST_F(FileIoTest, VerifyReleaseIntegrity) {
    // 三态：
    //   1) 无 manifest.sha256 → checked==0（调用方视为“未定标”，不判失败）
    //   2) 有清单且全匹配 → failed==0
    //   3) 有清单但文件被篡改 → failed>0
    const auto rep_none = weaknet::update::VerifyReleaseIntegrity(base_ + "/rel");
    EXPECT_EQ(rep_none.checked, 0);
    EXPECT_EQ(rep_none.failed, 0);

    WriteFile("rel/VERSION", "1.0.0\n");
    WriteFile("rel/payload.bin", "hello-weaknet");
    WriteFile("rel/manifest.sha256",
              "59854984853104df5c353e2f681a15fc7924742f9a2e468c29af248dce45ce03  VERSION\n"
              "ff219ea5d416c9cd53361967320c752ce0814432a0dd7654d03e6507a376cc23  payload.bin\n");
    const auto rep_ok = weaknet::update::VerifyReleaseIntegrity(base_ + "/rel");
    if (rep_ok.capability == weaknet::update::IntegrityReport::OK) {
        EXPECT_EQ(rep_ok.checked, 2);
        EXPECT_EQ(rep_ok.failed, 0);
        // 篡改 payload → 计入 failed，但 VERSION 仍匹配
        WriteFile("rel/payload.bin", "tampered");
        const auto rep_bad = weaknet::update::VerifyReleaseIntegrity(base_ + "/rel");
        EXPECT_EQ(rep_bad.checked, 2);
        EXPECT_EQ(rep_bad.failed, 1);
        // 清单列出但文件缺失 → 也计 failed
        ASSERT_EQ(std::system(("rm -f '" + base_ + "/rel/payload.bin'").c_str()), 0);
        const auto rep_missing = weaknet::update::VerifyReleaseIntegrity(base_ + "/rel");
        EXPECT_EQ(rep_missing.failed, 1);
    } else {
        // 无 OpenSSL 构建：退化为未定标，不得误报 failed
        EXPECT_EQ(rep_ok.capability, weaknet::update::IntegrityReport::NO_CAPABILITY);
        EXPECT_EQ(rep_ok.failed, 0);
    }
}

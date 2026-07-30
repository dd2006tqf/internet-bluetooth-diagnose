// test_iface_type.cpp
// Wi-Fi 接口类型识别单元测试
// 被测模块: iface_type.cpp（isWirelessInterface 函数）
// 编译: g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Itest/unit -o test/bin/test_iface_type
//        test/unit/test_iface_type.cpp src/iface_type.cpp -lglog -pthread
//
// 测试策略：使用临时目录模拟 sysfs 结构，覆盖以下场景：
//   1. WEXT wireless 目录存在 → WiFi
//   2. cfg80211 phy80211 符号链接存在（无 wireless）→ WiFi
//   3. 接口名 wlan/wlp/wlx 前缀（无 sysfs 标志）→ WiFi
//   4. 有线接口 eth0（无任何无线标志）→ 非 WiFi
//   5. 空接口名 → 非 WiFi

#include "test_common.hpp"
#include "iface_type.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace weaknet_dbus;

// ============================================================================
// 辅助：临时 sysfs 模拟目录
// ============================================================================
class TempSysfs {
public:
    std::string root;

    TempSysfs() {
        char tmpl[] = "/tmp/weaknet_iface_test_XXXXXX";
        root = mkdtemp(tmpl);
    }

    ~TempSysfs() {
        cleanup(root);
    }

    // 创建接口目录：root/<iface>/
    void makeInterfaceDir(const std::string& iface) {
        std::string path = root + "/" + iface;
        mkdir(path.c_str(), 0755);
    }

    // 创建 wireless 目录：root/<iface>/wireless
    void makeWirelessDir(const std::string& iface) {
        makeInterfaceDir(iface);
        std::string path = root + "/" + iface + "/wireless";
        mkdir(path.c_str(), 0755);
    }

    // 创建 phy80211 符号链接：root/<iface>/phy80211 -> /sys/class/ieee80211/phy0
    void makePhy80211Symlink(const std::string& iface) {
        makeInterfaceDir(iface);
        std::string path = root + "/" + iface + "/phy80211";
        symlink("/sys/class/ieee80211/phy0", path.c_str());
    }

    // 创建 phy80211 目录（某些驱动可能创建目录而非符号链接）
    void makePhy80211Dir(const std::string& iface) {
        makeInterfaceDir(iface);
        std::string path = root + "/" + iface + "/phy80211";
        mkdir(path.c_str(), 0755);
    }

private:
    void cleanup(const std::string& dir) {
        std::string cmd = "rm -rf '" + dir + "'";
        system(cmd.c_str());
    }
};

// ============================================================================
// 测试用例
// ============================================================================

// 测试1: WEXT wireless 目录存在 → 应识别为 WiFi
static void testWirelessDirIsWifi() {
    TEST_CASE("WEXT wireless 目录存在应识别为 WiFi");
    TempSysfs sysfs;
    sysfs.makeWirelessDir("wlan0");
    CHECK(isWirelessInterface("wlan0", sysfs.root));
}

// 测试2: phy80211 符号链接存在（无 wireless 目录）→ 应识别为 WiFi
//        这是 cfg80211 驱动的典型场景，也是本次修复的核心目标
static void testPhy80211SymlinkIsWifi() {
    TEST_CASE("phy80211 符号链接存在（无 wireless）应识别为 WiFi");
    TempSysfs sysfs;
    sysfs.makePhy80211Symlink("wlan0");
    CHECK(isWirelessInterface("wlan0", sysfs.root));
}

// 测试3: phy80211 目录存在（无 wireless 目录）→ 应识别为 WiFi
static void testPhy80211DirIsWifi() {
    TEST_CASE("phy80211 目录存在（无 wireless）应识别为 WiFi");
    TempSysfs sysfs;
    sysfs.makePhy80211Dir("wlan0");
    CHECK(isWirelessInterface("wlan0", sysfs.root));
}

// 测试4: 接口名 wlan 前缀（无 sysfs 标志）→ 应识别为 WiFi（前缀后备）
static void testWlanPrefixIsWifi() {
    TEST_CASE("wlan 前缀（无 sysfs 标志）应识别为 WiFi");
    TempSysfs sysfs;
    sysfs.makeInterfaceDir("wlan0");
    CHECK(isWirelessInterface("wlan0", sysfs.root));
}

// 测试5: 接口名 wlp 前缀（无 sysfs 标志）→ 应识别为 WiFi（前缀后备）
static void testWlpPrefixIsWifi() {
    TEST_CASE("wlp 前缀（无 sysfs 标志）应识别为 WiFi");
    TempSysfs sysfs;
    sysfs.makeInterfaceDir("wlp3s0");
    CHECK(isWirelessInterface("wlp3s0", sysfs.root));
}

// 测试6: 接口名 wlx 前缀（无 sysfs 标志）→ 应识别为 WiFi（前缀后备，USB Wi-Fi）
static void testWlxPrefixIsWifi() {
    TEST_CASE("wlx 前缀（无 sysfs 标志）应识别为 WiFi");
    TempSysfs sysfs;
    sysfs.makeInterfaceDir("wlx001122334455");
    CHECK(isWirelessInterface("wlx001122334455", sysfs.root));
}

// 测试7: 有线接口 eth0（无任何无线标志）→ 不应识别为 WiFi
static void testEthIsNotWifi() {
    TEST_CASE("eth0 有线接口不应识别为 WiFi");
    TempSysfs sysfs;
    sysfs.makeInterfaceDir("eth0");
    CHECK(!isWirelessInterface("eth0", sysfs.root));
}

// 测试8: 有线接口 enp0s3（无任何无线标志）→ 不应识别为 WiFi
static void testEnpIsNotWifi() {
    TEST_CASE("enp0s3 有线接口不应识别为 WiFi");
    TempSysfs sysfs;
    sysfs.makeInterfaceDir("enp0s3");
    CHECK(!isWirelessInterface("enp0s3", sysfs.root));
}

// 测试9: 空接口名 → 不应识别为 WiFi
static void testEmptyNameIsNotWifi() {
    TEST_CASE("空接口名不应识别为 WiFi");
    TempSysfs sysfs;
    CHECK(!isWirelessInterface("", sysfs.root));
}

// 测试10: 不存在的接口 → 不应识别为 WiFi
static void testNonExistentIsNotWifi() {
    TEST_CASE("不存在的接口不应识别为 WiFi");
    TempSysfs sysfs;
    CHECK(!isWirelessInterface("nonexistent0", sysfs.root));
}

// ============================================================================
// main
// ============================================================================
int main() {
    initTestLogging("test_iface_type");

    testWirelessDirIsWifi();
    testPhy80211SymlinkIsWifi();
    testPhy80211DirIsWifi();
    testWlanPrefixIsWifi();
    testWlpPrefixIsWifi();
    testWlxPrefixIsWifi();
    testEthIsNotWifi();
    testEnpIsNotWifi();
    testEmptyNameIsNotWifi();
    testNonExistentIsNotWifi();

    return printTestResult();
}

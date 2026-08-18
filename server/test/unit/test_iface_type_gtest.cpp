// test_iface_type_gtest.cpp
// Wi-Fi Interface Type Detection unit tests (Google Test version)
// Module under test: iface_type.cpp (isWirelessInterface function)
//
// Test strategy: Use temporary directories to simulate sysfs structure, covering:
//   1. WEXT wireless directory exists -> WiFi
//   2. cfg80211 phy80211 symlink exists (no wireless) -> WiFi
//   3. Interface name wlan/wlp/wlx prefix (no sysfs flags) -> WiFi
//   4. Wired interface eth0 (no wireless flags) -> Not WiFi
//   5. Empty interface name -> Not WiFi

#include <gtest/gtest.h>
#include "iface_type.hpp"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace weaknet_dbus;

// ============================================================================
// Test Fixture: Temporary sysfs simulation directory
// ============================================================================
class IfaceTypeTest : public ::testing::Test {
protected:
    void SetUp() override {
        char tmpl[] = "/tmp/weaknet_iface_test_XXXXXX";
        root_ = mkdtemp(tmpl);
    }

    void TearDown() override {
        // Recursive cleanup
        std::string cmd = "rm -rf '" + root_ + "'";
        system(cmd.c_str());
    }

    // Create interface directory: root/<iface>/
    void makeInterfaceDir(const std::string& iface) {
        std::string path = root_ + "/" + iface;
        mkdir(path.c_str(), 0755);
    }

    // Create wireless directory: root/<iface>/wireless
    void makeWirelessDir(const std::string& iface) {
        makeInterfaceDir(iface);
        std::string path = root_ + "/" + iface + "/wireless";
        mkdir(path.c_str(), 0755);
    }

    // Create phy80211 symlink: root/<iface>/phy80211 -> /sys/class/ieee80211/phy0
    void makePhy80211Symlink(const std::string& iface) {
        makeInterfaceDir(iface);
        std::string path = root_ + "/" + iface + "/phy80211";
        symlink("/sys/class/ieee80211/phy0", path.c_str());
    }

    // Create phy80211 directory (some drivers create directory instead of symlink)
    void makePhy80211Dir(const std::string& iface) {
        makeInterfaceDir(iface);
        std::string path = root_ + "/" + iface + "/phy80211";
        mkdir(path.c_str(), 0755);
    }

    std::string root_;
};

// ============================================================================
// Test Cases
// ============================================================================

// Test 1: WEXT wireless directory exists -> should be WiFi
TEST_F(IfaceTypeTest, WirelessDirIsWifi) {
    makeWirelessDir("wlan0");
    EXPECT_TRUE(isWirelessInterface("wlan0", root_));
}

// Test 2: phy80211 symlink exists (no wireless) -> should be WiFi
TEST_F(IfaceTypeTest, Phy80211SymlinkIsWifi) {
    makePhy80211Symlink("wlan0");
    EXPECT_TRUE(isWirelessInterface("wlan0", root_));
}

// Test 3: phy80211 directory exists (no wireless) -> should be WiFi
TEST_F(IfaceTypeTest, Phy80211DirIsWifi) {
    makePhy80211Dir("wlan0");
    EXPECT_TRUE(isWirelessInterface("wlan0", root_));
}

// Test 4: Interface name wlan prefix (no sysfs flags) -> should be WiFi (prefix fallback)
TEST_F(IfaceTypeTest, WlanPrefixIsWifi) {
    makeInterfaceDir("wlan0");
    EXPECT_TRUE(isWirelessInterface("wlan0", root_));
}

// Test 5: Interface name wlp prefix (no sysfs flags) -> should be WiFi (prefix fallback)
TEST_F(IfaceTypeTest, WlpPrefixIsWifi) {
    makeInterfaceDir("wlp3s0");
    EXPECT_TRUE(isWirelessInterface("wlp3s0", root_));
}

// Test 6: Interface name wlx prefix (no sysfs flags) -> should be WiFi (USB Wi-Fi prefix fallback)
TEST_F(IfaceTypeTest, WlxPrefixIsWifi) {
    makeInterfaceDir("wlx001122334455");
    EXPECT_TRUE(isWirelessInterface("wlx001122334455", root_));
}

// Test 7: Wired interface eth0 (no wireless flags) -> should not be WiFi
TEST_F(IfaceTypeTest, EthIsNotWifi) {
    makeInterfaceDir("eth0");
    EXPECT_FALSE(isWirelessInterface("eth0", root_));
}

// Test 8: Wired interface enp0s3 (no wireless flags) -> should not be WiFi
TEST_F(IfaceTypeTest, EnpIsNotWifi) {
    makeInterfaceDir("enp0s3");
    EXPECT_FALSE(isWirelessInterface("enp0s3", root_));
}

// Test 9: Empty interface name -> should not be WiFi
TEST_F(IfaceTypeTest, EmptyNameIsNotWifi) {
    EXPECT_FALSE(isWirelessInterface("", root_));
}

// Test 10: Non-existent interface -> should not be WiFi
TEST_F(IfaceTypeTest, NonExistentIsNotWifi) {
    EXPECT_FALSE(isWirelessInterface("nonexistent0", root_));
}

// ============================================================================
// main function
// ============================================================================
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

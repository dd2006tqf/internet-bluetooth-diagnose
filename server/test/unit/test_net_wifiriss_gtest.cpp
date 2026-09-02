#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "net_wifiriss.h"

using weaknet_dbus::parseWifiRssiResponse;

TEST(WifiRssiTest, ParsesValidSignalPollResponse) {
    EXPECT_EQ(parseWifiRssiResponse("RSSI=-42\nLINKSPEED=65000\n"), -42);
}

TEST(WifiRssiTest, RejectsOutOfRangeSignal) {
    EXPECT_EQ(parseWifiRssiResponse("RSSI=-15\n"), -1000);
    EXPECT_EQ(parseWifiRssiResponse("RSSI=-101\n"), -1000);
    EXPECT_EQ(parseWifiRssiResponse("RSSI=3\n"), -1000);
}

TEST(WifiRssiTest, ReadsProcWirelessFallback) {
    const std::string proc =
        "Inter-| sta-|   Quality        | Discarded packets |\n"
        " wlan0: 0000   70.  -17.  -256        0      0      0\n";
    const std::string procPath = "./test_proc_wireless_rssi.txt";
    {
        std::ofstream out(procPath);
        out << proc;
    }
    const int rssi = weaknet_dbus::readProcWirelessRssi("wlan0", procPath);
    std::remove(procPath.c_str());
    EXPECT_EQ(rssi, -30);
}

TEST(WifiRssiTest, RejectsMissingOrMalformedSignal) {
    EXPECT_EQ(parseWifiRssiResponse("LINKSPEED=65000\n"), -1000);
    EXPECT_EQ(parseWifiRssiResponse("RSSI=unknown\n"), -1000);
}



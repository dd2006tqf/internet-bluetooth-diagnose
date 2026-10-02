// test_topology_collector_gtest.cpp
// 物理网络拓扑元数据采集单测

#include <gtest/gtest.h>
#include "topology_collector.hpp"

#include <fstream>
#include <cstdio>
#include <unistd.h>

using namespace weaknet;

namespace {

std::string createTempResolvConf(const std::string& content) {
    std::string path = "/tmp/test_resolv_" + std::to_string(getpid()) + "_" + std::to_string(rand()) + ".conf";
    std::ofstream out(path);
    out << content;
    out.close();
    return path;
}

} // namespace

TEST(TopologyCollectorTest, ReadDnsServersParsesValidEntries) {
    std::string mock_resolv =
        "# This is a comment\n"
        "; Another comment\n"
        "nameserver 223.5.5.5\n"
        "nameserver 114.114.114.114 # inline comment\n"
        "search example.com\n"
        "nameserver 2400:3200::1\n" // IPv6 支持
        "nameserver invalid_ip\n";

    std::string path = createTempResolvConf(mock_resolv);
    auto servers = TopologyCollector::readDnsServers(path);
    std::remove(path.c_str());

    ASSERT_EQ(servers.size(), 3u);
    EXPECT_EQ(servers[0], "223.5.5.5");
    EXPECT_EQ(servers[1], "114.114.114.114");
    EXPECT_EQ(servers[2], "2400:3200::1");
}

TEST(TopologyCollectorTest, ReadDnsServersHandlesNonexistentFile) {
    auto servers = TopologyCollector::readDnsServers("/tmp/nonexistent_resolv_path_xyz.conf");
    EXPECT_TRUE(servers.empty());
}

TEST(TopologyCollectorTest, CollectHandlesEmptyInterfaceGracefully) {
    auto snap = TopologyCollector::collect("", false);
    EXPECT_FALSE(snap.collected);
    EXPECT_FALSE(snap.mac_address.has_value());
    EXPECT_FALSE(snap.ip_address.has_value());
}

TEST(TopologyCollectorTest, CollectLoopbackInterface) {
    // 针对本地 lo 回环接口采集
    auto snap = TopologyCollector::collect("lo", false);
    EXPECT_TRUE(snap.collected);
    // lo 接口没有实际公网默认网关，且 IP 127.0.0.1 会被跳过过滤
    EXPECT_FALSE(snap.ip_address.has_value());
}

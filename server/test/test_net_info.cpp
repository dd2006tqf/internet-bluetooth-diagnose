// test_net_info.cpp
// NetInfo 序列化/反序列化与数据验证的单元测试
// 编译: g++ -std=c++17 -O2 -Wall -Iinclude -o test_net_info test/test_net_info.cpp src/net_info.cpp src/serializer.cpp

#include "net_info.hpp"

#include <cassert>
#include <iostream>
#include <string>

using namespace weaknet_dbus;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::cerr << "FAIL: " << #cond << " (line " << __LINE__ << ")" << std::endl; } \
} while (0)

static void testValidation() {
    std::cout << "[test] 数据验证..." << std::endl;

    // 默认构造的对象接口名为空，应判定为无效
    NetInfo def;
    CHECK(!def.isValid());

    // 空接口名无效
    NetInfo empty_name;
    CHECK(!empty_name.isValid());

    // 正常对象
    NetInfo ok("wlan0");
    ok.setRttMs(45);
    ok.setTcpLossRate(2.5);
    ok.setRssiDbm(-65);
    ok.setJitterMs(10.0);
    CHECK(ok.isValid());

    // 仅设置接口名也应有效（字段取默认值均合法）
    NetInfo name_only("eth0");
    CHECK(name_only.isValid());

    // RTT 非法
    NetInfo bad_rtt("wlan0");
    bad_rtt.setRttMs(-2);
    CHECK(!bad_rtt.isValid());

    // 丢包率超范围
    NetInfo bad_loss("wlan0");
    bad_loss.setTcpLossRate(150.0);
    CHECK(!bad_loss.isValid());

    // RSSI 非法
    NetInfo bad_rssi("wlan0");
    bad_rssi.setRssiDbm(-200);
    CHECK(!bad_rssi.isValid());

    // hasXxx 检查
    CHECK(name_only.hasRtt() == false);
    CHECK(name_only.hasTcpLoss() == false);
    CHECK(name_only.hasRssi() == false);
    CHECK(name_only.hasJitter() == false);
    CHECK(ok.hasRtt());
    CHECK(ok.hasTcpLoss());
    CHECK(ok.hasRssi());
    CHECK(ok.hasJitter());
    CHECK(ok.hasEnoughMetricsForAssessment());
    CHECK(!name_only.hasEnoughMetricsForAssessment());
}

static void testNeedsUpdate() {
    std::cout << "[test] needsUpdate..." << std::endl;

    NetInfo a("wlan0");
    a.setRttMs(45);
    a.setRssiDbm(-65);

    NetInfo b("wlan0");
    b.setRttMs(45);
    b.setRssiDbm(-65);
    CHECK(!a.needsUpdate(b));  // 完全相同

    b.setRssiDbm(-70);
    CHECK(a.needsUpdate(b));   // RSSI 变化

    NetInfo c("eth0");
    c.setRttMs(45);
    CHECK(a.needsUpdate(c));   // 不同接口
}

static void testJsonRoundTrip() {
    std::cout << "[test] JSON 往返..." << std::endl;

    NetInfo orig("wlan0");
    orig.setDefaultRoute(true);
    orig.setType(NetType::WiFi);
    orig.setState(NetState::Up);
    orig.setUsingNow(true);
    orig.setQuality(LinkQuality::Good);
    orig.setRttMs(45);
    orig.setPrevRttMs(40);
    orig.setRssiDbm(-65);
    orig.setTcpLossRate(2.5);
    orig.setTcpLossLevel("degraded");
    orig.setTrafficStats(1250000, 1500, 42);
    orig.setJitterMs(10.5);
    orig.setJitterLevel("good");

    std::string json = orig.toJson();
    std::cout << "  JSON: " << json << std::endl;

    NetInfo restored;
    CHECK(restored.fromJson(json));
    CHECK(restored.ifName() == "wlan0");
    CHECK(restored.isDefaultRoute() == true);
    CHECK(restored.type() == NetType::WiFi);
    CHECK(restored.state() == NetState::Up);
    CHECK(restored.usingNow() == true);
    CHECK(restored.quality() == LinkQuality::Good);
    CHECK(restored.rttMs() == 45);
    CHECK(restored.prevRttMs() == 40);
    CHECK(restored.rssiDbm() == -65);
    CHECK(restored.trafficTotalBps() == 1250000);
    CHECK(restored.trafficTotalPps() == 1500);
    CHECK(restored.trafficActiveFlows() == 42);
    CHECK(restored.tcpLossLevel() == "degraded");
    CHECK(restored.jitterLevel() == "good");

    // 浮点比较（容差）
    CHECK(std::abs(restored.tcpLossRate() - 2.5) < 0.01);
    CHECK(std::abs(restored.jitterMs() - 10.5) < 0.01);
}

static void testJsonSpecialChars() {
    std::cout << "[test] JSON 特殊字符转义..." << std::endl;

    NetInfo orig("eth\"\\0\n");
    orig.setTcpLossLevel("level\twith\nbreaks");
    orig.setJitterLevel("jitter\\level");

    std::string json = orig.toJson();
    NetInfo restored;
    CHECK(restored.fromJson(json));
    CHECK(restored.ifName() == "eth\"\\0\n");
    CHECK(restored.tcpLossLevel() == "level\twith\nbreaks");
    CHECK(restored.jitterLevel() == "jitter\\level");
}

static void testJsonMalformed() {
    std::cout << "[test] JSON 畸形输入..." << std::endl;

    NetInfo info("wlan0");
    info.setRttMs(100);

    // 空字符串
    NetInfo tmp1;
    CHECK(!tmp1.fromJson(""));

    // 缺少大括号
    CHECK(!tmp1.fromJson("\"ifname\":\"wlan0\""));

    // 不完整字段
    CHECK(!tmp1.fromJson("{\"ifname\":"));

    // 合法但未知字段应被忽略
    NetInfo tmp2;
    CHECK(tmp2.fromJson("{\"ifname\":\"eth0\",\"unknown_field\":123}"));
    CHECK(tmp2.ifName() == "eth0");

    // 畸形输入不应修改原对象
    NetInfo tmp3("original");
    CHECK(!tmp3.fromJson("{broken"));
    CHECK(tmp3.ifName() == "original");

    // 负数的 traffic_bps 应被拒绝（不会变成巨大的无符号值）
    NetInfo tmp4;
    CHECK(tmp4.fromJson("{\"ifname\":\"eth0\",\"traffic_bps\":-1}"));
    CHECK(tmp4.trafficTotalBps() == 0);  // 负数被拒绝，保持默认值 0
}

static void testBinaryRoundTrip() {
    std::cout << "[test] 二进制往返..." << std::endl;

    NetInfo orig("wlan0");
    orig.setDefaultRoute(true);
    orig.setType(NetType::WiFi);
    orig.setState(NetState::Up);
    orig.setUsingNow(true);
    orig.setQuality(LinkQuality::Good);
    orig.setRttMs(45);
    orig.setPrevRttMs(40);
    orig.setRssiDbm(-65);
    orig.setTcpLossRate(2.5);
    orig.setTcpLossLevel("degraded");
    orig.setTrafficStats(1250000, 1500, 42);
    orig.setJitterMs(10.5);
    orig.setJitterLevel("good");

    std::vector<uint8_t> bin = orig.toBinary();
    std::cout << "  二进制大小: " << bin.size() << " 字节" << std::endl;

    NetInfo restored;
    CHECK(restored.fromBinary(bin));
    CHECK(restored.ifName() == "wlan0");
    CHECK(restored.isDefaultRoute() == true);
    CHECK(restored.type() == NetType::WiFi);
    CHECK(restored.state() == NetState::Up);
    CHECK(restored.usingNow() == true);
    CHECK(restored.quality() == LinkQuality::Good);
    CHECK(restored.rttMs() == 45);
    CHECK(restored.prevRttMs() == 40);
    CHECK(restored.rssiDbm() == -65);
    CHECK(restored.trafficTotalBps() == 1250000);
    CHECK(restored.trafficTotalPps() == 1500);
    CHECK(restored.trafficActiveFlows() == 42);
    CHECK(restored.tcpLossLevel() == "degraded");
    CHECK(restored.jitterLevel() == "good");
    CHECK(std::abs(restored.tcpLossRate() - 2.5) < 0.001);
    CHECK(std::abs(restored.jitterMs() - 10.5) < 0.001);
}

static void testBinaryMalformed() {
    std::cout << "[test] 二进制畸形输入..." << std::endl;

    // 空缓冲区
    NetInfo tmp1;
    CHECK(!tmp1.fromBinary({}));

    // 版本号错误
    std::vector<uint8_t> bad_version = {0x02, 0x00, 0x00, 0x00};
    NetInfo tmp2;
    CHECK(!tmp2.fromBinary(bad_version));

    // 截断缓冲区
    NetInfo orig("wlan0");
    orig.setRttMs(45);
    auto bin = orig.toBinary();
    bin.resize(bin.size() / 2);  // 截断
    NetInfo tmp3("original");
    CHECK(!tmp3.fromBinary(bin));
    CHECK(tmp3.ifName() == "original");  // 不应被修改
}

static void testBinaryCompatibility() {
    std::cout << "[test] 版本号兼容性..." << std::endl;

    // 旧版本应被拒绝
    std::vector<uint8_t> old;
    old.push_back(0x02);  // 版本号 = 2（未来版本）
    old.push_back(0x00);
    old.push_back(0x00);
    old.push_back(0x00);
    NetInfo tmp;
    CHECK(!tmp.fromBinary(old));
}

int main() {
    std::cout << "========== NetInfo 扩展功能测试 ==========" << std::endl;

    testValidation();
    testNeedsUpdate();
    testJsonRoundTrip();
    testJsonSpecialChars();
    testJsonMalformed();
    testBinaryRoundTrip();
    testBinaryMalformed();
    testBinaryCompatibility();

    std::cout << "==========================================" << std::endl;
    std::cout << "通过: " << g_pass << ", 失败: " << g_fail << std::endl;
    return g_fail == 0 ? 0 : 1;
}

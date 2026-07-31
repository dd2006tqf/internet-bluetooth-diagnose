// test_traffic_analyzer.cpp
// TrafficAnalyzer 降级模式单元测试
// 被测模块: traffic_analyzer.hpp / traffic_analyzer.cpp（降级模式标志）
// 编译: 见 Makefile test_traffic_analyzer 规则

#include "test_common.hpp"
#include "traffic_analyzer.hpp"

#include <chrono>
#include <thread>

using namespace weaknet_dbus;

int main() {
    initTestLogging("test_traffic_analyzer");

    TEST_CASE("降级模式默认为 false");
    {
        TrafficAnalyzer analyzer;
        CHECK_EQ(analyzer.isDegradedMode(), false);
    }

    TEST_CASE("initForInterface 失败后进入降级模式");
    {
        TrafficAnalyzer analyzer;
        // 使用不存在的接口名触发 initForInterface 失败
        analyzer.start("nonexistent_iface_test", 1);
        // degraded_mode_ 在 start() 中同步设置，无需等待线程
        CHECK_EQ(analyzer.isDegradedMode(), true);
        analyzer.stop();
    }

    return printTestResult();
}

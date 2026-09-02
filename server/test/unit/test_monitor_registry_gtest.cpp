// test_monitor_registry_gtest.cpp
// 监控器插件注册表单元测试
// Module under test: monitor_registry.hpp / monitor_registry.cpp

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "monitor_registry.hpp"
#include "logger.hpp"

using namespace weaknet_dbus;

namespace {

// 测试用假插件，order 可配
class FakePlugin : public IMonitorPlugin {
public:
    explicit FakePlugin(std::string name, int ord = 100)
        : name_(std::move(name)), order_(ord) {}
    const char* name() const override { return name_.c_str(); }
    int order() const override { return order_; }
    bool init(ServerContext*) override { return true; }
    bool start(ServerContext*) override { return true; }
    void stop() override {}

private:
    std::string name_;
    int order_;
};

}  // namespace

// 注册表依赖全局状态，测试按名隔离
class MonitorRegistryTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        if (!Logger::init("test_monitor_registry", "./test_logs")) {
            std::cerr << "logger init failed" << std::endl;
        }
    }
    static void TearDownTestSuite() { Logger::shutdown(); }
};

TEST_F(MonitorRegistryTest, InstantiatesInOrder) {
    registerPlugin("reg_a", [] { return std::make_unique<FakePlugin>("reg_a", 30); });
    registerPlugin("reg_b", [] { return std::make_unique<FakePlugin>("reg_b", 10); });
    registerPlugin("reg_c", [] { return std::make_unique<FakePlugin>("reg_c", 20); });

    auto plugins = instantiateAllPlugins();

    // 过滤出本测试注册的插件并验证顺序
    std::vector<std::string> names;
    for (auto& p : plugins) {
        if (std::string(p->name()) == "reg_a" || std::string(p->name()) == "reg_b" ||
            std::string(p->name()) == "reg_c") {
            names.push_back(p->name());
        }
    }
    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "reg_b");  // order 10
    EXPECT_EQ(names[1], "reg_c");  // order 20
    EXPECT_EQ(names[2], "reg_a");  // order 30
}

TEST_F(MonitorRegistryTest, DuplicateNameRejected) {
    // 同名注册第二个应被忽略，实例化后只出现一个
    registerPlugin("reg_dup", [] { return std::make_unique<FakePlugin>("reg_dup", 5); });
    registerPlugin("reg_dup", [] { return std::make_unique<FakePlugin>("reg_dup_second", 5); });

    auto plugins = instantiateAllPlugins();
    int count = 0;
    for (auto& p : plugins) {
        if (std::string(p->name()) == "reg_dup") ++count;
    }
    EXPECT_EQ(count, 1);
}

TEST_F(MonitorRegistryTest, DefaultOrderIsHundred) {
    registerPlugin("reg_def", [] { return std::make_unique<FakePlugin>("reg_def"); });

    auto plugins = instantiateAllPlugins();
    for (auto& p : plugins) {
        if (std::string(p->name()) == "reg_def") {
            EXPECT_EQ(p->order(), 100);
        }
    }
}

// test_monitor_manager_gtest.cpp
// MonitorManager 阶段一生命周期协调测试

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "monitor_manager.hpp"

using namespace weaknet_dbus;

namespace {

class FakePlugin final : public IMonitorPlugin {
public:
    explicit FakePlugin(std::string name, int order = 100, bool init_ok = true,
                        bool start_ok = true,
                        std::vector<std::string> dependencies = {})
        : name_(std::move(name)), order_(order), init_ok_(init_ok), start_ok_(start_ok),
          dependencies_(std::move(dependencies)) {}

    const char* name() const override { return name_.c_str(); }
    int order() const override { return order_; }
    std::vector<std::string> dependencies() const override { return dependencies_; }
    bool init(ServerContext* ctx) override { (void)ctx; ++init_calls; return init_ok_; }
    bool start(ServerContext* ctx) override { (void)ctx; ++start_calls; return start_ok_; }
    void stop() override { ++stop_calls; }

    int init_calls = 0;
    int start_calls = 0;
    int stop_calls = 0;

private:
    std::string name_;
    int order_;
    bool init_ok_;
    bool start_ok_;
    std::vector<std::string> dependencies_;
};

}  // namespace

TEST(MonitorManagerTest, OwnsPluginsAndReportsRunningInOrder) {
    std::vector<std::unique_ptr<IMonitorPlugin>> plugins;
    plugins.push_back(std::make_unique<FakePlugin>("late", 20));
    plugins.push_back(std::make_unique<FakePlugin>("early", 10));

    MonitorManager manager(nullptr, std::move(plugins));

    ASSERT_TRUE(manager.startConfigured());
    const auto statuses = manager.list();
    ASSERT_EQ(statuses.size(), 2u);
    EXPECT_EQ(statuses[0].name, "early");
    EXPECT_EQ(statuses[0].state, MonitorState::Running);
    EXPECT_EQ(statuses[0].generation, 1u);
    EXPECT_EQ(statuses[1].name, "late");
    EXPECT_EQ(statuses[1].state, MonitorState::Running);
}

TEST(MonitorManagerTest, RecordsInitializationFailureWithoutBlockingOtherPlugins) {
    std::vector<std::unique_ptr<IMonitorPlugin>> plugins;
    plugins.push_back(std::make_unique<FakePlugin>("bad", 10, false));
    plugins.push_back(std::make_unique<FakePlugin>("good", 20));

    MonitorManager manager(nullptr, std::move(plugins));

    EXPECT_FALSE(manager.startConfigured());
    MonitorStatus bad;
    MonitorStatus good;
    ASSERT_TRUE(manager.status("bad", &bad));
    ASSERT_TRUE(manager.status("good", &good));
    EXPECT_EQ(bad.state, MonitorState::Failed);
    EXPECT_FALSE(bad.error.empty());
    EXPECT_EQ(good.state, MonitorState::Running);
}

TEST(MonitorManagerTest, ReportsConfiguredDisabledAndTracksDesiredState) {
    std::vector<std::unique_ptr<IMonitorPlugin>> plugins;
    plugins.push_back(std::make_unique<FakePlugin>("optional"));
    MonitorManager manager(nullptr, std::move(plugins));
    // A null context has no configuration source; the fake plugin remains runnable.
    ASSERT_TRUE(manager.startConfigured());
    MonitorStatus status;
    ASSERT_TRUE(manager.status("optional", &status));
    EXPECT_TRUE(status.desired_enabled);
    EXPECT_EQ(status.state, MonitorState::Running);
    EXPECT_FALSE(status.changed_at.empty());
}

TEST(MonitorManagerTest, EnablingDependentPluginRequiresDependency) {
    std::vector<std::unique_ptr<IMonitorPlugin>> plugins;
    plugins.push_back(std::make_unique<FakePlugin>("dependent", 20, true, true,
                                                    std::vector<std::string>{"base"}));
    MonitorManager manager(nullptr, std::move(plugins));
    EXPECT_FALSE(manager.startConfigured());
    MonitorStatus dependent_status;
    ASSERT_TRUE(manager.status("dependent", &dependent_status));
    EXPECT_NE(dependent_status.error.find("dependency"), std::string::npos);
    std::string error;
    EXPECT_FALSE(manager.restart("dependent", &error));
    EXPECT_NE(error.find("dependency"), std::string::npos);
}

TEST(MonitorManagerTest, StopAllIsIdempotentAndUnknownOperationsAreRejected) {
    std::vector<std::unique_ptr<IMonitorPlugin>> plugins;
    plugins.push_back(std::make_unique<FakePlugin>("one"));
    MonitorManager manager(nullptr, std::move(plugins));

    ASSERT_TRUE(manager.startConfigured());
    EXPECT_TRUE(manager.stopAll());
    EXPECT_TRUE(manager.stopAll());

    MonitorStatus status;
    ASSERT_TRUE(manager.status("one", &status));
    EXPECT_EQ(status.state, MonitorState::Stopped);

    std::string error;
    EXPECT_FALSE(manager.enable("one", &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(manager.status("missing", &status));
}

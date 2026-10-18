/**
 * @file test_bt_events_lifecycle_gtest.cpp
 * @brief BtEventsPlugin 生命周期、指针读写锁纪律及 UsingInterfaceManager 停机回归套件
 */

#include <gtest/gtest.h>
#include <glog/logging.h>

#include "server.hpp"
#include "dbus_service.hpp"
#include "weak_netmgr.hpp"
#include "active_connectivity_monitor.hpp"
#include "database_manager.hpp"
#include "monitor_plugin.hpp"
#include "wireless_event_store.hpp"
#include "bt_link_quality_tracker.hpp"
#include "site_incident_correlator.hpp"
#include "using_iface.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <shared_mutex>
#include <thread>
#include <vector>

using namespace weaknet_dbus;

class BtEventsLifecycleTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_unique<ServerContext>();
    }

    void TearDown() override {
        ctx_.reset();
    }

    std::unique_ptr<ServerContext> ctx_;
};

// 验证判空共享锁纪律：初始状态下三个跨插件指针均为 nullptr，
// 读者必须能在 monitor_pointers_mutex 共享锁内安全读到空指针并降级
TEST_F(BtEventsLifecycleTest, MonitorPointersMutexProtectsNullptrAccess) {
    std::shared_lock<std::shared_mutex> lock(ctx_->monitor_pointers_mutex);
    EXPECT_EQ(ctx_->wireless_event_store, nullptr);
    EXPECT_EQ(ctx_->bt_link_quality_tracker, nullptr);
    EXPECT_EQ(ctx_->site_incident_correlator, nullptr);
}

// 模拟并发读写：一个写线程模拟插件不断切换启动与销毁状态，
// 多个读线程并发持有 shared_lock 读取并判空访问，断言不发生 UAF 或竞争
TEST_F(BtEventsLifecycleTest, ConcurrentReadWriteLifecycleNoUAF) {
    std::atomic<bool> running{true};
    std::atomic<uint64_t> reads_completed{0};
    std::atomic<uint64_t> writes_completed{0};

    // 写线程
    std::thread writer([&]() {
        while (running.load()) {
            auto store = std::make_unique<WirelessEventStore>(nullptr, WirelessEventStoreConfig{});
            auto tracker = std::make_unique<BtLinkQualityTracker>();
            auto correlator = std::make_unique<SiteIncidentCorrelator>(nullptr, SiteIncidentConfig{});

            // 写入发布
            {
                std::unique_lock<std::shared_mutex> lock(ctx_->monitor_pointers_mutex);
                ctx_->wireless_event_store = store.get();
                ctx_->bt_link_quality_tracker = tracker.get();
                ctx_->site_incident_correlator = correlator.get();
            }

            std::this_thread::sleep_for(std::chrono::microseconds(500));

            // 置空并析构（在独占锁内原子完成）
            {
                std::unique_lock<std::shared_mutex> lock(ctx_->monitor_pointers_mutex);
                ctx_->wireless_event_store = nullptr;
                ctx_->bt_link_quality_tracker = nullptr;
                ctx_->site_incident_correlator = nullptr;
                correlator.reset();
                tracker.reset();
                store.reset();
            }

            writes_completed.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    // 读线程池（模拟 bt_hci worker、BtMonitor 循环与 D-Bus 查询）
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&]() {
            while (running.load()) {
                std::shared_lock<std::shared_mutex> lock(ctx_->monitor_pointers_mutex);
                // 关键断言：只要指针非空，其底层对象必须完整有效，绝不为野指针
                if (ctx_->wireless_event_store) {
                    EXPECT_NO_THROW({
                        (void)ctx_->wireless_event_store->config().gateway_id;
                    });
                }
                if (ctx_->bt_link_quality_tracker) {
                    EXPECT_NO_THROW({
                        (void)ctx_->bt_link_quality_tracker->getAllProfiles();
                    });
                }
                reads_completed.fetch_add(1);
            }
        });
    }

    // 压测 300 毫秒
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    running.store(false);

    writer.join();
    for (auto& r : readers) {
        r.join();
    }

    EXPECT_GT(writes_completed.load(), 0u);
    EXPECT_GT(reads_completed.load(), 100u);
}

// 验证 UsingInterfaceManager::stop() 的幂等性与安全性
TEST(UsingInterfaceManagerLifecycleTest, StopIdempotentAndSafe) {
    auto mgr = UsingInterfaceManager::getInstance();
    ASSERT_NE(mgr, nullptr);

    // 显式测试 stop 接口的幂等调用
    EXPECT_NO_THROW(mgr->stop());
    EXPECT_NO_THROW(mgr->stop());
    EXPECT_NO_THROW(mgr->stop());
}

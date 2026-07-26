// test_bt_full_link.cpp
// 蓝牙全链路集成测试：频段冲突检测 → EventManager 路由 → D-Bus 信号载荷
//
// 验证 spec Event Routing "频段冲突事件" 场景：
//   WHEN 频段冲突检测器输出 detected=true
//   THEN 经 EventManager.emitNetworkQualityChanged 推送，信号载荷含 "band_conflict" 标识
//
// 本测试链接多个真实生产组件（BandConflictDetector + NetworkEventManager + DbusService
// 桩），验证 feed→detect→emit→callback 端到端链路。DbusService 用 mock 桩替代真实
// D-Bus 连接，仅作为信号 sink 记录 counter；emit 路由逻辑为 event_manager.cpp 真实执行。
//
// 编译: g++ -std=c++17 -O2 -Wall -Wextra -Iinclude -Itest/unit -o test/bin/test_bt_full_link
//        test/unit/test_bt_full_link.cpp src/band_conflict_detector.cpp
//        src/event_manager.cpp test/unit/mock_dbus_service.cpp src/logger.cpp
//        -lglog `pkg-config --cflags --libs dbus-1`

#include "test_common.hpp"
#include "band_conflict_detector.hpp"
#include "event_manager.hpp"
#include "server.hpp"
#include "dbus_service.hpp"

#include <atomic>
#include <string>
#include <vector>
#include <cstdint>

// 测试用 counter 记录器（定义于 mock_dbus_service.cpp），验证 D-Bus emit 路径走通
namespace weaknet_dbus::test_recorder {
void resetCounters();
std::vector<int32_t> snapshotCounters();
}

using namespace weaknet_dbus;

// ============================================================================
// 测试1: 频段冲突全链路 — feedSample 完全正相关 → detect → emit → callback
// 对应 spec: Event Routing / 频段冲突事件
//   WHEN 频段冲突检测器输出 detected=true
//   THEN 经 EventManager.emitNetworkQualityChanged 推送，NetworkQualityChanged 信号
//        载荷含 "band_conflict" 标识（source 或 details）
// ============================================================================
static void testBandConflictFullLink() {
    TEST_CASE("频段冲突全链路: feed→detect→emit→callback 含 band_conflict 载荷");

    // 构造 EventManager + ServerContext + DbusService(桩)，使 emitEvent 走 D-Bus 路径
    auto& mgr = getEventManager();
    ServerContext ctx;
    DbusService svc(&ctx);
    ctx.service = &svc;
    mgr.startEventMonitoring(&ctx);
    test_recorder::resetCounters();

    // 注册回调记录 NetworkQualityChanged 事件载荷
    std::atomic<bool> callback_triggered{false};
    std::string recorded_msg, recorded_src, recorded_details;
    mgr.registerCallback(EventType::NetworkQualityChanged,
        [&](const NetworkEvent& ev) {
            callback_triggered.store(true);
            recorded_msg = ev.message;
            recorded_src = ev.source;
            recorded_details = ev.details;
        });

    // feedSample 注入完全正相关样本（spec "频段冲突确认" 场景）
    // 20 个高基线 + 3 个同步下降（降幅 20dBm，远超 10dBm 阈值，Pearson≈1.0）
    BandConflictDetector detector;
    for (int i = 0; i < 20; i++) detector.feedSample(-50, -60);
    for (int i = 0; i < 3; i++) detector.feedSample(-70, -80);

    auto result = detector.detect();
    CHECK(result.detected);                    // 检测到冲突
    CHECK_GT(result.confidence, 50.0);         // 置信度 > 50%
    CHECK_GT(result.correlation, 0.7);         // Pearson > 0.7
    CHECK(!result.suggestion.empty());         // 处置建议非空

    // 模拟 server.cpp network_quality_thread 的 emit 调用（server.cpp:412-416）
    mgr.emitNetworkQualityChanged(
        "2.4GHz band conflict detected",
        result.suggestion,
        "band_conflict_detector"
    );

    // 验证回调触发（EventManager 路由通）
    CHECK(callback_triggered.load());

    // 验证 source 含 band_conflict 标识
    CHECK_CONTAINS(recorded_src, "band_conflict");

    // 验证 details（suggestion）含 band_conflict 标识
    // spec "band_conflict 载荷" 严格解读：信号载荷应含 band_conflict 标识
    // RED 阶段: suggestion 当前为中文"检测到严重 2.4GHz 频段冲突..."，不含 band_conflict 英文标识 → 断言失败
    CHECK_CONTAINS(recorded_details, "band_conflict");

    // 验证 D-Bus emit 路径走通（mock 桩记录了 counter）
    CHECK(!test_recorder::snapshotCounters().empty());

    mgr.unregisterCallback(EventType::NetworkQualityChanged);
}

// ============================================================================
// 测试2: 频段冲突未检测时不 emit — 验证全链路在无冲突时不误发信号
// 对应 spec: Event Routing / 频段冲突事件（隐含：detected=false 不 emit）
// ============================================================================
static void testNoConflictNoEmit() {
    TEST_CASE("无冲突时不 emit band_conflict 信号");

    auto& mgr = getEventManager();
    ServerContext ctx;
    DbusService svc(&ctx);
    ctx.service = &svc;
    mgr.startEventMonitoring(&ctx);
    test_recorder::resetCounters();

    std::atomic<int> trigger_count{0};
    mgr.registerCallback(EventType::NetworkQualityChanged,
        [&trigger_count](const NetworkEvent&) { trigger_count.fetch_add(1); });

    // 稳定样本（无下降）→ detect 返回 detected=false
    BandConflictDetector detector;
    for (int i = 0; i < 25; i++) detector.feedSample(-50, -60);
    auto result = detector.detect();
    CHECK(!result.detected);

    // server.cpp 仅在 detected && confidence>50 时 emit，故此处不应 emit
    // （模拟 server.cpp:404 的条件判断）
    if (result.detected && result.confidence > 50.0) {
        mgr.emitNetworkQualityChanged("band conflict", result.suggestion, "band_conflict_detector");
    }

    CHECK_EQ(trigger_count.load(), 0);
    CHECK(test_recorder::snapshotCounters().empty());

    mgr.unregisterCallback(EventType::NetworkQualityChanged);
}

int main(int /*argc*/, char* argv[]) {
    initTestLogging(argv[0]);
    testBandConflictFullLink();
    testNoConflictNoEmit();
    return printTestResult();
}

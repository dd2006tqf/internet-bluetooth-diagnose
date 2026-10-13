#include <gtest/gtest.h>
#include <chrono>
#include <thread>

#include "bt_hci_connection_tracker.hpp"
#include "bt_data_path_analyzer.hpp"

using namespace weaknet;
using namespace weaknet_dbus;

// ============================================================================
// 测试套件 1: BtHciConnectionTracker 单元测试（8项关键验证）
// ============================================================================

TEST(BtHciConnectionTrackerTest, BasicLifecycle) {
    BtHciConnectionTracker tracker;

    EXPECT_EQ(tracker.activeCount(), 0u);

    // 建立连接
    auto key1 = tracker.onConnectionEstablished("hci0", 129, "D0:96:EA:52:30:AC", "PUBLIC");
    EXPECT_EQ(key1.controller_id, "hci0");
    EXPECT_EQ(key1.handle, 129);
    EXPECT_EQ(key1.generation, 1u);
    EXPECT_EQ(tracker.activeCount(), 1u);

    // 查询活跃连接
    auto rec = tracker.getActiveConnection("hci0", 129);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->device_mac, "D0:96:EA:52:30:AC");
    EXPECT_EQ(rec->attribution_scope, "device");
    EXPECT_TRUE(rec->is_active);

    // 断开连接
    bool disconn_ok = tracker.onConnectionDisconnected("hci0", 129);
    EXPECT_TRUE(disconn_ok);
    EXPECT_EQ(tracker.activeCount(), 0u);

    // 断开后查询应为 nullopt
    auto rec_after = tracker.getActiveConnection("hci0", 129);
    EXPECT_FALSE(rec_after.has_value());
}

TEST(BtHciConnectionTrackerTest, GenerationIncrementOnReconnect) {
    BtHciConnectionTracker tracker;

    // 第一次连接 handle 42
    auto key1 = tracker.onConnectionEstablished("hci0", 42, "AA:BB:CC:DD:EE:01");
    EXPECT_EQ(key1.generation, 1u);

    tracker.onConnectionDisconnected("hci0", 42);

    // 相同 handle 被新设备复用
    auto key2 = tracker.onConnectionEstablished("hci0", 42, "11:22:33:44:55:66");
    EXPECT_EQ(key2.generation, 2u); // generation 必须递增，防御句柄复用串设备

    auto rec = tracker.getActiveConnection("hci0", 42);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->device_mac, "11:22:33:44:55:66");
    EXPECT_EQ(rec->key.generation, 2u);
}

TEST(BtHciConnectionTrackerTest, IdempotentNotifications) {
    BtHciConnectionTracker tracker;

    // 首次建立
    auto key1 = tracker.onConnectionEstablished("hci0", 100, "AA:BB:CC:DD:EE:FF");
    EXPECT_EQ(key1.generation, 1u);

    // 重复收到相同 MAC 的建链通知（如 D-Bus 与底层事件重叠）
    auto key2 = tracker.onConnectionEstablished("hci0", 100, "AA:BB:CC:DD:EE:FF");
    EXPECT_EQ(key2.generation, 1u); // 幂等保持相同 generation
    EXPECT_EQ(tracker.activeCount(), 1u);
}

TEST(BtHciConnectionTrackerTest, UnknownDeviceScope) {
    BtHciConnectionTracker tracker;

    // 无法获取 MAC 的连接
    auto key = tracker.onConnectionEstablished("hci0", 200, "");
    EXPECT_EQ(key.handle, 200);

    auto rec = tracker.getActiveConnection("hci0", 200);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->attribution_scope, "controller"); // 明确降级为 controller，不瞎猜
    EXPECT_TRUE(rec->device_mac.empty());
}

TEST(BtHciConnectionTrackerTest, MultipleControllersIsolation) {
    BtHciConnectionTracker tracker;

    tracker.onConnectionEstablished("hci0", 10, "AA:00:00:00:00:01");
    tracker.onConnectionEstablished("hci1", 10, "BB:00:00:00:00:02");

    EXPECT_EQ(tracker.activeCount(), 2u);

    auto rec0 = tracker.getActiveConnection("hci0", 10);
    auto rec1 = tracker.getActiveConnection("hci1", 10);
    ASSERT_TRUE(rec0.has_value());
    ASSERT_TRUE(rec1.has_value());
    EXPECT_EQ(rec0->device_mac, "AA:00:00:00:00:01");
    EXPECT_EQ(rec1->device_mac, "BB:00:00:00:00:02");

    tracker.onConnectionDisconnected("hci0", 10);
    EXPECT_FALSE(tracker.getActiveConnection("hci0", 10).has_value());
    EXPECT_TRUE(tracker.getActiveConnection("hci1", 10).has_value());
}

TEST(BtHciConnectionTrackerTest, ResetController) {
    BtHciConnectionTracker tracker;

    tracker.onConnectionEstablished("hci0", 1, "AA:01");
    tracker.onConnectionEstablished("hci0", 2, "AA:02");
    tracker.onConnectionEstablished("hci1", 1, "BB:01");

    EXPECT_EQ(tracker.activeCount(), 3u);

    tracker.resetController("hci0");
    EXPECT_EQ(tracker.activeCount(), 1u);
    EXPECT_FALSE(tracker.getActiveConnection("hci0", 1).has_value());
    EXPECT_FALSE(tracker.getActiveConnection("hci0", 2).has_value());
    EXPECT_TRUE(tracker.getActiveConnection("hci1", 1).has_value());

    // reset 后再次建立同一 handle，代次必须继续递增
    auto key_re = tracker.onConnectionEstablished("hci0", 1, "AA:01_NEW");
    EXPECT_GT(key_re.generation, 1u);
}

// ============================================================================
// 测试套件 2: BtDataPathAnalyzer 单元测试（吞吐、窗口计算与纯净 DTO）
// ============================================================================

TEST(BtDataPathAnalyzerTest, FallbackOnMissingBpfObject) {
    BtDataPathAnalyzer analyzer;
    bool ok = analyzer.init("/non_existent_path/bt_hci_traffic.bpf.o");
    EXPECT_FALSE(ok);
    EXPECT_FALSE(analyzer.isAvailable());
    EXPECT_EQ(analyzer.commonState(), EbpfMonitorState::Fallback);
}

TEST(BtDataPathAnalyzerTest, ObservationComputationAndRate) {
    auto tracker = std::make_shared<BtHciConnectionTracker>();
    BtDataPathAnalyzer analyzer(tracker);

    // 1. 登记连接身份
    tracker->onConnectionEstablished("hci0", 129, "D0:96:EA:52:30:AC", "PUBLIC");

    auto t0 = std::chrono::steady_clock::now();

    // 2. 注入第一次采样数据（handle 129: TX 9 pkts/416 B, RX 9 pkts/416 B）
    RawAclTrafficVal v1{};
    v1.tx_packets = 9;
    v1.tx_bytes = 416;
    v1.rx_packets = 9;
    v1.rx_bytes = 416;
    v1.last_tx_ktime_ns = 1000000000ULL;
    v1.last_rx_ktime_ns = 1050000000ULL; // 50ms gap
    analyzer.injectMockSample(129, v1);

    auto obs_list1 = analyzer.collectObservations(t0);
    ASSERT_EQ(obs_list1.size(), 1u);

    const auto& obs1 = obs_list1[0];
    EXPECT_EQ(obs1.observation_type, "BT_HCI_ACL_TRAFFIC");
    EXPECT_EQ(obs1.controller_id, "hci0");
    EXPECT_EQ(obs1.connection_handle, 129);
    EXPECT_EQ(obs1.connection_generation, 1u);
    EXPECT_EQ(obs1.attribution_scope, "device");
    ASSERT_TRUE(obs1.device_id.has_value());
    EXPECT_EQ(*obs1.device_id, "D0:96:EA:52:30:AC");
    EXPECT_EQ(obs1.tx_bytes, 416u);
    EXPECT_EQ(obs1.rx_bytes, 416u);
    EXPECT_EQ(obs1.max_gap_ms, 50u);
    EXPECT_EQ(obs1.status, "OBSERVED");

    // 3. 模拟 1000ms 后增量注入（TX 新增 1000 字节，RX 新增 2000 字节）
    auto t1 = t0 + std::chrono::milliseconds(1000);
    RawAclTrafficVal v2 = v1;
    v2.tx_packets += 10;
    v2.tx_bytes += 1000;
    v2.rx_packets += 20;
    v2.rx_bytes += 2000;
    v2.last_tx_ktime_ns += 1000000000ULL;
    v2.last_rx_ktime_ns += 1000000000ULL;
    analyzer.injectMockSample(129, v2);

    auto obs_list2 = analyzer.collectObservations(t1);
    ASSERT_EQ(obs_list2.size(), 1u);

    const auto& obs2 = obs_list2[0];
    EXPECT_EQ(obs2.tx_bytes, 1000u);
    EXPECT_EQ(obs2.rx_bytes, 2000u);
    EXPECT_EQ(obs2.tx_packets, 10u);
    EXPECT_EQ(obs2.rx_packets, 20u);
    EXPECT_EQ(obs2.window_ms, 1000u);
    // 速率校验：1000B * 8 / 1s = 8000 bps
    EXPECT_DOUBLE_EQ(obs2.tx_throughput_bps, 8000.0);
    // 速率校验：2000B * 8 / 1s = 16000 bps
    EXPECT_DOUBLE_EQ(obs2.rx_throughput_bps, 16000.0);

    // 4. JSON 序列化验证
    std::string json = obs2.toJson();
    EXPECT_NE(json.find("\"observation_type\":\"BT_HCI_ACL_TRAFFIC\""), std::string::npos);
    EXPECT_NE(json.find("\"device_id\":\"D0:96:EA:52:30:AC\""), std::string::npos);
    EXPECT_NE(json.find("\"tx_bytes\":1000"), std::string::npos);
    EXPECT_NE(json.find("\"status\":\"OBSERVED\""), std::string::npos);
}

TEST(BtDataPathAnalyzerTest, HandleReuseProtection) {
    auto tracker = std::make_shared<BtHciConnectionTracker>();
    BtDataPathAnalyzer analyzer(tracker);

    // 设备 1 占用 handle 50
    tracker->onConnectionEstablished("hci0", 50, "AA:11:11:11:11:11");
    auto t0 = std::chrono::steady_clock::now();

    RawAclTrafficVal val1{};
    val1.tx_bytes = 500;
    val1.rx_bytes = 500;
    analyzer.injectMockSample(50, val1);
    analyzer.collectObservations(t0);

    // 断开设备 1
    tracker->onConnectionDisconnected("hci0", 50);

    // 设备 2 复用 handle 50
    tracker->onConnectionEstablished("hci0", 50, "BB:22:22:22:22:22");
    auto t1 = t0 + std::chrono::milliseconds(500);

    // 模拟内核计数器在代次变更时的采样 (包含设备 1 遗留的 500 字节 + 设备 2 新增数据)
    RawAclTrafficVal val2{};
    val2.tx_bytes = 600; // 设备 1 留存 500 + 设备 2 初始 100
    val2.rx_bytes = 700; // 设备 1 留存 500 + 设备 2 初始 200
    analyzer.injectMockSample(50, val2);

    // 第 1 轮采集：代次跃迁发生，系统将旧值重置为基准锚点，增量归零隔离
    auto obs_list1 = analyzer.collectObservations(t1);
    ASSERT_EQ(obs_list1.size(), 1u);
    const auto& obs1 = obs_list1[0];

    EXPECT_EQ(obs1.connection_generation, 2u);
    ASSERT_TRUE(obs1.device_id.has_value());
    EXPECT_EQ(*obs1.device_id, "BB:22:22:22:22:22");
    EXPECT_EQ(obs1.tx_bytes, 0u);
    EXPECT_EQ(obs1.rx_bytes, 0u);

    // 第 2 轮采集：设备 2 产生实际业务流量 (+50 TX, +80 RX)
    auto t2 = t1 + std::chrono::milliseconds(1000);
    RawAclTrafficVal val3 = val2;
    val3.tx_bytes += 50;
    val3.rx_bytes += 80;
    analyzer.injectMockSample(50, val3);

    auto obs_list2 = analyzer.collectObservations(t2);
    ASSERT_EQ(obs_list2.size(), 1u);
    const auto& obs2 = obs_list2[0];
    EXPECT_EQ(obs2.connection_generation, 2u);
    EXPECT_EQ(obs2.tx_bytes, 50u);
    EXPECT_EQ(obs2.rx_bytes, 80u);
}

TEST(BtDataPathAnalyzerTest, PruneRemovesDeadHandleEntriesOnly) {
    auto tracker = std::make_shared<BtHciConnectionTracker>();
    BtDataPathAnalyzer analyzer(tracker);

    // handle 50 是活跃连接；handle 77 是断连后的死句柄残留
    tracker->onConnectionEstablished("hci0", 50, "AA:00:00:00:00:50");

    RawAclTrafficVal v50{};
    v50.tx_bytes = 111;
    analyzer.injectMockSample(50, v50);
    RawAclTrafficVal v77{};
    v77.tx_bytes = 999;
    analyzer.injectMockSample(77, v77);

    // 活跃集只有 50 → 77 必须被清掉，50 必须保留
    std::set<uint16_t> live = {50};
    size_t pruned = analyzer.pruneStaleEntries(live);
    EXPECT_EQ(pruned, 1u);

    RawAclTrafficVal out{};
    EXPECT_TRUE(analyzer.readRawMapValue(50, &out));
    EXPECT_EQ(out.tx_bytes, 111u);
    EXPECT_FALSE(analyzer.readRawMapValue(77, &out));

    // 清理后死句柄不再出现在观测里
    auto obs = analyzer.collectObservations(std::chrono::steady_clock::now());
    for (const auto& o : obs) {
        EXPECT_NE(o.connection_handle, 77);
    }
}

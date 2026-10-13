/**
 * @file test_bt_anomaly_detector_gtest.cpp
 * @brief Phase 2 工业蓝牙数据通道规则驱动异常检测单元测试
 */

#include <gtest/gtest.h>
#include "bt_data_path_anomaly_detector.hpp"

using namespace weaknet_dbus;

class BtDataPathAnomalyDetectorTest : public ::testing::Test {
protected:
    BtAnomalyDetectorConfig cfg_;
    void SetUp() override {
        cfg_.stall_timeout_ms = 5000;
        cfg_.stall_consecutive_windows = 2;
        cfg_.min_expected_bps = 10000.0;
        cfg_.degraded_consecutive_windows = 3;
    }
};

TEST_F(BtDataPathAnomalyDetectorTest, AttributionLostDetectedWhenTrafficWithoutDeviceMac) {
    BtDataPathAnomalyDetector detector(cfg_);

    BtDataPathObservation obs;
    obs.controller_id = "hci0";
    obs.connection_handle = 129;
    obs.connection_generation = 1;
    obs.attribution_scope = "controller"; // 无具体设备归因
    obs.device_id = std::nullopt;
    obs.tx_bytes = 500;
    obs.rx_bytes = 200;
    obs.tx_throughput_bps = 4000.0;
    obs.rx_throughput_bps = 1600.0;

    auto anomalies = detector.evaluate({obs}, 1700000000000LL);
    ASSERT_EQ(anomalies.size(), 1u); // 仅触发 AttributionLost（degraded 需连续3窗口）

    EXPECT_EQ(anomalies[0].anomaly_type, "BT_DATAPATH_ATTRIBUTION_LOST");
    EXPECT_EQ(anomalies[0].controller_id, "hci0");
    EXPECT_EQ(anomalies[0].connection_handle, 129);
    EXPECT_EQ(anomalies[0].severity, "WARNING");
    EXPECT_EQ(anomalies[0].consecutive_windows, 1u);
}

TEST_F(BtDataPathAnomalyDetectorTest, StallSuspectedAfterConsecutiveSilenceWindows) {
    BtDataPathAnomalyDetector detector(cfg_);

    BtDataPathObservation obs;
    obs.controller_id = "hci0";
    obs.connection_handle = 50;
    obs.connection_generation = 1;
    obs.attribution_scope = "device";
    obs.device_id = "AA:BB:CC:DD:EE:FF";
    obs.tx_bytes = 0;
    obs.rx_bytes = 0;

    // Window 1: 首次静默，consecutive=1 (阈值是 2)，不触发
    auto a1 = detector.evaluate({obs}, 1000);
    EXPECT_TRUE(a1.empty());

    // Window 2: 连续第 2 次静默，consecutive=2，触发 StallSuspected
    auto a2 = detector.evaluate({obs}, 2000);
    ASSERT_EQ(a2.size(), 1u);
    EXPECT_EQ(a2[0].anomaly_type, "BT_DATAPATH_STALL_SUSPECTED");
    EXPECT_EQ(a2[0].consecutive_windows, 2u);
    EXPECT_EQ(a2[0].severity, "WARNING");

    // 活跃流量恢复，停顿计数清零
    obs.tx_bytes = 100;
    obs.tx_throughput_bps = 50000.0; // 充足带宽
    auto a3 = detector.evaluate({obs}, 3000);
    EXPECT_TRUE(a3.empty());

    // 再次静默一次，不触发
    obs.tx_bytes = 0;
    obs.tx_throughput_bps = 0.0;
    auto a4 = detector.evaluate({obs}, 4000);
    EXPECT_TRUE(a4.empty());
}

TEST_F(BtDataPathAnomalyDetectorTest, ThroughputDegradedAfterConsecutiveLowBps) {
    BtDataPathAnomalyDetector detector(cfg_);

    BtDataPathObservation obs;
    obs.controller_id = "hci0";
    obs.connection_handle = 60;
    obs.connection_generation = 1;
    obs.attribution_scope = "device";
    obs.device_id = "11:22:33:44:55:66";
    obs.tx_bytes = 50;
    obs.rx_bytes = 50;
    obs.tx_throughput_bps = 2000.0;
    obs.rx_throughput_bps = 2000.0; // 合计 4000 bps < 10000 bps

    // 窗口 1 (低于阈值 1 次，门槛是 3)
    auto a1 = detector.evaluate({obs}, 1000);
    EXPECT_TRUE(a1.empty());

    // 窗口 2 (低于阈值 2 次)
    auto a2 = detector.evaluate({obs}, 2000);
    EXPECT_TRUE(a2.empty());

    // 窗口 3 (低于阈值 3 次，触发 ThroughputDegraded)
    auto a3 = detector.evaluate({obs}, 3000);
    ASSERT_EQ(a3.size(), 1u);
    EXPECT_EQ(a3[0].anomaly_type, "BT_DATAPATH_THROUGHPUT_DEGRADED");
    EXPECT_EQ(a3[0].consecutive_windows, 3u);
    EXPECT_DOUBLE_EQ(a3[0].actual_bps, 4000.0);
    EXPECT_DOUBLE_EQ(a3[0].expected_bps, 10000.0);
}

TEST_F(BtDataPathAnomalyDetectorTest, GenerationBumpResetsAnomalyCounters) {
    BtDataPathAnomalyDetector detector(cfg_);

    BtDataPathObservation obs;
    obs.controller_id = "hci0";
    obs.connection_handle = 70;
    obs.connection_generation = 1;
    obs.attribution_scope = "device";
    obs.device_id = "AA:11:11:11:11:11";
    obs.tx_bytes = 0;
    obs.rx_bytes = 0;

    // 窗口 1
    detector.evaluate({obs}, 1000);

    // 断开并重连，代次变为 2 (新设备复用 handle 70)
    obs.connection_generation = 2;
    obs.device_id = "BB:22:22:22:22:22";

    // 首次观测新代次：静默计数已重置为 0，不继承旧代次的连续计数
    auto a2 = detector.evaluate({obs}, 2000);
    EXPECT_TRUE(a2.empty());
}

TEST_F(BtDataPathAnomalyDetectorTest, RepeatedSilenceDoesNotFloodEmissions) {
    BtDataPathAnomalyDetector detector(cfg_);
    // threshold=2: WARNING 在第 2 个静默窗口跨越，CRITICAL 在第 4 个窗口升级；
    // 此后持续静默不再逐窗口重复上报（板端实测曾每 2 秒刷一条）。

    BtDataPathObservation obs;
    obs.controller_id = "hci0";
    obs.connection_handle = 80;
    obs.connection_generation = 1;
    obs.attribution_scope = "device";
    obs.device_id = "CC:33:44:55:66:77";
    obs.tx_bytes = 0;
    obs.rx_bytes = 0;

    int warning_count = 0, critical_count = 0, other = 0;
    for (int i = 1; i <= 20; ++i) {
        auto anomalies = detector.evaluate({obs}, 1000LL * i);
        for (const auto& a : anomalies) {
            if (a.anomaly_type != "BT_DATAPATH_STALL_SUSPECTED") { other++; continue; }
            if (a.severity == "WARNING") warning_count++;
            else if (a.severity == "CRITICAL") critical_count++;
        }
    }
    EXPECT_EQ(warning_count, 1);   // 第 2 窗口恰好一次
    EXPECT_EQ(critical_count, 1);  // 第 4 窗口恰好一次升级
    EXPECT_EQ(other, 0);

    // 恢复流量后重新武装：再来一轮静默必须能再次上报
    obs.tx_bytes = 100;
    detector.evaluate({obs}, 30000);
    obs.tx_bytes = 0;
    int rearm_warning = 0;
    for (int i = 21; i <= 24; ++i) {
        auto anomalies = detector.evaluate({obs}, 1000LL * i);
        for (const auto& a : anomalies) {
            if (a.anomaly_type == "BT_DATAPATH_STALL_SUSPECTED" && a.severity == "WARNING")
                rearm_warning++;
        }
    }
    EXPECT_EQ(rearm_warning, 1);
}

TEST_F(BtDataPathAnomalyDetectorTest, AttributionLostReportsOncePerEpisode) {
    BtDataPathAnomalyDetector detector(cfg_);

    BtDataPathObservation obs;
    obs.controller_id = "hci0";
    obs.connection_handle = 90;
    obs.connection_generation = 1;
    obs.attribution_scope = "controller";
    obs.device_id = std::nullopt;
    obs.tx_bytes = 300;
    obs.rx_bytes = 100;
    obs.tx_throughput_bps = 24000.0;
    obs.rx_throughput_bps = 8000.0;

    // 持续 controller 归因 + 有流量：只上报一次
    int count = 0;
    for (int i = 1; i <= 5; ++i) {
        auto anomalies = detector.evaluate({obs}, 1000LL * i);
        for (const auto& a : anomalies)
            if (a.anomaly_type == "BT_DATAPATH_ATTRIBUTION_LOST") count++;
    }
    EXPECT_EQ(count, 1);
}

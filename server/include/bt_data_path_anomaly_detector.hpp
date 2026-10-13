/**
 * @file bt_data_path_anomaly_detector.hpp
 * @brief Phase 2 规则驱动工业蓝牙 L2 数据通道异常检测器
 *
 * 核心原则：
 *   1. 规则驱动与配置可控：绝不凭空推断未经验证的底层原因（如缓冲区溢出、队列挂起）。
 *   2. 严格基于 L1 纯净观测事实 (BtDataPathObservation) 进行状态评估。
 *   3. 支持三种确定性异常类型：
 *      - BT_DATAPATH_STALL_SUSPECTED: 明确配置业务心跳周期时，连续超时无数据传输；
 *      - BT_DATAPATH_THROUGHPUT_DEGRADED: 明确配置业务吞吐预期时，持续低于阈值；
 *      - BT_DATAPATH_ATTRIBUTION_LOST: 控制器存在活跃流量，但设备身份不可归因。
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <optional>
#include <chrono>

#include "bt_data_path_analyzer.hpp"

namespace weaknet_dbus {

enum class BtAnomalyType {
    StallSuspected,       ///< 疑似数据流停顿（心跳超时）
    ThroughputDegraded,   ///< 吞吐量持续劣化低于业务基准
    AttributionLost       ///< 归因丢失（控制器有流量但设备未知）
};

inline const char* btAnomalyTypeToString(BtAnomalyType type) {
    switch (type) {
        case BtAnomalyType::StallSuspected:     return "BT_DATAPATH_STALL_SUSPECTED";
        case BtAnomalyType::ThroughputDegraded: return "BT_DATAPATH_THROUGHPUT_DEGRADED";
        case BtAnomalyType::AttributionLost:    return "BT_DATAPATH_ATTRIBUTION_LOST";
    }
    return "UNKNOWN";
}

/**
 * @brief L2 工业蓝牙数据通道异常记录 DTO
 */
struct BtDataPathAnomaly {
    std::string anomaly_type;       ///< 规则名称
    std::string controller_id;      ///< 控制器名
    uint16_t connection_handle{0};  ///< 连接句柄
    uint32_t connection_generation{0}; ///< 连接代次
    std::string device_address;     ///< 设备 MAC（若已知）
    std::string severity{"WARNING"};///< WARNING | CRITICAL
    std::string description;        ///< 人类可读客观描述
    int64_t detected_at_ms{0};      ///< 检出时刻 (Unix 毫秒)

    // 支撑证据 (来源于 L1 观测事实)
    uint64_t tx_bytes{0};
    uint64_t rx_bytes{0};
    double actual_bps{0.0};
    double expected_bps{0.0};
    uint32_t consecutive_windows{0};
    uint32_t max_gap_ms{0};

    std::string toJson() const;
};

/**
 * @brief 异常检测规则参数
 */
struct BtAnomalyDetectorConfig {
    uint32_t stall_timeout_ms{5000};          ///< 业务心跳预期周期（0 表示禁用停顿检测）
    uint32_t stall_consecutive_windows{2};     ///< 触发停顿的连续超时窗口数
    double min_expected_bps{0.0};              ///< 业务保底吞吐量（0 表示未设定预期）
    uint32_t degraded_consecutive_windows{3};  ///< 触发劣化的连续窗口数
};

/**
 * @brief 规则驱动工业蓝牙数据通道异常检测引擎
 */
class BtDataPathAnomalyDetector {
public:
    explicit BtDataPathAnomalyDetector(BtAnomalyDetectorConfig config = {});

    void updateConfig(const BtAnomalyDetectorConfig& config);
    BtAnomalyDetectorConfig config() const;

    /**
     * @brief 评估一批 L1 观测事实并产出当前触发的异常
     * @param observations L1 原始观测切片
     * @param now_ms 当前时间戳（Unix 毫秒）
     * @return 检出的异常列表
     */
    std::vector<BtDataPathAnomaly> evaluate(
        const std::vector<BtDataPathObservation>& observations,
        int64_t now_ms);

    /**
     * @brief 清除指定控制器/句柄的检测历史状态
     */
    void resetConnectionState(const std::string& controller, uint16_t handle);

private:
    struct ConnectionState {
        uint32_t generation{0};
        uint32_t consecutive_stalls{0};
        uint32_t consecutive_degraded{0};
        int64_t last_active_ts_ms{0};
        // 抑制重复上报：只在阈值首次跨越或严重度升级时产出异常，
        // 否则一条持续静默的连接会每 2 秒刷一条异常+一条事件（板端实测踩过）。
        // 0=未上报 1=WARNING 已上报 2=CRITICAL 已上报
        int stall_reported_level{0};
        bool degraded_reported{false};
        bool attribution_reported{false};
    };

    BtAnomalyDetectorConfig config_;
    // key: (controller_id + ":" + handle)
    std::unordered_map<std::string, ConnectionState> conn_states_;
};

} // namespace weaknet_dbus

/**
 * @file bt_data_path_anomaly_detector.cpp
 * @brief Phase 2 规则驱动工业蓝牙 L2 数据通道异常检测器实现
 */

#include "bt_data_path_anomaly_detector.hpp"
#include "utils/json_escape.hpp"
#include <sstream>

namespace weaknet_dbus {

std::string BtDataPathAnomaly::toJson() const {
    std::ostringstream oss;
    oss << "{"
        << "\"anomaly_type\":\"" << weaknet_utils::escapeJsonString(anomaly_type) << "\","
        << "\"controller_id\":\"" << weaknet_utils::escapeJsonString(controller_id) << "\","
        << "\"handle\":" << connection_handle << ","
        << "\"generation\":" << connection_generation << ","
        << "\"device_address\":\"" << weaknet_utils::escapeJsonString(device_address) << "\","
        << "\"severity\":\"" << weaknet_utils::escapeJsonString(severity) << "\","
        << "\"description\":\"" << weaknet_utils::escapeJsonString(description) << "\","
        << "\"detected_at_ms\":" << detected_at_ms << ","
        << "\"evidence\":{"
        << "\"tx_bytes\":" << tx_bytes << ","
        << "\"rx_bytes\":" << rx_bytes << ","
        << "\"actual_bps\":" << actual_bps << ","
        << "\"expected_bps\":" << expected_bps << ","
        << "\"consecutive_windows\":" << consecutive_windows << ","
        << "\"max_gap_ms\":" << max_gap_ms
        << "}"
        << "}";
    return oss.str();
}

BtDataPathAnomalyDetector::BtDataPathAnomalyDetector(BtAnomalyDetectorConfig config)
    : config_(std::move(config)) {}

void BtDataPathAnomalyDetector::updateConfig(const BtAnomalyDetectorConfig& config) {
    config_ = config;
}

BtAnomalyDetectorConfig BtDataPathAnomalyDetector::config() const {
    return config_;
}

void BtDataPathAnomalyDetector::resetConnectionState(const std::string& controller, uint16_t handle) {
    std::string key = controller + ":" + std::to_string(handle & 0x0FFF);
    conn_states_.erase(key);
}

std::vector<BtDataPathAnomaly> BtDataPathAnomalyDetector::evaluate(
    const std::vector<BtDataPathObservation>& observations,
    int64_t now_ms) {

    std::vector<BtDataPathAnomaly> anomalies;

    for (const auto& obs : observations) {
        std::string slot_key = obs.controller_id + ":" + std::to_string(obs.connection_handle & 0x0FFF);
        auto& state = conn_states_[slot_key];

        // 代次跃迁或新连接：状态归零重置
        if (state.generation != obs.connection_generation) {
            state.generation = obs.connection_generation;
            state.consecutive_stalls = 0;
            state.consecutive_degraded = 0;
            state.last_active_ts_ms = now_ms;
            state.stall_reported_level = 0;
            state.degraded_reported = false;
            state.attribution_reported = false;
        }

        const bool has_traffic = (obs.tx_bytes > 0 || obs.rx_bytes > 0);
        const double total_bps = obs.tx_throughput_bps + obs.rx_throughput_bps;

        // -----------------------------------------------------------------
        // 规则 1：BT_DATAPATH_ATTRIBUTION_LOST (归因丢失)
        // 存在真实控制器级传输，但未能绑定到具体设备 BDADDR。
        // 抑制重复：同一连接只在归因丢失期间上报一次，归因恢复或代次变迁后重置。
        // -----------------------------------------------------------------
        if (has_traffic && (obs.attribution_scope == "controller" || obs.attribution_scope == "unknown")) {
            if (!state.attribution_reported) {
                state.attribution_reported = true;
                BtDataPathAnomaly a;
                a.anomaly_type = btAnomalyTypeToString(BtAnomalyType::AttributionLost);
                a.controller_id = obs.controller_id;
                a.connection_handle = obs.connection_handle;
                a.connection_generation = obs.connection_generation;
                a.device_address = obs.device_id.value_or("");
                a.severity = "WARNING";
                a.description = "Controller observed active ACL payload traffic, but device identity could not be reliably attributed.";
                a.detected_at_ms = now_ms;
                a.tx_bytes = obs.tx_bytes;
                a.rx_bytes = obs.rx_bytes;
                a.actual_bps = total_bps;
                a.consecutive_windows = 1;
                a.max_gap_ms = obs.max_gap_ms;
                anomalies.push_back(std::move(a));
            }
        } else {
            state.attribution_reported = false;
        }

        // -----------------------------------------------------------------
        // 规则 2：BT_DATAPATH_STALL_SUSPECTED (疑似数据流停顿)
        // 必须配置了业务心跳预期 (stall_timeout_ms > 0)。
        // 只对已归因到具体设备的连接生效：心跳 SLA 是设备级契约，没有设备
        // 身份就无从谈起；controller 归因的活跃流量异常由规则 1 覆盖。
        // 抑制重复：只在阈值首次跨越上报 WARNING、达到 2 倍阈值再上报一次
        // CRITICAL 升级——持续静默期间不逐窗口重复刷（板端实测每 2 秒一条）。
        // -----------------------------------------------------------------
        if (config_.stall_timeout_ms > 0 &&
            obs.attribution_scope == "device" && obs.device_id.has_value()) {
            if (!has_traffic) {
                state.consecutive_stalls++;
                if (state.consecutive_stalls >= config_.stall_consecutive_windows) {
                    const int level = (state.consecutive_stalls >= config_.stall_consecutive_windows * 2) ? 2 : 1;
                    if (level > state.stall_reported_level) {
                        state.stall_reported_level = level;
                        BtDataPathAnomaly a;
                        a.anomaly_type = btAnomalyTypeToString(BtAnomalyType::StallSuspected);
                        a.controller_id = obs.controller_id;
                        a.connection_handle = obs.connection_handle;
                        a.connection_generation = obs.connection_generation;
                        a.device_address = obs.device_id.value_or("");
                        a.severity = (level == 2) ? "CRITICAL" : "WARNING";
                        a.description = "Consecutive silence windows detected under configured heartbeat SLA expectation.";
                        a.detected_at_ms = now_ms;
                        a.tx_bytes = 0;
                        a.rx_bytes = 0;
                        a.actual_bps = 0.0;
                        a.expected_bps = config_.min_expected_bps;
                        a.consecutive_windows = state.consecutive_stalls;
                        a.max_gap_ms = obs.max_gap_ms;
                        anomalies.push_back(std::move(a));
                    }
                }
            } else {
                state.consecutive_stalls = 0;
                state.stall_reported_level = 0;
                state.last_active_ts_ms = now_ms;
            }
        } else {
            // 非设备归因连接（幻影观测/归因丢失期间）：清空停顿状态，
            // 避免死句柄残留观测在归因恢复后带着旧计数复活误报。
            state.consecutive_stalls = 0;
            state.stall_reported_level = 0;
        }

        // -----------------------------------------------------------------
        // 规则 3：BT_DATAPATH_THROUGHPUT_DEGRADED (吞吐量持续劣化)
        // 必须配置了最低保底预期 (min_expected_bps > 0)，且连接处于通信状态。
        // 抑制重复：自上次恢复起只上报一次，吞吐回到阈值之上后重新武装。
        // -----------------------------------------------------------------
        if (config_.min_expected_bps > 0.0) {
            if (has_traffic && total_bps < config_.min_expected_bps) {
                state.consecutive_degraded++;
                if (state.consecutive_degraded >= config_.degraded_consecutive_windows &&
                    !state.degraded_reported) {
                    state.degraded_reported = true;
                    BtDataPathAnomaly a;
                    a.anomaly_type = btAnomalyTypeToString(BtAnomalyType::ThroughputDegraded);
                    a.controller_id = obs.controller_id;
                    a.connection_handle = obs.connection_handle;
                    a.connection_generation = obs.connection_generation;
                    a.device_address = obs.device_id.value_or("");
                    a.severity = "WARNING";
                    a.description = "Observed throughput consistently below configured operational threshold.";
                    a.detected_at_ms = now_ms;
                    a.tx_bytes = obs.tx_bytes;
                    a.rx_bytes = obs.rx_bytes;
                    a.actual_bps = total_bps;
                    a.expected_bps = config_.min_expected_bps;
                    a.consecutive_windows = state.consecutive_degraded;
                    a.max_gap_ms = obs.max_gap_ms;
                    anomalies.push_back(std::move(a));
                }
            } else if (has_traffic) {
                state.consecutive_degraded = 0;
                state.degraded_reported = false;
            }
        }
    }

    return anomalies;
}

} // namespace weaknet_dbus

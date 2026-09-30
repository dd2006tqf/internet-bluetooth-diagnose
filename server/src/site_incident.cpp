/**
 * @file site_incident.cpp
 * @brief 区域级异常事件领域模型实现（枚举转换 + 合格异常判定）
 */

#include "site_incident.hpp"

namespace weaknet_dbus {

// ============================================================================
// 枚举 <-> 字符串
// ============================================================================

const char* toString(IncidentState state) {
    switch (state) {
        case IncidentState::Open:     return "OPEN";
        case IncidentState::Ongoing:  return "ONGOING";
        case IncidentState::Resolved: return "RESOLVED";
    }
    return "UNKNOWN";
}

IncidentState incidentStateFromString(const std::string& s, IncidentState fallback) {
    if (s == "OPEN")     return IncidentState::Open;
    if (s == "ONGOING")  return IncidentState::Ongoing;
    if (s == "RESOLVED") return IncidentState::Resolved;
    return fallback;
}

// ============================================================================
// 合格异常判定
// ============================================================================

bool QualifyingAnomalyPolicy::isPlannedTermination(DisconnectReason reason) {
    // 计划内断开 = 用户/对端主动结束链路，不是无线环境故障。
    //
    //   RemoteUserTerminated  对端设备主动断开（用户关掉耳机/传感器）
    //   LocalHostTerminated   本机主动断开（用户执行 disconnect）
    //
    // 两者都不计入区域事故。Counter-example：若计入，每天下班整片设备关机
    // 必然产出一条"区域无线故障"——误报一次就足以让运维不再信任整个系统。
    //
    // 刻意**不**排除 Unknown：语义是"已确认断开、但原因不在已知分类内"。
    // 未知不是正常（对照 wireless_event.hpp 的 Other/Unknown 不能成为信息黑洞）。
    return reason == DisconnectReason::RemoteUserTerminated ||
           reason == DisconnectReason::LocalHostTerminated;
}

bool QualifyingAnomalyPolicy::qualify(const WirelessDeviceEvent& event) const {
    switch (event.event_type) {
        case DeviceEventType::LinkDisconnected:
            if (!include_disconnected) return false;
            return !isPlannedTermination(event.reason);

        case DeviceEventType::LinkDegraded:
            // 链路劣化本身就是链路层异常；它没有"原因"字段参与判定。
            return include_degraded;

        // 明确的非异常事实：发现/消失/建链成功/恢复/连接尝试。
        // LinkRecovered 尤其不能计入——"恢复"是故障结束的信号，
        // 把它当成事故证据会让每次故障都自我延续。
        case DeviceEventType::DeviceAppeared:
        case DeviceEventType::DeviceLost:
        case DeviceEventType::LinkConnected:
        case DeviceEventType::LinkRecovered:
        case DeviceEventType::ConnectionAttempt:
            return false;
    }
    return false;
}

}  // namespace weaknet_dbus

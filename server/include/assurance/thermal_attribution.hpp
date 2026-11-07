/**
 * @file thermal_attribution.hpp
 * @brief 热状态归因的结论类型（纯数据，无任何 I/O 依赖）
 *
 * 为什么单独一个头文件而不是塞进 thermal_monitor.hpp：
 *   NetworkExperience 是评估快照的纯数据结构，它只需要知道「归因结论是什么」，
 *   完全不需要、也不应该依赖一个会打开 sysfs 的读取器。把枚举留在这里，
 *   network_experience.hpp 就只多一个几十行的纯头文件依赖，
 *   而不是把 <fstream>/<dirent.h> 一路传染进快照序列化层。
 *
 * 判定逻辑与阈值见 thermal_monitor.hpp 的 attributeThermalState()。
 */

#pragma once

namespace weaknet {

/**
 * @brief 热状态的归因结论
 *
 * 值域刻意保持窄：上层只需要知道「要不要怀疑采集端受限」。
 */
enum class ThermalAttribution {
    /// 本轮未做评估（配置关闭，或尚未采样）
    NONE,

    /// 温度正常、风扇正常、未被节流
    NORMAL,

    /// 频率上限被压低 → 采集端可能来不及处理证据，对观测质量有直接影响
    THROTTLED,

    /// 温度与风扇的匹配关系异常 → 设备可靠性问题，**不影响**观测质量
    /// （例如低温满载：风扇策略过度冷却，但 CPU 频率完全正常）
    FAN_ANOMALY,

    /// 读不到热状态 → 老实说不知道，不得猜
    UNAVAILABLE
};

inline const char* thermalAttributionToString(ThermalAttribution a) {
    switch (a) {
        case ThermalAttribution::NONE: return "NONE";
        case ThermalAttribution::NORMAL: return "NORMAL";
        case ThermalAttribution::THROTTLED: return "THROTTLED";
        case ThermalAttribution::FAN_ANOMALY: return "FAN_ANOMALY";
        case ThermalAttribution::UNAVAILABLE: return "UNAVAILABLE";
        default: return "NONE";
    }
}

}  // namespace weaknet

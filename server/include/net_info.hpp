// net_info.hpp
// 定义 NetInfo 类：存储网络信息与基本比较

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace weaknet_dbus {

enum class NetType {
    Unknown = 0,
    Ethernet,
    WiFi,
    Cellular,
};

enum class NetState {
    Down = 0,
    Up,
};

enum class LinkQuality {
    Unknown = 0,
    Good,
    Fair,
    Poor,
    Bad
};

class NetInfo {
public:
    NetInfo() = default;
    explicit NetInfo(std::string name) : ifname_(std::move(name)) {}

    // 基本属性
    void setIfName(const std::string& n) { ifname_ = n; }
    const std::string& ifName() const { return ifname_; }

    void setDefaultRoute(bool v) { is_default_ = v; }
    bool isDefaultRoute() const { return is_default_; }

    void setType(NetType t) { type_ = t; }
    NetType type() const { return type_; }

    void setRttMs(int rtt) { rtt_ms_ = rtt; }
    int rttMs() const { return rtt_ms_; }

    void setPrevRttMs(int rtt) { prev_rtt_ms_ = rtt; }
    int prevRttMs() const { return prev_rtt_ms_; }

    void setState(NetState s) { state_ = s; }
    NetState state() const { return state_; }

    void setQuality(LinkQuality q) { quality_ = q; }
    LinkQuality quality() const { return quality_; }

    void setRssiDbm(int rssi) { rssi_dbm_ = rssi; }
    int rssiDbm() const { return rssi_dbm_; }

    void setUsingNow(bool v) { using_now_ = v; }
    bool usingNow() const { return using_now_; }

    // TCP丢包率相关
    void setTcpLossRate(double rate) { tcp_loss_rate_ = rate; }
    double tcpLossRate() const { return tcp_loss_rate_; }
    
    void setTcpLossLevel(const std::string& level) { tcp_loss_level_ = level; }
    const std::string& tcpLossLevel() const { return tcp_loss_level_; }
    
    // 流量统计相关
    void setTrafficStats(uint64_t totalBps, uint64_t totalPps, uint32_t activeFlows) {
        traffic_total_bps_ = totalBps;
        traffic_total_pps_ = totalPps;
        traffic_active_flows_ = activeFlows;
    }
    
    uint64_t trafficTotalBps() const { return traffic_total_bps_; }
    uint64_t trafficTotalPps() const { return traffic_total_pps_; }
    uint32_t trafficActiveFlows() const { return traffic_active_flows_; }

    // 网络抖动(Jitter)相关 - RTT的标准差，反映网络延迟稳定性
    void setJitterMs(double jitter) { jitter_ms_ = jitter; }
    double jitterMs() const { return jitter_ms_; }
    
    void setJitterLevel(const std::string& level) { jitter_level_ = level; }
    const std::string& jitterLevel() const { return jitter_level_; }

    // 蓝牙设备距离（米），-1.0 表示未知/未测量
    void setBtDistance(double d) { bt_distance_ = d; }
    double btDistance() const { return bt_distance_; }

    // 蓝牙音频质量等级（excellent/good/fair/poor/unknown）
    void setBtAudioQuality(const std::string& q) { bt_audio_quality_ = q; }
    const std::string& btAudioQuality() const { return bt_audio_quality_; }

    // 2.4GHz 频段冲突检测标志
    void setBandConflict(bool v) { band_conflict_ = v; }
    bool bandConflict() const { return band_conflict_; }

    // 2.4GHz 频段冲突置信度 (0-100%)
    void setBandConflictConfidence(double c) { band_conflict_confidence_ = c; }
    double bandConflictConfidence() const { return band_conflict_confidence_; }

    // 用于比较是否同一网卡（以 ifname 为键）
    bool sameKey(const NetInfo& other) const { return ifname_ == other.ifname_; }

    // 等价比较（所有关键字段）
    bool equals(const NetInfo& other) const {
        return ifname_ == other.ifname_ && is_default_ == other.is_default_ && type_ == other.type_
            && rtt_ms_ == other.rtt_ms_ && state_ == other.state_
            && rssi_dbm_ == other.rssi_dbm_ && tcp_loss_rate_ == other.tcp_loss_rate_
            && traffic_total_bps_ == other.traffic_total_bps_ && traffic_total_pps_ == other.traffic_total_pps_
            && traffic_active_flows_ == other.traffic_active_flows_
            && jitter_ms_ == other.jitter_ms_ && using_now_ == other.using_now_;
    }

    // 检查是否具备蓝牙距离数据
    bool hasBtDistance() const { return bt_distance_ >= 0.0; }

    // ==================== 数据验证接口 ====================

    // 校验当前对象所有字段是否处于合法取值区间（不要求已被采集）
    bool isValid() const;

    // 各指标是否已被采集（按约定：负值/极小值表示未测量）
    bool hasRtt() const { return rtt_ms_ >= 0; }
    bool hasTcpLoss() const { return tcp_loss_rate_ >= 0.0; }
    bool hasRssi() const { return rssi_dbm_ > -1000; }
    bool hasTraffic() const { return traffic_total_bps_ > 0 || traffic_total_pps_ > 0 || traffic_active_flows_ > 0; }
    bool hasJitter() const { return jitter_ms_ >= 0.0; }

    // 是否具备进行质量评估所需的最低指标集合（RTT 与丢包率）
    bool hasEnoughMetricsForAssessment() const { return hasRtt() && hasTcpLoss(); }

    // 与另一对象相比，是否存在需要重新评估/上报的变化
    bool needsUpdate(const NetInfo& other) const;

    // ==================== 序列化/反序列化接口 ====================

    // 序列化为 JSON 字符串（与 NetworkQualityAssessor 输出格式保持兼容）
    std::string toJson() const;

    // 从 JSON 字符串反序列化，失败返回 false 且不修改当前对象
    bool fromJson(const std::string& json);

    // 序列化为二进制缓冲区（复用 serializer 工具函数，含版本号头）
    std::vector<uint8_t> toBinary() const;

    // 从二进制缓冲区反序列化，失败返回 false 且不修改当前对象
    bool fromBinary(const std::vector<uint8_t>& buffer);

private:
    std::string ifname_;
    bool is_default_ = false;
    NetType type_ = NetType::Unknown;
    int rtt_ms_ = -1;
    int prev_rtt_ms_ = -1;
    NetState state_ = NetState::Down;
    bool using_now_ = false;  // 是否当前被判定为"正在上网"的接口
    LinkQuality quality_ = LinkQuality::Unknown;
    int rssi_dbm_ = -1000; // Wi-Fi RSSI (dBm), 非 Wi-Fi 接口保持默认
    double tcp_loss_rate_ = -1.0; // TCP丢包率 (百分比)
    std::string tcp_loss_level_; // TCP丢包率等级 (good/degraded/poor/insufficient)
    
    // 流量统计
    uint64_t traffic_total_bps_ = 0; // 总带宽 (bytes per second)
    uint64_t traffic_total_pps_ = 0; // 总包速率 (packets per second)
    uint32_t traffic_active_flows_ = 0; // 活跃连接数

    // 网络抖动(RTT标准差,毫秒)
    double jitter_ms_ = -1.0;
    std::string jitter_level_; // 抖动等级 (good/degraded/poor/unknown)

    // 蓝牙相关扩展字段
    double bt_distance_ = -1.0;            // 蓝牙设备距离（米），-1 表示未知
    std::string bt_audio_quality_;          // 蓝牙音频质量等级 excellent/good/fair/poor
    bool band_conflict_ = false;            // 是否检测到频段冲突
    double band_conflict_confidence_ = 0.0; // 频段冲突置信度 0-100%
};

}  // namespace weaknet_dbus


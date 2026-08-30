/**
 * @file net_info.hpp
 * @brief 网络接口状态模型：NetInfo 类及其依赖枚举
 *
 * NetInfo 是项目的核心数据结构，封装了单个网络接口的所有可监控指标。
 * WeakNetMgr 维护 std::vector<NetInfo>，DbusService 从中读取数据作为 D-Bus 方法返回值。
 * 各种监控线程（RTT/RSSI/丢包/蓝牙等）通过 WeakNetMgr 的 updateXxxSafe 方法更新 NetInfo 字段。
 *
 * 哨兵值约定：
 *   rtt_ms_       = -1        表示未测量
 *   rssi_dbm_     = -1000     表示未测量（实际 RSSI 范围 -100 ~ -30）
 *   tcp_loss_rate_= -1.0      表示未测量
 *   jitter_ms_    = -1.0      表示未测量
 *   bt_distance_  = -1.0      表示未测量
 *
 * 线程安全：NetInfo 本身非线程安全。所有并发访问必须通过 WeakNetMgr 的
 *   iface_mutex_ 或各 updateXxxSafe 方法保护。
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace weaknet_dbus {

// ==================== 枚举定义 ====================

/// 网络接口类型：用于区分 Wi-Fi / 以太网 / 蜂窝，以采用不同的监控策略
enum class NetType {
    Unknown = 0,    ///< 未能识别类型（netlink 返回未匹配到已知 pattern）
    Ethernet,       ///< 有线以太网（eth0, enp3s0 等）
    WiFi,           ///< 无线局域网（wlan0, wlp2s0 等，支持 RSSI 和频段冲突检测）
    Cellular,       ///< 蜂窝网络（rmnet_data0 等，暂未深度支持）
};

/// 接口状态：对应 netlink RTM_IFINFO 的 IFF_UP flag
enum class NetState {
    Down = 0,   ///< 接口关闭或未激活（IFF_UP 未设置）
    Up,         ///< 接口开启并激活
};

/// 链路质量等级：由 NetworkQualityAssessor 综合评分后判定
enum class LinkQuality {
    Unknown = 0,    ///< 数据不足，无法评估
    Good,           ///< 链路质量好（所有指标在健康范围内）
    Fair,           ///< 链路质量一般（个别指标轻度劣化）
    Poor,           ///< 链路质量差（多个指标明显劣化，已影响体验）
    Bad             ///< 链路质量极差（基本不可用）
};

// ==================== NetInfo 类 ====================

/**
 * @brief 单个网络接口的完整状态快照
 *
 * 字段涵盖：接口标识 → 状态 → 性能指标（RTT/RSSI/丢包/流量/抖动）→ 蓝牙扩展。
 * 使用 setter/getter 模式（多数为 inline 实现），所有字段有明确的哨兵默认值。
 *
 * 序列化：支持 JSON 和二进制两种格式，供持久化和 D-Bus 传输使用。
 */
class NetInfo {
public:
    NetInfo() = default;

    /// 带接口名构造：最常用入口（WeakNetMgr 初始化接口列表时使用）
    explicit NetInfo(std::string name) : ifname_(std::move(name)) {}

    // ---------- 基本标识属性 ----------
    void setIfName(const std::string& n) { ifname_ = n; }
    const std::string& ifName() const { return ifname_; }

    void setDefaultRoute(bool v) { is_default_ = v; }
    bool isDefaultRoute() const { return is_default_; }

    void setType(NetType t) { type_ = t; }
    NetType type() const { return type_; }

    // ---------- RTT 延迟（ICMP Ping 结果，单位 ms）----------
    void setRttMs(int rtt) { rtt_ms_ = rtt; }
    int rttMs() const { return rtt_ms_; }

    void setPrevRttMs(int rtt) { prev_rtt_ms_ = rtt; }
    int prevRttMs() const { return prev_rtt_ms_; }

    // ---------- 接口状态 ----------
    void setState(NetState s) { state_ = s; }
    NetState state() const { return state_; }

    // ---------- 综合链路质量（NetworkQualityAssessor 判定结果）----------
    void setQuality(LinkQuality q) { quality_ = q; }
    LinkQuality quality() const { return quality_; }

    // ---------- Wi-Fi 信号强度（单位 dBm，仅对 WiFi 类型有意义）----------
    void setRssiDbm(int rssi) { rssi_dbm_ = rssi; }
    int rssiDbm() const { return rssi_dbm_; }

    /// 是否当前正在上网的接口（WeakNetMgr 通过默认路由判定）
    void setUsingNow(bool v) { using_now_ = v; }
    bool usingNow() const { return using_now_; }

    // ---------- TCP 丢包率 ----------
    /// 丢包率（百分比，0-100）。由 TcpLossMonitor 通过 eBPF tcp_retransmit 差分计算
    void setTcpLossRate(double rate) { tcp_loss_rate_ = rate; }
    double tcpLossRate() const { return tcp_loss_rate_; }

    /// 丢包率等级（good / degraded / poor / insufficient）。由 TcpLossMonitor 内部判定
    void setTcpLossLevel(const std::string& level) { tcp_loss_level_ = level; }
    const std::string& tcpLossLevel() const { return tcp_loss_level_; }

    // ---------- 流量统计 ----------
    /// 带宽（bytes per second）+ 包速率（packets per second）+ 活跃连接数
    void setTrafficStats(uint64_t totalBps, uint64_t totalPps, uint32_t activeFlows) {
        traffic_total_bps_ = totalBps;
        traffic_total_pps_ = totalPps;
        traffic_active_flows_ = activeFlows;
    }
    uint64_t trafficTotalBps() const { return traffic_total_bps_; }
    uint64_t trafficTotalPps() const { return traffic_total_pps_; }
    uint32_t trafficActiveFlows() const { return traffic_active_flows_; }

    // ---------- 网络抖动（Jitter = RTT 样本标准差，单位 ms）----------
    void setJitterMs(double jitter) { jitter_ms_ = jitter; }
    double jitterMs() const { return jitter_ms_; }

    void setJitterLevel(const std::string& level) { jitter_level_ = level; }
    const std::string& jitterLevel() const { return jitter_level_; }

    // ---------- 蓝牙扩展字段 ----------
    // 蓝牙设备距离（米），-1.0 表示未知/未测量（RSSI→距离估算）
    void setBtDistance(double d) { bt_distance_ = d; }
    double btDistance() const { return bt_distance_; }

    // 蓝牙音频质量等级（excellent/good/fair/poor/unknown，由 BtAudioFusion 综合评估）
    void setBtAudioQuality(const std::string& q) { bt_audio_quality_ = q; }
    const std::string& btAudioQuality() const { return bt_audio_quality_; }

    // 2.4GHz 频段冲突检测标志（同频段存在 Wi-Fi / 蓝牙设备过多）
    void setBandConflict(bool v) { band_conflict_ = v; }
    bool bandConflict() const { return band_conflict_; }

    /// 频段冲突置信度 0-100%（BandConflictDetector 基于 RSSI 密度估算）
    void setBandConflictConfidence(double c) { band_conflict_confidence_ = c; }
    double bandConflictConfidence() const { return band_conflict_confidence_; }

    // ---------- 比较工具 ----------

    /**
     * @brief 键比较：两个 NetInfo 是否代表同一物理接口（以 ifname 为唯一键）
     *
     * 用于 WeakNetMgr 在接口列表中查找/替换指定接口的条目。
     * 注意：即使字段值不同，sameKey 仍返回 true。
     */
    bool sameKey(const NetInfo& other) const { return ifname_ == other.ifname_; }

    /**
     * @brief 值比较：所有关键字段是否完全一致
     *
     * 用于 WeakNetMgr 判断接口列表是否需要发射 Changed 信号。
     * 注意：equals 未包含蓝牙扩展字段（bt_distance_/band_conflict_ 等），
     * 因为蓝牙数据是独立监测的，与接口本身的网络状态不是同一层级。
     */
    bool equals(const NetInfo& other) const {
        return ifname_ == other.ifname_ && is_default_ == other.is_default_ && type_ == other.type_
            && rtt_ms_ == other.rtt_ms_ && state_ == other.state_
            && rssi_dbm_ == other.rssi_dbm_ && tcp_loss_rate_ == other.tcp_loss_rate_
            && traffic_total_bps_ == other.traffic_total_bps_ && traffic_total_pps_ == other.traffic_total_pps_
            && traffic_active_flows_ == other.traffic_active_flows_
            && jitter_ms_ == other.jitter_ms_ && using_now_ == other.using_now_;
    }

    /// 是否具备蓝牙距离数据（bt_distance_ >= 0.0 表示已测量）
    bool hasBtDistance() const { return bt_distance_ >= 0.0; }

    // ---------- 数据验证接口 ----------

    /**
     * @brief 校验对象所有字段是否处于合法取值区间（不要求已被采集）
     *
     * 检查：RTT 不应超过 60000ms（1 分钟）、RSSI 范围 [-1000, 0]、丢包率 [0, 100] 等。
     * @return true 所有字段在合法区间内；false 发现异常值（可能是上游监控器 bug）
     */
    bool isValid() const;

    /// 各指标哨兵值检查：负值或极小值表示该指标尚未被对应监控器采集
    bool hasRtt() const { return rtt_ms_ >= 0; }
    bool hasTcpLoss() const { return tcp_loss_rate_ >= 0.0; }
    bool hasRssi() const { return rssi_dbm_ > -1000; }
    bool hasTraffic() const { return traffic_total_bps_ > 0 || traffic_total_pps_ > 0 || traffic_active_flows_ > 0; }
    bool hasJitter() const { return jitter_ms_ >= 0.0; }

    /// 是否具备质量评估所需的最低指标集合（RTT + 丢包率，二者由不同监控器独立采集）
    bool hasEnoughMetricsForAssessment() const { return hasRtt() && hasTcpLoss(); }

    /**
     * @brief 与另一 NetInfo 相比，是否存在需要重新评估/上报的变化
     *
     * 内部调用 equals() 的反逻辑，但可能增加对蓝牙扩展字段的额外检查。
     * @param other 上一次快照
     */
    bool needsUpdate(const NetInfo& other) const;

    // ---------- 序列化接口 ----------

    /// 序列化为 JSON 字符串（格式与 NetworkQualityAssessor 输出兼容，可直接发给客户端）
    std::string toJson() const;

    /**
     * @brief 从 JSON 字符串反序列化
     * @param json 输入 JSON 文本
     * @return true 成功，false 解析失败且不修改当前对象（原子性保证）
     */
    bool fromJson(const std::string& json);

    /// 序列化为二进制缓冲区（复用 serializer 工具函数，含版本号头）
    std::vector<uint8_t> toBinary() const;

    /// 从二进制缓冲区反序列化（同 fromJson 的原子性保证）
    bool fromBinary(const std::vector<uint8_t>& buffer);

private:
    std::string ifname_;           ///< 接口名（唯一标识，如 "wlan0", "eth0"）
    bool is_default_ = false;      ///< 是否默认路由接口（有 internet 连接的可能候选）
    NetType type_ = NetType::Unknown;
    int rtt_ms_ = -1;              ///< RTT 延迟（ms），-1 表示未测量
    int prev_rtt_ms_ = -1;         ///< 上一次 RTT 值（用于变化检测和抖动计算）
    NetState state_ = NetState::Down;
    bool using_now_ = false;       ///< 当前是否被判定为"正在上网"（WeakNetMgr::updateCurrentUsing 更新）
    LinkQuality quality_ = LinkQuality::Unknown;
    int rssi_dbm_ = -1000;         ///< Wi-Fi RSSI (dBm)，-1000 表示未测量

    // 性能指标
    double tcp_loss_rate_ = -1.0;          ///< TCP 丢包率（%），-1.0 表示未测量
    std::string tcp_loss_level_;           ///< 丢包率等级文本

    uint64_t traffic_total_bps_ = 0;       ///< 总带宽（bytes/s）
    uint64_t traffic_total_pps_ = 0;       ///< 总包速率（packets/s）
    uint32_t traffic_active_flows_ = 0;    ///< 活跃连接数

    double jitter_ms_ = -1.0;              ///< 抖动（ms，RTT 标准差），-1.0 表示未测量
    std::string jitter_level_;             ///< 抖动等级文本

    // 蓝牙扩展字段（与接口本身的网络状态独立）
    double bt_distance_ = -1.0;            ///< 蓝牙设备距离（米），-1 表示未测量
    std::string bt_audio_quality_;          ///< 蓝牙音频质量等级
    bool band_conflict_ = false;            ///< 是否检测到 2.4GHz 频段冲突
    double band_conflict_confidence_ = 0.0; ///< 频段冲突置信度 0-100%
};

}  // namespace weaknet_dbus

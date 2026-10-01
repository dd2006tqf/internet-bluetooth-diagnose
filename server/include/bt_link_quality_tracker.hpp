/**
 * @file bt_link_quality_tracker.hpp
 * @brief 蓝牙链路质量跟踪器（相对基线计算、三态状态机、因果 RSSI 检索）
 *
 * 核心职责：
 *   1. 接收 D-Bus PropertiesChanged 信号喂入的真实 Fresh RSSI（feedFreshRssi）
 *   2. 接收轮询快照（registerDeviceSnapshot），仅做发现/首末见记录，绝不计入基线与连续计数
 *   3. 纯相对基线：以自身历史滑动中位数（上限 30 点）为准，不设统一绝对阈值
 *   4. 三态状态机：LEARNING -> STABLE <-> DEGRADED
 *      - 异常判断严格先于基线学习（劣质样本不污染正常基线）
 *      - DEGRADED 期间基线窗口彻底冻结
 *      - 阈值 + 时间连续性双防抖（相邻样本间隔 <= max_fresh_gap_ms）
 *   5. 状态边沿发射 LinkDegraded / LinkRecovered 业务事件
 *   6. 断连前有效 RSSI 的因果回填（getRssiBeforeEvent）：
 *      按断连事件发生时刻在内核单调时钟域向前检索（sample.mono_ns <= event_mono_ns 且差值 <= ttl）
 *   7. 长期离线重学策略（baseline_stale_after_ms，默认 24h）
 *
 * 线程安全：内部独立 mutex_ 保护，不依赖或嵌套外部锁。
 */

#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "wireless_event.hpp"

namespace weaknet_dbus {

class BtLinkQualityTracker {
public:
    explicit BtLinkQualityTracker(BtLinkQualityConfig cfg = {});
    ~BtLinkQualityTracker() = default;

    BtLinkQualityTracker(const BtLinkQualityTracker&) = delete;
    BtLinkQualityTracker& operator=(const BtLinkQualityTracker&) = delete;

    /**
     * @brief 喂入来自 BlueZ PropertiesChanged 信号的真实 Fresh RSSI 观测
     *
     * @param key        复合设备身份（网关/HCI/地址类型/MAC）
     * @param rssi_dbm   采集到的信号强度（须 > -1000 且 != 0）
     * @param wall_ms    墙钟时间戳（毫秒）
     * @param mono_ns    单调时间戳（CLOCK_MONOTONIC 纳秒）
     * @return 若发生状态边沿切换，返回产出的规范化事件（LinkDegraded 或 LinkRecovered），
     *         否则返回 std::nullopt
     */
    std::optional<WirelessDeviceEvent> feedFreshRssi(
        const WirelessDeviceKey& key,
        int16_t rssi_dbm,
        uint64_t wall_ms,
        uint64_t mono_ns);

    /**
     * @brief 轮询发现设备时登记快照
     *
     * 强类型防误用：本接口仅更新 first_seen/last_seen 时间戳，
     * 严禁将轮询缓存当成真实 Fresh RSSI，不推进基线，不推进连续坏/好采样计数。
     */
    void registerDeviceSnapshot(
        const WirelessDeviceKey& key,
        uint64_t wall_ms,
        uint64_t mono_ns);

    /**
     * @brief 严格时序因果检索：获取断连发生前在 TTL 内的最近有效 RSSI
     *
     * 判定准则：
     *   1. sample.monotonic_ns <= event_monotonic_ns（绝不取断连之后的新样本）
     *   2. (event_monotonic_ns - sample.monotonic_ns) <= ttl_ns（默认 10s）
     *   3. 满足条件的样本中取单调时间最晚（最贴近断开瞬时）的一条
     *
     * @return 命中的 RSSI dBm；若超时或无满足条件的样本，返回 std::nullopt（落库为 NULL）
     */
    std::optional<int16_t> getRssiBeforeEvent(
        const WirelessDeviceKey& key,
        uint64_t event_monotonic_ns,
        uint64_t ttl_ns = 10000000000ULL) const;

    /**
     * @brief 重启服务预热加载历史基线（Warm-start 策略）
     *
     * 旧基线作为异常判定 reference 载入内存，避免重启初期监控盲区；
     * 同时内存重新累积 fresh 样本，累积满 min_baseline_samples 后平滑替换。
     */
    void loadWarmStartBaseline(const DeviceLinkProfile& profile);

    /// 获取单个设备当前链路画像
    std::optional<DeviceLinkProfile> getProfile(const WirelessDeviceKey& key) const;

    /// 获取全部已知设备链路画像快照
    std::vector<DeviceLinkProfile> getAllProfiles() const;

    /// 获取算法配置
    const BtLinkQualityConfig& config() const { return cfg_; }

    /// 重置所有内存状态（测试或适配器重置时用）
    void reset();

private:
    struct DeviceState {
        WirelessDeviceKey key;
        LinkQualityState state = LinkQualityState::Learning;

        // --- 采样时序缓冲（用于断连因果回填，保留最近 N 个，带单调时间） ---
        static constexpr size_t kMaxSampleHistory = 30;
        std::deque<BtLinkRssiSample> recent_samples;

        // --- 可信基线滑动窗口（仅限 STABLE 期间的非恶化样本） ---
        std::deque<int16_t> baseline_window;
        std::optional<int16_t> warm_start_baseline_dbm; // 重启加载的预热基线值

        // --- 极值与统计 ---
        std::optional<int16_t> min_seen_rssi_dbm;
        std::optional<int16_t> max_seen_rssi_dbm;
        uint64_t first_seen_ms = 0;
        uint64_t last_seen_ms = 0;
        uint64_t last_seen_mono_ns = 0;

        // --- 防抖连续计数 ---
        size_t bad_streak = 0;
        uint64_t last_bad_sample_mono_ns = 0;

        size_t good_streak = 0;
        uint64_t last_good_sample_mono_ns = 0;

        /// 计算当前参考基线（若有成熟中位数取中位数，否则取 warm-start）
        std::optional<int16_t> currentReferenceBaseline(size_t min_samples) const;
    };

    /// 辅助：计算整型向量的中位数
    static int16_t calculateMedian(std::vector<int16_t> values);

    BtLinkQualityConfig cfg_;
    mutable std::mutex mutex_;
    std::map<WirelessDeviceKey, DeviceState> devices_;
    uint64_t event_seq_ = 0;
};

}  // namespace weaknet_dbus

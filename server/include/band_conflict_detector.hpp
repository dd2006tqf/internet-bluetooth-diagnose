/**
 * @file band_conflict_detector.hpp
 * @brief 2.4GHz 频段冲突检测器
 *
 * Wi-Fi 和蓝牙都工作在 2.4GHz ISM 频段，当两者同时活跃时可能产生互扰。
 * 本检测器通过关联 Wi-Fi RSSI 和蓝牙 RSSI 时间序列来识别频段冲突：
 *
 *   正常情况：Wi-Fi 与蓝牙 RSSI 各自独立变化（Pearson 相关系数低）
 *   频段冲突：两者同时显著下降，且下降趋势高度相关（Pearson > 0.7）
 *
 * 算法流程：
 *   1. feedSample() 推入一对 (wifi_rssi, bt_rssi) 样本，维护最近 30 个样本的时间序列
 *   2. detect() 计算：
 *      a. 用 BASELINE_SAMPLES（20 个）样本计算各自的基线均值
 *      b. 若两者同时低于基线 DROP_THRESHOLD_DB（10dBm）以上 → 疑似冲突
 *      c. 计算 Pearson 相关系数，> CORRELATION_THRESHOLD（0.7）确认冲突
 *      d. 综合降幅与相关性输出置信度（0-100%）
 *
 * 线程安全：detect() 是 const，前提是调用方保证它与 feedSample() 不同时执行。
 *           若需多线程安全，调用方应在外层加锁。
 */

#pragma once

#include <deque>
#include <string>
#include <chrono>
#include <cstddef>

namespace weaknet_dbus {

// ============================================================================
// 频段冲突检测结果
// ============================================================================
struct BandConflictResult {
    bool detected = false;          ///< 是否检测到冲突（detect() 的最终判定）
    double confidence = 0.0;        ///< 置信度 0-100%（综合降幅与相关性）
    double correlation = 0.0;       ///< Pearson 相关系数 [-1.0, 1.0]
    int wifiRssiDrop = 0;           ///< Wi-Fi RSSI 降幅 (dBm)，非负值
    int btRssiDrop = 0;             ///< 蓝牙 RSSI 降幅 (dBm)，非负值
    std::string suggestion;         ///< 处置建议（如 "切换 Wi-Fi 到 5GHz"）
    std::chrono::system_clock::time_point timestamp;  ///< 检测时间戳
};

// ============================================================================
// 频段冲突检测器
//
// 用法：
//   // 每个采样周期：
//   detector.feedSample(wifi_rssi, bt_rssi);
//   // 每隔一段时间查询：
//   auto result = detector.detect();
//   if (result.detected) { ... }
// ============================================================================
class BandConflictDetector {
public:
    BandConflictDetector() = default;

    /**
     * @brief 推入一对 RSSI 样本
     *
     * 无效值（=0 或 <= -1000 哨兵值）会被静默跳过，不推入历史队列。
     * 内部维护最多 MAX_HISTORY（30）个样本，超出时自动丢弃最旧样本。
     *
     * @param wifiRssi  Wi-Fi 当前 RSSI (dBm)，须 > -1000 且 != 0 才有效
     * @param btRssi    蓝牙当前 RSSI (dBm)，须 > -1000 且 != 0 才有效
     */
    void feedSample(int wifiRssi, int btRssi);

    /**
     * @brief 基于当前历史计算冲突检测结果
     *
     * @return 检测结果。样本不足 MIN_SAMPLES（5 个）时返回 detected=false 的空结果。
     *
     * @note 该方法是 const 的，线程安全前提是调用方保证在 detect() 期间
     *       没有其他线程同时调用 feedSample()。若需多线程安全，外层加锁。
     */
    BandConflictResult detect() const;

    /**
     * @brief 根据检测结果生成处置建议文本
     *
     * @param r  检测结果，detected=false 时返回空字符串
     * @return 人类可读的处置建议（如 "切换 Wi-Fi 到 5GHz 频道"）
     */
    static std::string generateSuggestion(const BandConflictResult& r);

    /// 重置所有历史数据（适配器重启、D-Bus 重连等场景）
    void reset();

    /// 当前已收集的有效样本数量（取两者中较小值）
    size_t sampleCount() const { return std::min(wifiHistory_.size(), btHistory_.size()); }

private:
    // ---- 历史队列 ----
    std::deque<int> wifiHistory_;  ///< Wi-Fi RSSI 历史（最近 30 个有效样本）
    std::deque<int> btHistory_;    ///< 蓝牙 RSSI 历史

    // ---- 可调阈值常量 ----
    static constexpr size_t MAX_HISTORY = 30;          ///< 最大历史样本数
    static constexpr size_t MIN_SAMPLES = 5;           ///< 最少样本数，不足则不输出结论
    static constexpr size_t BASELINE_SAMPLES = 20;     ///< 用于计算基线的样本数（取最近 N 个）
    static constexpr int DROP_THRESHOLD_DB = 10;       ///< 低于基线此值标记为异常 (dBm)
    static constexpr double CORRELATION_THRESHOLD = 0.7; ///< Pearson 确认阈值（> 0.7 认为强相关）

    /**
     * @brief 计算队列中最近 n 个样本的均值作为基线
     * @param h  历史队列
     * @param n  取最近 n 个样本（实际取 min(n, h.size())）
     * @return 均值；若队列为空或 n 为 0，返回 0.0
     */
    double baseline(const std::deque<int>& h, size_t n) const;

    /**
     * @brief 计算两个队列的 Pearson 相关系数
     *
     * 取两者中较短的长度作为 n，从队列末尾取最近 n 个样本计算。
     *
     * @param x  Wi-Fi 历史队列
     * @param y  蓝牙历史队列
     * @return 相关系数 [-1.0, 1.0]；样本不足或分母为零时返回 0.0
     */
    double pearson(const std::deque<int>& x, const std::deque<int>& y) const;
};

}  // namespace weaknet_dbus

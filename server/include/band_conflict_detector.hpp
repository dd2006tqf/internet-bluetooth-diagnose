// band_conflict_detector.hpp
// 2.4GHz 频段冲突检测器：关联 Wi-Fi RSSI 与蓝牙 RSSI，识别共享频段干扰
//
// 检测原理：
//   正常情况：Wi-Fi 与蓝牙 RSSI 各自独立变化
//   频段冲突：两者同时显著下降，且下降趋势高度相关 (Pearson > 0.7)
//
// 算法流程：
//   1. 维护 Wi-Fi 与蓝牙 RSSI 各 30 个样本的时间序列
//   2. 用前 20 个样本计算基线均值
//   3. 当两者同时低于基线 10dBm 以上 → 标记为疑似冲突
//   4. 计算 Pearson 相关系数，>0.7 确认冲突
//   5. 综合降幅与相关性输出置信度 (0-100%)

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
    bool detected = false;          // 是否检测到冲突
    double confidence = 0.0;        // 置信度 0-100%
    double correlation = 0.0;       // Pearson 相关系数
    int wifiRssiDrop = 0;           // Wi-Fi RSSI 降幅 (dBm)，非负值
    int btRssiDrop = 0;             // 蓝牙 RSSI 降幅 (dBm)，非负值
    std::string suggestion;         // 处置建议
    std::chrono::system_clock::time_point timestamp;
};

// ============================================================================
// 频段冲突检测器
// ============================================================================
class BandConflictDetector {
public:
    BandConflictDetector() = default;

    // ------------------------------------------------------------------
    // 推入一对 RSSI 样本
    // @param wifiRssi  Wi-Fi 当前 RSSI (dBm)，须 > -1000 且 != 0 才有效
    // @param btRssi    蓝牙当前 RSSI (dBm)，须 > -1000 且 != 0 才有效
    //
    // 无效值（=0 或 <= -1000）会被静默跳过，不推入历史队列。
    // 内部维护最多 30 个样本，超出时自动丢弃最旧样本。
    // ------------------------------------------------------------------
    void feedSample(int wifiRssi, int btRssi);

    // ------------------------------------------------------------------
    // 基于当前历史计算冲突检测结果
    // @return 检测结果。样本不足 5 个时返回 detected=false 的空结果。
    //
    // 该方法是 const 的，线程安全前提是调用方保证在 detect() 期间
    // 没有其他线程同时调用 feedSample()。
    // ------------------------------------------------------------------
    BandConflictResult detect() const;

    // ------------------------------------------------------------------
    // 根据检测结果生成处置建议文本
    // @param r  检测结果，detected=false 时返回空字符串
    // @return 人类可读的处置建议
    // ------------------------------------------------------------------
    static std::string generateSuggestion(const BandConflictResult& r);

    // ------------------------------------------------------------------
    // 重置所有历史数据（用于适配器重启、D-Bus 重连等场景）
    // ------------------------------------------------------------------
    void reset();

    // ------------------------------------------------------------------
    // 查询当前已收集的样本数量
    // ------------------------------------------------------------------
    size_t sampleCount() const { return std::min(wifiHistory_.size(), btHistory_.size()); }

private:
    // 历史队列，各维护最近 MAX_HISTORY 个样本
    std::deque<int> wifiHistory_;
    std::deque<int> btHistory_;

    // 可调阈值常量
    static constexpr size_t MAX_HISTORY = 30;          // 最大历史样本数
    static constexpr size_t MIN_SAMPLES = 5;           // 最少样本数，不足则不输出结论
    static constexpr size_t BASELINE_SAMPLES = 20;     // 用于计算基线的样本数
    static constexpr int DROP_THRESHOLD_DB = 10;       // 低于基线此值标记为异常 (dBm)
    static constexpr double CORRELATION_THRESHOLD = 0.7; // Pearson 确认阈值

    // ------------------------------------------------------------------
    // 计算队列中最近 n 个样本的均值作为基线
    // @param h  历史队列
    // @param n  取最近 n 个样本
    // @return 均值；若队列为空或 n 为 0，返回 0.0
    // ------------------------------------------------------------------
    double baseline(const std::deque<int>& h, size_t n) const;

    // ------------------------------------------------------------------
    // 计算两个队列的 Pearson 相关系数
    // @param x  Wi-Fi 历史队列
    // @param y  蓝牙历史队列
    // @return 相关系数 [-1.0, 1.0]；样本不足或分母为零时返回 0.0
    //
    // 取两者中较短的长度作为 n，从队列末尾取最近 n 个样本计算。
    // ------------------------------------------------------------------
    double pearson(const std::deque<int>& x, const std::deque<int>& y) const;
};

}  // namespace weaknet_dbus
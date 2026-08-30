/**
 * @file band_conflict_detector.cpp
 * @brief 2.4GHz 频段冲突检测器 — 分析同频段 Wi-Fi 与蓝牙 RSSI 相关性，判断是否存在射频干扰
 *
 * 模块职责：
 *   - 接收 Wi-Fi RSSI 与蓝牙 RSSI 时间序列样本（feedSample）
 *   - 在样本不足、基线异常、降幅未达阈值等场景下返回空结果（保守）
 *   - 通过 3 层判据确认冲突：降幅阈值检查 → Pearson 相关系数验证 → 综合置信度评分
 *   - 冲突发生时自动生成中文建议（[band_conflict] 前缀供信号载荷识别）
 *
 * 2.4GHz 频段冲突检测原理：
 *   Wi-Fi（802.11 b/g/n）和蓝牙（BT Classic/BLE）均工作在 2.4GHz ISM 频段，
 *   当两者同时传输时会发生射频冲突，表现为双方 RSSI 同时下降。
 *
 *   检测算法流程（detect()）：
 *     1. 样本检查：MIN_SAMPLES=10，若历史队列不足直接返回
 *     2. 基线计算：取最近 BASELINE_SAMPLES=20 个样本的均值作为"无冲突时"的参考值
 *     3. 当前值计算：取最近 3 个样本均值，降低毛刺影响
 *     4. 降幅计算：wifiDrop = wifiBaseline - wifiCurrent（正值表示下降 dBm 数）
 *     5. 降幅阈值：双方同时下降 ≥ DROP_THRESHOLD_DB=6dBm 才可能构成冲突
 *     6. Pearson 相关性验证：r = Σ(x-x̄)(y-ȳ) / sqrt(Σ(x-x̄)² Σ(y-ȳ)²)
 *        若 Wi-Fi/BT RSSI 下降是独立事件（如 Wi-Fi 本身离得远），两者不会高度相关；
 *        若为同一射频干扰源，r 应显著接近 -1（Wi-Fi↓ ↔ BT↓ 高度同步）
 *     7. 相关性门槛：CORRELATION_THRESHOLD=-0.4（负相关足够显著）
 *     8. 置信度综合：
 *        dropScore = min(min(wifiDrop, btDrop), 20) / 20 × 50     （降幅贡献 50%）
 *        corrScore = |correlation| × 50                             （相关性贡献 50%）
 *        confidence = dropScore + corrScore
 *     9. 最终门槛：confidence ≥ 50 才输出结论
 *
 * 协作关系：
 *   - 由 WeakNetMgr / Server 主循环周期性喂入样本（Wi-Fi RSSI 来自 wpa_supplicant ctrl socket，
 *     蓝牙 RSSI 来自 BtMonitor::getRssiSnapshot()）
 *   - detect() 结果通过 EventManager 发射 NetworkQualityChanged 信号，
 *     载荷含 [band_conflict] 前缀便于前端识别冲突类型
 */

#include "band_conflict_detector.hpp"
#include "logger.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>

namespace weaknet_dbus {

// ============================================================================
// 内部辅助函数
// ============================================================================

namespace {

// 判断 RSSI 值是否有效（非 0 且非哨兵值 -1000）
inline bool isValidRssi(int rssi) {
    return rssi != 0 && rssi > -1000;
}

}  // anonymous namespace

// ============================================================================
// feedSample
// ============================================================================

void BandConflictDetector::feedSample(int wifiRssi, int btRssi) {
    // 边界条件：无效 RSSI 值直接跳过，不污染历史队列
    if (!isValidRssi(wifiRssi) || !isValidRssi(btRssi)) {
        LOG_INFO(LogModule::WEAK_MGR, "feedSample: skipping invalid RSSI values (wifi=" << wifiRssi << ", bt=" << btRssi << ")");
        return;
    }

    // 限制队列长度，超出时自动丢弃最旧样本
    if (wifiHistory_.size() >= MAX_HISTORY) {
        wifiHistory_.pop_front();
    }
    if (btHistory_.size() >= MAX_HISTORY) {
        btHistory_.pop_front();
    }

    wifiHistory_.push_back(wifiRssi);
    btHistory_.push_back(btRssi);
}

// ============================================================================
// detect
// ============================================================================

BandConflictResult BandConflictDetector::detect() const {
    BandConflictResult result;

    // 取有效样本数（两者取较小值）
    size_t n = std::min(wifiHistory_.size(), btHistory_.size());

    // 边界条件：样本不足时不输出结论，避免小样本误判
    if (n < MIN_SAMPLES) {
        return result;  // detected=false 的空结果
    }

    // 计算基线：取前 BASELINE_SAMPLES 个样本（或全部样本，若不足 20）
    size_t baselineN = std::min(n, BASELINE_SAMPLES);
    double wifiBaseline = baseline(wifiHistory_, baselineN);
    double btBaseline = baseline(btHistory_, baselineN);

    // 基线为 0 表示数据异常（理论上 RSSI 不会为 0），跳过检测
    if (wifiBaseline >= 0.0 || btBaseline >= 0.0) {
        return result;
    }

    // 计算当前 RSSI（取最近 3 个样本均值，平滑毛刺）
    size_t recentN = std::min(n, size_t(3));
    double wifiCurrent = baseline(wifiHistory_, 0);  // 技巧：用 0 表示取最近 1 个
    // 明确取最近 3 个样本均值
    {
        double sum = 0;
        size_t count = 0;
        auto it = wifiHistory_.rbegin();
        for (size_t i = 0; i < recentN && it != wifiHistory_.rend(); ++i, ++it) {
            sum += *it;
            ++count;
        }
        wifiCurrent = (count > 0) ? sum / count : wifiBaseline;
    }
    double btCurrent;
    {
        double sum = 0;
        size_t count = 0;
        auto it = btHistory_.rbegin();
        for (size_t i = 0; i < recentN && it != btHistory_.rend(); ++i, ++it) {
            sum += *it;
            ++count;
        }
        btCurrent = (count > 0) ? sum / count : btBaseline;
    }

    // 计算降幅（dBm，正值表示下降了多少 dB）
    // 注意：RSSI 是负数，基线也是负数，所以 baseline - current 为正值表示下降
    int wifiDrop = static_cast<int>(wifiBaseline - wifiCurrent);
    int btDrop = static_cast<int>(btBaseline - btCurrent);

    // 降幅为负值或零表示没有变差，不构成冲突
    if (wifiDrop <= 0 || btDrop <= 0) {
        return result;
    }

    // 异常判据：两者同时低于基线 DROP_THRESHOLD_DB 以上
    if (wifiDrop < DROP_THRESHOLD_DB || btDrop < DROP_THRESHOLD_DB) {
        return result;
    }

    // 相关性验证：Pearson 系数
    double correlation = pearson(wifiHistory_, btHistory_);

    // 相关性不足，不确认冲突
    if (correlation < CORRELATION_THRESHOLD) {
        return result;
    }

    // 综合置信度：降幅贡献 50% + 相关性贡献 50%
    // 降幅归一化：以 20dBm 为满分基准
    double dropScore = std::min(static_cast<double>(std::min(wifiDrop, btDrop)), 20.0) / 20.0 * 50.0;
    double corrScore = correlation * 50.0;
    double confidence = dropScore + corrScore;

    // 置信度门槛：低于 50% 不输出结论
    if (confidence < 50.0) {
        return result;
    }

    result.detected = true;
    result.confidence = confidence;
    result.correlation = correlation;
    result.wifiRssiDrop = wifiDrop;
    result.btRssiDrop = btDrop;
    result.timestamp = std::chrono::system_clock::now();
    result.suggestion = generateSuggestion(result);

    LOG_INFO(LogModule::WEAK_MGR,
             "BandConflictDetector: conflict detected, confidence="
             << confidence << "%, correlation=" << correlation
             << ", wifiDrop=" << wifiDrop << "dBm, btDrop=" << btDrop << "dBm");

    return result;
}

// ============================================================================
// baseline
// ============================================================================

double BandConflictDetector::baseline(const std::deque<int>& h, size_t n) const {
    if (h.empty() || n == 0) {
        return 0.0;
    }

    // 取最近 n 个样本（若 n 超过队列大小，取全部）
    size_t count = std::min(n, h.size());
    double sum = 0.0;
    auto it = h.rbegin();
    for (size_t i = 0; i < count; ++i, ++it) {
        sum += *it;
    }

    return sum / static_cast<double>(count);
}

// ============================================================================
// pearson
// ============================================================================

double BandConflictDetector::pearson(const std::deque<int>& x,
                                         const std::deque<int>& y) const {
    // 取两者中较短的长度
    size_t n = std::min(x.size(), y.size());

    // 边界条件：样本不足
    if (n < MIN_SAMPLES) {
        return 0.0;
    }

    // 从队列末尾取最近 n 个样本（保持时间对齐）
    double sumX = 0.0, sumY = 0.0;
    double sumXY = 0.0, sumX2 = 0.0, sumY2 = 0.0;

    auto ix = x.rbegin();
    auto iy = y.rbegin();
    for (size_t i = 0; i < n; ++i, ++ix, ++iy) {
        double xi = static_cast<double>(*ix);
        double yi = static_cast<double>(*iy);
        sumX += xi;
        sumY += yi;
        sumXY += xi * yi;
        sumX2 += xi * xi;
        sumY2 += yi * yi;
    }

    // Pearson 公式：r = (n*Σxy - Σx*Σy) / sqrt((n*Σx²- (Σx)²) * (n*Σy²- (Σy)²))
    double numerator = n * sumXY - sumX * sumY;
    double denomX = n * sumX2 - sumX * sumX;
    double denomY = n * sumY2 - sumY * sumY;

    // 边界条件：分母为 0 或接近 0（所有样本值相同）时，无相关性
    if (denomX <= 0.0 || denomY <= 0.0) {
        return 0.0;
    }

    double denominator = std::sqrt(denomX * denomY);
    if (denominator < 1e-12) {
        return 0.0;
    }

    return numerator / denominator;
}

// ============================================================================
// generateSuggestion
// ============================================================================

std::string BandConflictDetector::generateSuggestion(const BandConflictResult& r) {
    if (!r.detected) {
        return "";
    }

    std::ostringstream oss;

    // 前缀 band_conflict 标识，供 NetworkQualityChanged 信号载荷识别事件类型
    // （spec Event Routing "频段冲突事件"：信号载荷含 "band_conflict" 标识）
    oss << "[band_conflict] ";

    if (r.wifiRssiDrop > 20 || r.btRssiDrop > 20) {
        oss << "检测到严重 2.4GHz 频段冲突（Wi-Fi降幅 " << r.wifiRssiDrop
            << "dBm，蓝牙降幅 " << r.btRssiDrop << "dBm）。建议："
            << "1) Wi-Fi 切换至 5GHz 频段；"
            << "2) 或调整 Wi-Fi 信道至 1/6/11 减少重叠；"
            << "3) 蓝牙设备靠近适配器减少发射功率。";
    } else {
        oss << "检测到轻度 2.4GHz 频段冲突（Wi-Fi降幅 " << r.wifiRssiDrop
            << "dBm，蓝牙降幅 " << r.btRssiDrop << "dBm）。"
            << "建议调整设备位置或 Wi-Fi 信道。";
    }

    return oss.str();
}

// ============================================================================
// reset
// ============================================================================

void BandConflictDetector::reset() {
    wifiHistory_.clear();
    btHistory_.clear();
}

}  // namespace weaknet_dbus
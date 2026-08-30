/**
 * @file bt_audio_fusion.cpp
 * @brief 蓝牙音频质量融合评分器 — 将 BlueZ D-Bus MediaTransport1 属性 + eBPF 内核态流量统计融合为单一质量分数
 *
 * 模块职责：
 *   - 接收 BtAudioTransport（来自 BlueZ MediaTransport1 D-Bus 接口）与 BtTrafficStats（来自 eBPF）
 *   - 基于 eBPF 累计流量增量（bytes_delta / gap_count / max_gap_ns）判断音频会话是否"真正活跃"
 *   - 检测疑似卡顿（gap_count 异常增长）与大间隔丢包（max_gap_ns 超过阈值）
 *   - 融合评分 = 基础评分(D-Bus Delay/Codec) × 有效活跃占比 + eBPF 修正量（流量加分 / 卡顿扣分）
 *   - eBPF 不可用时自动降级为纯 D-Bus 模式（evaluateDbOnly）
 *
 * 依赖的外部接口：
 *   - **bt_monitor.hpp**        协作关系：由 BtMonitor::getAudioFusionResult() 调用
 *   - **bt_audio_analyzer.hpp** 协作关系：其 BtTrafficStats（内核态累计流量）通过参数 stats/prevStats 传入
 *   - **bt_audio_fusion.hpp**   定义融合配置 BtAudioFusionConfig 与结果结构体 BtAudioFusionResult
 *
 * 音频融合评分算法：
 *   1. 非活跃（state != "active"）→ score=0, level=inactive
 *   2. eBPF 不可用 → evaluateDbOnly()：仅按 Delay（2000ms→-40, 1000ms→-20, 500ms→-10）+ SBC 编码（-5）扣分
 *   3. eBPF 可用 → evaluate()：
 *      a. 计算增量 bytesDelta/packetsDelta/gapDelta/maxGapDelta（保护 stats<prevStats 重置场景）
 *      b. effectiveActive = bytesDelta > config.minBytesPerSec
 *      c. suspectedStall  = gapDelta    > config.stallCountThreshold
 *      d. maxGapMs = maxGapDelta / 1e6
 *      e. 计算 ebpfCorrection：有效活跃加分 log2(bytes/1024)*3 (上限+20)；卡顿扣分 gapDelta*5 (上限-30)；maxGap>500ms 额外-10
 *      f. baseScore（Delay+Codec 扣分，同 D-Bus 逻辑）
 *      g. activeRatio = effectiveActive ? 1.0 : min(1.0, bytesDelta/minBytesPerSec)
 *      h. qualityScore = clamp(baseScore × activeRatio + ebpfCorrection, 0, 100)
 *      i. 映射 level：≥90 excellent, ≥70 good, ≥50 fair, ≥30 poor, 其他 unknown
 *
 * 边界条件：
 *   - prevStats==nullptr：首次采集，无法计算增量，bytesDelta=stats->bytes 直接使用
 *   - stats==nullptr：eBPF 已降级，走 evaluateDbOnly 纯 D-Bus 路径
 *   - transport.state != "active"：直接返回非活跃结果
 */

#include "bt_audio_fusion.hpp"
#include "bt_monitor.hpp"
#include "bt_audio_analyzer.hpp"
#include "logger.hpp"
#include <algorithm>
#include <cmath>
#include <sstream>

namespace weaknet_dbus {

// ============================================================================
// 构造与配置
// ============================================================================

BtAudioFusion::BtAudioFusion() = default;

BtAudioFusion::BtAudioFusion(const BtAudioFusionConfig& config)
    : config_(config) {}

void BtAudioFusion::setConfig(const BtAudioFusionConfig& config) {
    config_ = config;
}

BtAudioFusionConfig BtAudioFusion::config() const {
    return config_;
}

// ============================================================================
// 融合评估
// ============================================================================

BtAudioFusionResult BtAudioFusion::evaluate(
        const BtAudioTransport& transport,
        const BtTrafficStats* stats,
        const BtTrafficStats* prevStats,
        bool ebpfAvailable) const {

    BtAudioFusionResult result;
    result.deviceMac = transport.deviceMac;
    result.isActive = (transport.state == "active");
    result.ebpfAvailable = ebpfAvailable;

    LOG_INFO(LogModule::BLUETOOTH, "evaluate: device=" << transport.deviceMac << " state=" << transport.state << " ebpf=" << ebpfAvailable);

    // 非活跃状态：直接返回最低评分
    if (!result.isActive) {
        result.effectiveActive = false;
        result.suspectedStall = false;
        result.qualityScore = 0.0;
        result.level = "inactive";
        result.activeRatio = 0.0;
        result.ebpfCorrection = 0.0;
        result.diagnostic = "Transport not active (state=" + transport.state + ")";
        return result;
    }

    // eBPF 不可用：走纯 D-Bus 路径
    if (!ebpfAvailable || !stats) {
        return evaluateDbOnly(transport);
    }

    // ---- eBPF 融合路径 ----

    // 1. 计算增量（若有历史数据）
    uint64_t bytesDelta = stats->bytes;
    uint64_t packetsDelta = stats->packets;
    uint64_t gapDelta = stats->gapCount;
    // maxGapNs 是当前窗口观察到的最大包间隔（非累积值），直接使用当前值
    uint64_t maxGapDelta = stats->maxGapNs;

    if (prevStats && prevStats->packets > 0) {
        // 防止重置后 prev > current 的情况
        if (stats->bytes >= prevStats->bytes) {
            bytesDelta = stats->bytes - prevStats->bytes;
        }
        if (stats->packets >= prevStats->packets) {
            packetsDelta = stats->packets - prevStats->packets;
        }
        if (stats->gapCount >= prevStats->gapCount) {
            gapDelta = stats->gapCount - prevStats->gapCount;
        }
        // maxGapNs 是当前窗口的最大值，无需与前次比较
    }

    // 2. 判断有效活跃
    //    有效活跃 = 有流量通过（bytesDelta > 阈值）
    result.effectiveActive = (bytesDelta > config_.minBytesPerSec);

    // 3. 判断疑似卡顿
    //    卡顿条件：包间隔 > stallThresholdMs 的次数超过阈值
    result.suspectedStall = (gapDelta > config_.stallCountThreshold);
    if (result.suspectedStall) {
        LOG_WARNING(LogModule::BLUETOOTH, "evaluate: stall suspected for " << transport.deviceMac << " gap_count=" << gapDelta);
    }

    // 4. 计算最大包间隔（毫秒）
    result.maxGapMs = maxGapDelta / 1000000ULL;  // ns → ms

    // 5. 估算字节速率（bytes/s）
    //    注：窗口大小由上层控制，此处不做时间归一化，由上层传入间隔
    result.bytesPerSec = bytesDelta;

    // 6. 计算 eBPF 修正量
    //    有效活跃：加分（基于 bytesDelta 的规模）
    //    疑似卡顿：扣分（基于 gapDelta 的严重程度）
    double correction = 0.0;
    if (result.effectiveActive && bytesDelta > 0) {
        // 流量越大，修正加分越多（上限 +20）
        double trafficScore = std::min(20.0, std::log2(static_cast<double>(bytesDelta) / 1024.0) * 3.0);
        correction += trafficScore;
    }
    if (result.suspectedStall) {
        // 卡顿次数越多，扣分越多（上限 -30）
        double stallPenalty = std::min(30.0, static_cast<double>(gapDelta) * 5.0);
        correction -= stallPenalty;
    }
    // 最大包间隔过大：额外扣分
    if (result.maxGapMs > 500) {
        correction -= 10.0;
    } else if (result.maxGapMs > 300) {
        correction -= 5.0;
    }

    result.ebpfCorrection = correction;

    // 7. 计算基础评分（复用 Phase 1b 逻辑）
    double baseScore = 100.0;
    if (transport.delay > 2000) {
        baseScore -= 40.0;
    } else if (transport.delay > 1000) {
        baseScore -= 20.0;
    } else if (transport.delay > 500) {
        baseScore -= 10.0;
    }
    if (transport.codec == 0x00) {
        baseScore -= 5.0;  // SBC 扣分
    }

    // 8. 计算有效活跃占比
    //    简化：若有效活跃，占比为 1.0；否则根据流量比例估算
    result.activeRatio = result.effectiveActive ? 1.0
                         : std::min(1.0, static_cast<double>(bytesDelta) / static_cast<double>(config_.minBytesPerSec));

    // 9. 融合评分 = 基础评分 × 有效活跃占比 + eBPF 修正
    result.qualityScore = baseScore * result.activeRatio + correction;
    result.qualityScore = std::max(0.0, std::min(100.0, result.qualityScore));

    // 10. 生成等级
    result.level = scoreToLevel(result.qualityScore);

    // 11. 生成诊断信息
    std::ostringstream diag;
    diag << "eBPF mode: ";
    if (result.effectiveActive) {
        diag << "effective active, bytes_delta=" << bytesDelta
             << "B, packets_delta=" << packetsDelta;
    } else {
        diag << "inactive, bytes_delta=" << bytesDelta << "B (threshold="
             << config_.minBytesPerSec << "B/s)";
    }
    if (result.suspectedStall) {
        diag << ", STALL SUSPECTED (gap_count=" << gapDelta
             << ", max_gap=" << result.maxGapMs << "ms)";
    }
    diag << ", correction=" << (correction >= 0 ? "+" : "") << correction;
    result.diagnostic = diag.str();

    return result;
}

BtAudioFusionResult BtAudioFusion::evaluateDbOnly(const BtAudioTransport& transport) const {
    BtAudioFusionResult result;
    result.deviceMac = transport.deviceMac;
    result.isActive = (transport.state == "active");
    result.effectiveActive = result.isActive;  // 无 eBPF 时信任 D-Bus
    result.suspectedStall = false;
    result.ebpfAvailable = false;
    result.ebpfCorrection = 0.0;
    result.maxGapMs = 0;
    result.bytesPerSec = 0;

    if (!result.isActive) {
        result.qualityScore = 0.0;
        result.level = "inactive";
        result.activeRatio = 0.0;
        result.diagnostic = "Transport not active (state=" + transport.state + ")";
        return result;
    }

    // 纯 D-Bus 评分（与 Phase 1b 逻辑一致）
    double score = 100.0;
    if (transport.delay > 2000) {
        score -= 40.0;
    } else if (transport.delay > 1000) {
        score -= 20.0;
    } else if (transport.delay > 500) {
        score -= 10.0;
    }
    if (transport.codec == 0x00) {
        score -= 5.0;
    }

    result.qualityScore = std::max(0.0, score);
    result.level = scoreToLevel(result.qualityScore);
    result.activeRatio = 1.0;  // 无法精确计算，保守设为 1.0
    result.diagnostic = "D-Bus only mode (eBPF unavailable)";
    return result;
}

// ============================================================================
// 辅助方法
// ============================================================================

BtAudioQuality BtAudioFusion::toLegacyQuality(const BtAudioFusionResult& result) {
    BtAudioQuality quality;
    quality.deviceMac = result.deviceMac;
    quality.isActive = result.isActive;
    quality.qualityScore = result.qualityScore;
    quality.level = result.level;
    quality.activeRatio = result.activeRatio;

    // 将融合诊断信息转为 issue 列表
    if (result.suspectedStall) {
        quality.issues.push_back("Suspected audio stall (eBPF gap count > threshold)");
    }
    if (!result.effectiveActive && result.isActive) {
        quality.issues.push_back("D-Bus active but no actual traffic (possible stall)");
    }
    if (result.maxGapMs > 300) {
        quality.issues.push_back("Large packet gap: " + std::to_string(result.maxGapMs) + "ms");
    }
    if (!result.ebpfAvailable) {
        quality.issues.push_back("eBPF unavailable, using D-Bus only");
    }

    return quality;
}

std::string BtAudioFusion::scoreToLevel(double score) {
    if (score >= 90.0) return "excellent";
    if (score >= 70.0) return "good";
    if (score >= 50.0) return "fair";
    if (score >= 30.0) return "poor";
    return "unknown";
}

}  // namespace weaknet_dbus
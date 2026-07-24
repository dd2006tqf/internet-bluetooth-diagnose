// bt_audio_fusion.hpp
// 蓝牙音频质量融合层 — D-Bus 状态 + eBPF 流量统计的融合评估
//
// 设计原理：
//   BlueZ D-Bus 的 State=active 仅表示 A2DP 传输会话被获取，不保证音频数据在真正流动。
//   例如：播放器暂停、缓冲区欠载、蓝牙链路拥塞时，D-Bus 仍然报告 active。
//
//   融合层通过叠加 eBPF 内核态流量统计来纠正这个问题：
//   - 有效活跃 = D-Bus state==active && eBPF bytes_delta > 阈值
//   - 疑似卡顿 = D-Bus state==active && eBPF gap > 卡顿阈值
//   - 融合评分 = 基础评分 × 有效活跃占比
//
// 降级策略：
//   - eBPF 不可用时，仅基于 D-Bus 数据评分（退化为 Phase 1b 逻辑）
//   - 评分公式保持一致，但缺少 eBPF 修正项

#pragma once

#include <string>
#include <cstdint>
#include <chrono>

namespace weaknet_dbus {

// 前置声明
struct BtAudioTransport;
struct BtTrafficStats;
struct BtAudioQuality;

// ============================================================================
// 融合评估结果（升级版 BtAudioQuality）
// ============================================================================
struct BtAudioFusionResult {
    std::string deviceMac;
    bool isActive = false;               // D-Bus 报告的 active 状态
    bool effectiveActive = false;        // 融合后的有效活跃（有流量才计为活跃）
    bool suspectedStall = false;         // 疑似卡顿（active 但流量异常）
    double qualityScore = 0.0;           // 融合评分 (0-100)
    std::string level;                   // 质量等级
    double activeRatio = 0.0;            // 有效活跃占比 = effectiveDuration / bluezActiveDuration
    double ebpfCorrection = 0.0;         // eBPF 修正量（正数=加分，负数=扣分）
    uint64_t bytesPerSec = 0;            // 估算的音频字节速率
    uint64_t maxGapMs = 0;               // 最大包间隔（毫秒）
    bool ebpfAvailable = false;          // eBPF 是否可用
    std::string diagnostic;              // 诊断信息
};

// ============================================================================
// 融合层配置
// ============================================================================
struct BtAudioFusionConfig {
    // 音频数据流阈值：每秒最低字节数，低于此值视为无有效音频流
    // A2DP SBC 典型码率: 328kbps ≈ 41KB/s，设置合理下限 5KB/s
    uint64_t minBytesPerSec = 5120;      // 5 KB/s

    // 卡顿检测阈值：包间隔超过此值视为一次卡顿（毫秒）
    uint64_t stallThresholdMs = 200;     // 200ms

    // 卡顿计数阈值：窗口内卡顿次数超过此值标记为疑似卡顿
    uint64_t stallCountThreshold = 3;

    // 有效活跃占比阈值：低于此值认为播放质量差
    double effectiveRatioThreshold = 0.7; // 70%
};

// ============================================================================
// 蓝牙音频融合评估器
// ============================================================================
class BtAudioFusion {
public:
    BtAudioFusion();
    explicit BtAudioFusion(const BtAudioFusionConfig& config);

    // ---- 配置 ----

    void setConfig(const BtAudioFusionConfig& config);
    BtAudioFusionConfig config() const;

    // ---- 融合评估 ----

    // 基于 D-Bus Transport 状态 + eBPF 流量统计进行融合评估
    //
    // @param transport  D-Bus 采集的音频传输状态（当前快照）
    // @param stats      eBPF 采集的流量统计（当前快照，累计值）
    // @param prevStats  上一次采集的流量统计（用于增量计算），若为 nullptr 则仅做绝对评估
    // @param ebpfAvailable eBPF 是否可用
    // @return 融合评估结果
    BtAudioFusionResult evaluate(const BtAudioTransport& transport,
                                  const BtTrafficStats* stats,
                                  const BtTrafficStats* prevStats,
                                  bool ebpfAvailable) const;

    // 简化版：仅基于 D-Bus 数据评估（eBPF 不可用时使用）
    // @param transport  D-Bus 采集的音频传输状态
    // @return 融合评估结果（ebpfAvailable=false，无 eBPF 修正）
    BtAudioFusionResult evaluateDbOnly(const BtAudioTransport& transport) const;

    // ---- 辅助 ----

    // 将融合结果转换为 Phase 1b 兼容的 BtAudioQuality（向后兼容）
    static BtAudioQuality toLegacyQuality(const BtAudioFusionResult& result);

    // 数值评分 → 等级字符串
    static std::string scoreToLevel(double score);

private:
    BtAudioFusionConfig config_;
};

}  // namespace weaknet_dbus
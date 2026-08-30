/**
 * @file jitter_monitor.hpp
 * @brief 网络抖动（Jitter）监控线程启动接口
 *
 * 抖动 = RTT 样本标准差，反映延迟稳定性。对 VoIP/视频会议等实时业务，
 * 抖动比平均延迟更能代表体验质量。
 *
 * 算法：滑动窗口（默认 30 样本）→ 计算均值 → 平方差均值 → sqrt
 * 结果写入 ServerContext::NetInfo::jitter_ms_（哨兵值 -1.0 表示无效）。
 *
 * @note 依赖与 rtt_monitor 相同的原始 socket 权限。
 */

#pragma once

#include <string>

namespace weaknet_dbus {

class ServerContext;

/**
 * @brief 创建并启动网络抖动监控线程
 *
 * @param ctx         ServerContext 生命周期句柄
 * @param host        目标主机（如 "223.5.5.5"）
 * @param intervalMs  采样间隔，默认 2000ms
 * @param timeoutMs   单次 ping 超时，默认 800ms
 * @param windowSize  滑动窗口样本数，默认 30；值越大抖动计算越平滑但响应越慢
 */
void start_jitter_monitor_thread(ServerContext* ctx,
                                 const std::string& host,
                                 int intervalMs = 2000,
                                 int timeoutMs = 800,
                                 int windowSize = 30);

}  // namespace weaknet_dbus
